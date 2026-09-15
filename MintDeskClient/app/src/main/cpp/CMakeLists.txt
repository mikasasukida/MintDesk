cmake_minimum_required(VERSION 3.22.1)

project(mintdesk_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GameNetworkingSockets CONFIG REQUIRED)

set(VCPKG_ARM64_ANDROID_LIB_DIR "D:/3C++/tools/vcpkg/installed/arm64-android/lib")
file(GLOB ABSL_LIBS "${VCPKG_ARM64_ANDROID_LIB_DIR}/libabsl_*.a")
set(PROTOBUF_EXTRA_LIBS
        "${VCPKG_ARM64_ANDROID_LIB_DIR}/libutf8_range.a"
        "${VCPKG_ARM64_ANDROID_LIB_DIR}/libutf8_validity.a"
)

add_library(mintdesk_native SHARED
        native_bridge.cpp
)

target_link_libraries(mintdesk_native
        PRIVATE
        GameNetworkingSockets::GameNetworkingSockets_s
        android
        log
        ${PROTOBUF_EXTRA_LIBS}
        ${ABSL_LIBS}
)
