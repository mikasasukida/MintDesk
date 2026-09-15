#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>

#include <nvEncodeAPI.h>

class NvEncoder {
public:
    NvEncoder() = default;
    ~NvEncoder();

    NvEncoder(const NvEncoder&) = delete;
    NvEncoder& operator=(const NvEncoder&) = delete;

    bool initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps = 60);
    bool encodeFrame(ID3D11Texture2D* texture);
    bool encodeFrame(ID3D11Texture2D* texture, std::vector<uint8_t>& bitstream);
    void requestKeyFrame();
    void shutdown();

    uint32_t lastBitstreamSize() const { return lastBitstreamSize_; }

private:
    bool loadApi();
    bool openSession(ID3D11Device* device);
    bool initializeEncoder();
    void printStatus(const char* message, NVENCSTATUS status) const;

    HMODULE nvencDll_ = nullptr;
    void* session_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    NV_ENC_CONFIG encodeConfig_{};
    NV_ENC_OUTPUT_PTR bitstreamBuffer_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 60;
    uint32_t frameIndex_ = 0;
    uint32_t lastBitstreamSize_ = 0;
    bool forceKeyFrame_ = true;
};
