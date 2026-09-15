#include "GdiCapture.h"

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

GdiCapture::~GdiCapture() {
    shutdown();
}

bool GdiCapture::initialize(ID3D11Device* device, uint32_t& width, uint32_t& height) {
    if (!device) {
        return false;
    }

    x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    width_ = static_cast<uint32_t>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
    height_ = static_cast<uint32_t>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
    stride_ = width_ * 4;

    if (width_ == 0 || height_ == 0) {
        std::cerr << "GDI capture found empty virtual screen.\n";
        return false;
    }

    device_ = device;
    device_->AddRef();
    device_->GetImmediateContext(&context_);

    screenDc_ = GetDC(nullptr);
    if (!screenDc_) {
        std::cerr << "GetDC(nullptr) failed.\n";
        return false;
    }

    memoryDc_ = CreateCompatibleDC(screenDc_);
    if (!memoryDc_) {
        std::cerr << "CreateCompatibleDC failed.\n";
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width_);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height_);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    bitmap_ = CreateDIBSection(
        memoryDc_,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &bits_,
        nullptr,
        0
    );

    if (!bitmap_ || !bits_) {
        std::cerr << "CreateDIBSection failed.\n";
        return false;
    }

    oldBitmap_ = SelectObject(memoryDc_, bitmap_);

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width_;
    textureDesc.Height = height_;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &texture_);
    if (FAILED(hr)) {
        std::cerr << "Create GDI upload texture failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    width = width_;
    height = height_;

    std::cout
        << "GDI capture initialized: "
        << width_
        << "x"
        << height_
        << "\n";

    return true;
}

bool GdiCapture::captureFrame(ID3D11Texture2D** texture) {
    if (!texture || !screenDc_ || !memoryDc_ || !bits_ || !texture_ || !context_) {
        return false;
    }

    *texture = nullptr;

    BOOL ok = BitBlt(
        memoryDc_,
        0,
        0,
        static_cast<int>(width_),
        static_cast<int>(height_),
        screenDc_,
        x_,
        y_,
        SRCCOPY | CAPTUREBLT
    );

    if (!ok) {
        DWORD firstError = GetLastError();
        ok = BitBlt(
            memoryDc_,
            0,
            0,
            static_cast<int>(width_),
            static_cast<int>(height_),
            screenDc_,
            x_,
            y_,
            SRCCOPY
        );

        if (!ok) {
            std::cerr << "BitBlt failed: "
                      << firstError
                      << ", retry without CAPTUREBLT failed: "
                      << GetLastError()
                      << "\n";
            return false;
        }
    }

    context_->UpdateSubresource(
        texture_,
        0,
        nullptr,
        bits_,
        stride_,
        0
    );

    texture_->AddRef();
    *texture = texture_;
    return true;
}

void GdiCapture::shutdown() {
    SafeRelease(texture_);
    SafeRelease(context_);
    SafeRelease(device_);

    if (memoryDc_ && oldBitmap_) {
        SelectObject(memoryDc_, oldBitmap_);
        oldBitmap_ = nullptr;
    }

    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        bits_ = nullptr;
    }

    if (memoryDc_) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }

    if (screenDc_) {
        ReleaseDC(nullptr, screenDc_);
        screenDc_ = nullptr;
    }

    x_ = 0;
    y_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
}
