#include "NvEncoder.h"

#include <cstring>
#include <iostream>

namespace {
using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(
    NV_ENCODE_API_FUNCTION_LIST* functionList
);

constexpr uint32_t kFrameRateDen = 1;
constexpr uint32_t kTargetBitrate = 15'000'000;
}

NvEncoder::~NvEncoder() {
    shutdown();
}

bool NvEncoder::initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps) {
    if (!device || width == 0 || height == 0) {
        std::cerr << "NvEncoder::initialize received invalid arguments.\n";
        return false;
    }

    width_ = width;
    height_ = height;
    fps_ = fps == 0 ? 60 : fps;

    if (!loadApi()) {
        return false;
    }

    if (!openSession(device)) {
        return false;
    }

    if (!initializeEncoder()) {
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    NVENCSTATUS status = api_.nvEncCreateBitstreamBuffer(session_, &bitstream);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncCreateBitstreamBuffer failed", status);
        return false;
    }

    bitstreamBuffer_ = bitstream.bitstreamBuffer;

    std::cout
        << "NVENC encoder initialized: H264 "
        << width_
        << "x"
        << height_
        << " @ "
        << fps_
        << " fps\n";

    return true;
}

bool NvEncoder::encodeFrame(ID3D11Texture2D* texture) {
    std::vector<uint8_t> bitstream;
    return encodeFrame(texture, bitstream);
}

bool NvEncoder::encodeFrame(ID3D11Texture2D* texture, std::vector<uint8_t>& bitstream) {
    bitstream.clear();

    if (!session_ || !texture || !bitstreamBuffer_) {
        std::cerr << "NvEncoder::encodeFrame called before initialization.\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    if (desc.Width != width_ || desc.Height != height_ || desc.Format != DXGI_FORMAT_NV12) {
        std::cerr << "encodeFrame expects a "
                  << width_
                  << "x"
                  << height_
                  << " NV12 texture.\n";
        return false;
    }

    NV_ENC_REGISTER_RESOURCE registerResource{};
    registerResource.version = NV_ENC_REGISTER_RESOURCE_VER;
    registerResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registerResource.width = width_;
    registerResource.height = height_;
    registerResource.pitch = 0;
    registerResource.subResourceIndex = 0;
    registerResource.resourceToRegister = texture;
    registerResource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS status = api_.nvEncRegisterResource(session_, &registerResource);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncRegisterResource failed", status);
        return false;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapInput{};
    mapInput.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapInput.registeredResource = registerResource.registeredResource;

    status = api_.nvEncMapInputResource(session_, &mapInput);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncMapInputResource failed", status);
        api_.nvEncUnregisterResource(session_, registerResource.registeredResource);
        return false;
    }

    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputWidth = width_;
    picParams.inputHeight = height_;
    picParams.inputPitch = width_;
    picParams.encodePicFlags = (forceKeyFrame_ || frameIndex_ == 0)
        ? (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS)
        : 0;
    picParams.frameIdx = frameIndex_;
    picParams.inputTimeStamp = frameIndex_;
    picParams.inputDuration = 1;
    picParams.inputBuffer = mapInput.mappedResource;
    picParams.outputBitstream = bitstreamBuffer_;
    picParams.bufferFmt = mapInput.mappedBufferFmt;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    status = api_.nvEncEncodePicture(session_, &picParams);

    if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
        std::cout << "nvEncEncodePicture needs more input before output.\n";
        api_.nvEncUnmapInputResource(session_, mapInput.mappedResource);
        api_.nvEncUnregisterResource(session_, registerResource.registeredResource);
        ++frameIndex_;
        return true;
    }

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncEncodePicture failed", status);
        api_.nvEncUnmapInputResource(session_, mapInput.mappedResource);
        api_.nvEncUnregisterResource(session_, registerResource.registeredResource);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lockBitstream{};
    lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBitstream.outputBitstream = bitstreamBuffer_;

    status = api_.nvEncLockBitstream(session_, &lockBitstream);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncLockBitstream failed", status);
        api_.nvEncUnmapInputResource(session_, mapInput.mappedResource);
        api_.nvEncUnregisterResource(session_, registerResource.registeredResource);
        return false;
    }

    lastBitstreamSize_ = lockBitstream.bitstreamSizeInBytes;

    auto* data = static_cast<uint8_t*>(lockBitstream.bitstreamBufferPtr);

    if (data && lastBitstreamSize_ > 0) {
        bitstream.assign(data, data + lastBitstreamSize_);
    }

    status = api_.nvEncUnlockBitstream(session_, bitstreamBuffer_);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncUnlockBitstream failed", status);
    }

    status = api_.nvEncUnmapInputResource(session_, mapInput.mappedResource);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncUnmapInputResource failed", status);
    }

    status = api_.nvEncUnregisterResource(session_, registerResource.registeredResource);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncUnregisterResource failed", status);
    }

    ++frameIndex_;
    forceKeyFrame_ = false;
    return lastBitstreamSize_ > 0;
}

