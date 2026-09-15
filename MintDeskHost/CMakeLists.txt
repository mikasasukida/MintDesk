cmake_minimum_required(VERSION 3.20)

project(MintDeskHost)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NV_CODEC_SDK
        "D:/Video_Codec_Interface_13.1.15"
)

add_executable(MintDeskHost
        src/main.cpp
        src/NvEncoder.cpp
        src/TcpServer.cpp
        src/InputController.cpp
        src/VideoConverter.cpp
        src/GdiCapture.cpp
        src/MintDeskHost.rc
)

target_include_directories(MintDeskHost
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${NV_CODEC_SDK}/Interface"
)

target_link_libraries(MintDeskHost
        d3d11
        dxgi
        gdi32
        user32
        ws2_32
)
