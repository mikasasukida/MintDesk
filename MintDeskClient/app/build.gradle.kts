plugins {
    id("com.android.application")
}

android {
    namespace = "com.mintdesk.client"
    compileSdk = 37
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.mintdesk.client"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DCMAKE_PREFIX_PATH=D:/3C++/tools/vcpkg/installed/arm64-android",
                    "-DGameNetworkingSockets_DIR=D:/3C++/tools/vcpkg/installed/arm64-android/share/gamenetworkingsockets",
                    "-DProtobuf_INCLUDE_DIR=D:/3C++/tools/vcpkg/installed/arm64-android/include",
                    "-DProtobuf_LIBRARY=D:/3C++/tools/vcpkg/installed/arm64-android/lib/libprotobuf.a",
                    "-DOPENSSL_ROOT_DIR=D:/3C++/tools/vcpkg/installed/arm64-android",
                    "-DOPENSSL_INCLUDE_DIR=D:/3C++/tools/vcpkg/installed/arm64-android/include",
                    "-DOPENSSL_SSL_LIBRARY=D:/3C++/tools/vcpkg/installed/arm64-android/lib/libssl.a",
                    "-DOPENSSL_CRYPTO_LIBRARY=D:/3C++/tools/vcpkg/installed/arm64-android/lib/libcrypto.a",
                    "-DANDROID_STL=c++_shared"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}