void NvEncoder::requestKeyFrame() {
    forceKeyFrame_ = true;
}

void NvEncoder::shutdown() {
    if (session_ && bitstreamBuffer_) {
        NVENCSTATUS status = api_.nvEncDestroyBitstreamBuffer(session_, bitstreamBuffer_);

        if (status != NV_ENC_SUCCESS) {
            printStatus("nvEncDestroyBitstreamBuffer failed", status);
        }

        bitstreamBuffer_ = nullptr;
    }

    if (session_) {
        NVENCSTATUS status = api_.nvEncDestroyEncoder(session_);

        if (status != NV_ENC_SUCCESS) {
            printStatus("nvEncDestroyEncoder failed", status);
        }

        session_ = nullptr;
    }

    if (nvencDll_) {
        FreeLibrary(nvencDll_);
        nvencDll_ = nullptr;
    }

    std::memset(&api_, 0, sizeof(api_));
    std::memset(&encodeConfig_, 0, sizeof(encodeConfig_));
    width_ = 0;
    height_ = 0;
    fps_ = 60;
    frameIndex_ = 0;
    lastBitstreamSize_ = 0;
    forceKeyFrame_ = true;
}

bool NvEncoder::loadApi() {
    nvencDll_ = LoadLibraryW(L"nvEncodeAPI64.dll");

    if (!nvencDll_) {
        std::cerr << "Failed to load nvEncodeAPI64.dll\n";
        return false;
    }

    auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
        GetProcAddress(nvencDll_, "NvEncodeAPICreateInstance")
    );

    if (!createInstance) {
        std::cerr << "NvEncodeAPICreateInstance not found.\n";
        return false;
    }

    api_ = {};
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = createInstance(&api_);

    if (status != NV_ENC_SUCCESS) {
        printStatus("NvEncodeAPICreateInstance failed", status);
        return false;
    }

    return true;
}

bool NvEncoder::openSession(ID3D11Device* device) {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams{};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = device;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = api_.nvEncOpenEncodeSessionEx(&sessionParams, &session_);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncOpenEncodeSessionEx failed", status);
        return false;
    }

    return true;
}

bool NvEncoder::initializeEncoder() {
    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS status = api_.nvEncGetEncodePresetConfigEx(
        session_,
        NV_ENC_CODEC_H264_GUID,
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        &presetConfig
    );

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncGetEncodePresetConfigEx failed", status);
        return false;
    }

    encodeConfig_ = presetConfig.presetCfg;
    encodeConfig_.version = NV_ENC_CONFIG_VER;
    encodeConfig_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    encodeConfig_.gopLength = fps_;
    encodeConfig_.frameIntervalP = 1;
    encodeConfig_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encodeConfig_.mvPrecision = NV_ENC_MV_PRECISION_DEFAULT;
    encodeConfig_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encodeConfig_.rcParams.averageBitRate = kTargetBitrate;
    encodeConfig_.rcParams.maxBitRate = kTargetBitrate;
    encodeConfig_.rcParams.vbvBufferSize = kTargetBitrate / fps_;
    encodeConfig_.rcParams.vbvInitialDelay = encodeConfig_.rcParams.vbvBufferSize;
    encodeConfig_.rcParams.enableLookahead = 0;
    encodeConfig_.rcParams.lookaheadDepth = 0;
    encodeConfig_.rcParams.zeroReorderDelay = 1;
    encodeConfig_.rcParams.enableAQ = 0;
    encodeConfig_.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
    encodeConfig_.encodeCodecConfig.h264Config.idrPeriod = fps_;
    encodeConfig_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = width_;
    initParams.encodeHeight = height_;
    initParams.darWidth = width_;
    initParams.darHeight = height_;
    initParams.frameRateNum = fps_;
    initParams.frameRateDen = kFrameRateDen;
    initParams.enableEncodeAsync = 0;
    initParams.enablePTD = 1;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initParams.maxEncodeWidth = width_;
    initParams.maxEncodeHeight = height_;
    initParams.encodeConfig = &encodeConfig_;

    status = api_.nvEncInitializeEncoder(session_, &initParams);

    if (status != NV_ENC_SUCCESS) {
        printStatus("nvEncInitializeEncoder failed", status);
        return false;
    }

    return true;
}

void NvEncoder::printStatus(const char* message, NVENCSTATUS status) const {
    std::cerr << message << ": " << status;

    if (session_ && api_.nvEncGetLastErrorString) {
        const char* lastError = api_.nvEncGetLastErrorString(session_);

        if (lastError && lastError[0] != '\0') {
            std::cerr << " (" << lastError << ")";
        }
    }

    std::cerr << "\n";
}
