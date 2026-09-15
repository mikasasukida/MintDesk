#pragma once

#include <d3d11.h>

class VideoConverter {
public:
    VideoConverter() = default;
    ~VideoConverter();

    VideoConverter(const VideoConverter&) = delete;
    VideoConverter& operator=(const VideoConverter&) = delete;

    bool initialize(
        ID3D11Device* device,
        UINT inputWidth,
        UINT inputHeight,
        UINT outputWidth,
        UINT outputHeight
    );
    bool convert(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** nv12Texture);
    void shutdown();

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* enumerator_ = nullptr;
    ID3D11VideoProcessor* processor_ = nullptr;
    ID3D11Texture2D* nv12Texture_ = nullptr;
    UINT inputWidth_ = 0;
    UINT inputHeight_ = 0;
    UINT outputWidth_ = 0;
    UINT outputHeight_ = 0;
};
