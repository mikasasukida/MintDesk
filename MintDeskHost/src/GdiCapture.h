#pragma once

#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <vector>

class GdiCapture {
public:
    GdiCapture() = default;
    ~GdiCapture();

    GdiCapture(const GdiCapture&) = delete;
    GdiCapture& operator=(const GdiCapture&) = delete;

    bool initialize(ID3D11Device* device, uint32_t& width, uint32_t& height);
    bool captureFrame(ID3D11Texture2D** texture);
    void shutdown();

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    HDC screenDc_ = nullptr;
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int x_ = 0;
    int y_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
};
