#include "VideoConverter.h"

#include <dxgi.h>

#include <iostream>

namespace {
template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}
}

VideoConverter::~VideoConverter() {
    shutdown();
}

bool VideoConverter::initialize(
    ID3D11Device* device,
    UINT inputWidth,
    UINT inputHeight,
    UINT outputWidth,
    UINT outputHeight
) {
    if (!device || inputWidth == 0 || inputHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        return false;
    }

    device_ = device;
    inputWidth_ = inputWidth;
    inputHeight_ = inputHeight;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    device_->AddRef();
    device_->GetImmediateContext(&context_);

    HRESULT hr = device_->QueryInterface(
        __uuidof(ID3D11VideoDevice),
        reinterpret_cast<void**>(&videoDevice_)
    );

    if (FAILED(hr)) {
        std::cerr << "QueryInterface ID3D11VideoDevice failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = context_->QueryInterface(
        __uuidof(ID3D11VideoContext),
        reinterpret_cast<void**>(&videoContext_)
    );

    if (FAILED(hr)) {
        std::cerr << "QueryInterface ID3D11VideoContext failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inputWidth_;
    contentDesc.InputHeight = inputHeight_;
    contentDesc.OutputWidth = outputWidth_;
    contentDesc.OutputHeight = outputHeight_;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(
        &contentDesc,
        &enumerator_
    );

    if (FAILED(hr)) {
        std::cerr << "CreateVideoProcessorEnumerator failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(
        enumerator_,
        0,
        &processor_
    );

    if (FAILED(hr)) {
        std::cerr << "CreateVideoProcessor failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = outputWidth_;
    textureDesc.Height = outputHeight_;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&textureDesc, nullptr, &nv12Texture_);

    if (FAILED(hr)) {
        std::cerr << "Create NV12 texture failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    std::cout
        << "VideoConverter initialized: BGRA -> NV12 "
        << inputWidth_
        << "x"
        << inputHeight_
        << " -> "
        << outputWidth_
        << "x"
        << outputHeight_
        << "\n";

    return true;
}

bool VideoConverter::convert(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** nv12Texture) {
    if (!bgraTexture || !nv12Texture || !videoDevice_ || !videoContext_ || !processor_ || !nv12Texture_) {
        return false;
    }

    *nv12Texture = nullptr;

    D3D11_TEXTURE2D_DESC inputDesc{};
    bgraTexture->GetDesc(&inputDesc);

    if (inputDesc.Width != inputWidth_ || inputDesc.Height != inputHeight_) {
        std::cerr << "VideoConverter input size mismatch.\n";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;
    inputViewDesc.Texture2D.ArraySlice = 0;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        bgraTexture,
        enumerator_,
        &inputViewDesc,
        &inputView
    );

    if (FAILED(hr)) {
        std::cerr << "CreateVideoProcessorInputView failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorOutputView* outputView = nullptr;
    hr = videoDevice_->CreateVideoProcessorOutputView(
        nv12Texture_,
        enumerator_,
        &outputViewDesc,
        &outputView
    );

    if (FAILED(hr)) {
        SafeRelease(inputView);
        std::cerr << "CreateVideoProcessorOutputView failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    RECT sourceRect{
        0,
        0,
        static_cast<LONG>(inputWidth_),
        static_cast<LONG>(inputHeight_)
    };

    RECT outputRect{
        0,
        0,
        static_cast<LONG>(outputWidth_),
        static_cast<LONG>(outputHeight_)
    };

    videoContext_->VideoProcessorSetStreamSourceRect(processor_, 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(processor_, 0, TRUE, &outputRect);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_, TRUE, &outputRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = inputView;

    hr = videoContext_->VideoProcessorBlt(
        processor_,
        outputView,
        0,
        1,
        &stream
    );

    SafeRelease(outputView);
    SafeRelease(inputView);

    if (FAILED(hr)) {
        std::cerr << "VideoProcessorBlt failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    nv12Texture_->AddRef();
    *nv12Texture = nv12Texture_;
    return true;
}

void VideoConverter::shutdown() {
    SafeRelease(nv12Texture_);
    SafeRelease(processor_);
    SafeRelease(enumerator_);
    SafeRelease(videoContext_);
    SafeRelease(videoDevice_);
    SafeRelease(context_);
    SafeRelease(device_);
    inputWidth_ = 0;
    inputHeight_ = 0;
    outputWidth_ = 0;
    outputHeight_ = 0;
}
