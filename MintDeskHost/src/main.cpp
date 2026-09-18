#include "TcpServer.h"
#include "NvEncoder.h"
#include "VideoConverter.h"
#include "GdiCapture.h"
#include "InputController.h"
#include "ClipboardSyncServer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::atomic_bool g_running{true};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }

    return FALSE;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

struct HostConfig {
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t fps = 60;
    std::string resolution = "native";
};

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::pair<uint32_t, uint32_t> ParseResolution(
    const std::string& value,
    uint32_t captureWidth,
    uint32_t captureHeight
) {
    if (value.empty() || value == "native") {
        return {captureWidth, captureHeight};
    }

    const auto xPos = value.find('x');

    if (xPos == std::string::npos) {
        return {captureWidth, captureHeight};
    }

    uint32_t width = 0;
    uint32_t height = 0;

    try {
        width = static_cast<uint32_t>(std::stoul(value.substr(0, xPos)));
        height = static_cast<uint32_t>(std::stoul(value.substr(xPos + 1)));
    } catch (...) {
        return {captureWidth, captureHeight};
    }

    if (width < 320 || height < 200 || width > captureWidth || height > captureHeight) {
        return {captureWidth, captureHeight};
    }

    width &= ~1u;
    height &= ~1u;
    return {width, height};
}

HostConfig LoadHostConfig(uint32_t captureWidth, uint32_t captureHeight) {
    HostConfig config{};
    std::ifstream input("MintDeskHost.ini");
    std::string line;

    while (std::getline(input, line)) {
        auto comment = line.find_first_of("#;");

        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        auto equal = line.find('=');

        if (equal == std::string::npos) {
            continue;
        }

        std::string key = Trim(line.substr(0, equal));
        std::string value = Trim(line.substr(equal + 1));

        if (key == "resolution") {
            config.resolution = value;
        } else if (key == "fps") {
            try {
                config.fps = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
                config.fps = 60;
            }
        }
    }

    config.fps = std::clamp(config.fps, 1u, 240u);
    auto [outputWidth, outputHeight] = ParseResolution(
        config.resolution,
        captureWidth,
        captureHeight
    );
    config.outputWidth = outputWidth;
    config.outputHeight = outputHeight;
    return config;
}

IDXGIAdapter1* FindNvidiaAdapter() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory)
    );

    if (FAILED(hr)) {
        std::cerr << "CreateDXGIFactory1 failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    IDXGIAdapter1* selectedAdapter = nullptr;

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        hr = factory->EnumAdapters1(index, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        if (FAILED(hr)) {
            std::cerr << "EnumAdapters1 failed: 0x"
                      << std::hex << hr << std::dec << "\n";
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        std::wcout
            << L"Adapter "
            << index
            << L": "
            << desc.Description
            << L" | VendorId: 0x"
            << std::hex
            << desc.VendorId
            << std::dec
            << L"\n";

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x10DE) {
            selectedAdapter = adapter;
            break;
        }

        SafeRelease(adapter);
    }

    SafeRelease(factory);
    return selectedAdapter;
}

bool CreateD3D11Device(
    IDXGIAdapter1* adapter,
    ID3D11Device** device,
    ID3D11DeviceContext** context
) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);

    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        device,
        &featureLevel,
        context
    );

    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    std::wcout << L"D3D11 device created on adapter: " << desc.Description << L"\n";
    return true;
}

IDXGIOutputDuplication* CreateDesktopDuplication(
    IDXGIAdapter1* adapter,
    ID3D11Device* device,
    uint32_t& width,
    uint32_t& height
) {
    IDXGIOutput* output = nullptr;
    HRESULT hr = adapter->EnumOutputs(0, &output);

    if (FAILED(hr)) {
        std::cerr << "EnumOutputs failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);

    std::wcout << L"Capturing output: " << outputDesc.DeviceName << L"\n";

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(
        __uuidof(IDXGIOutput1),
        reinterpret_cast<void**>(&output1)
    );

    if (FAILED(hr)) {
        std::cerr << "QueryInterface IDXGIOutput1 failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        SafeRelease(output);
        return nullptr;
    }

    IDXGIOutputDuplication* duplication = nullptr;
    hr = output1->DuplicateOutput(device, &duplication);

    if (FAILED(hr)) {
        std::cerr << "DuplicateOutput failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        SafeRelease(output1);
        SafeRelease(output);
        return nullptr;
    }

    DXGI_OUTDUPL_DESC duplicationDesc{};
    duplication->GetDesc(&duplicationDesc);

    width = duplicationDesc.ModeDesc.Width;
    height = duplicationDesc.ModeDesc.Height;

    std::cout
        << "Desktop Duplication ready: "
        << width
        << "x"
        << height
        << "\n";

    SafeRelease(output1);
    SafeRelease(output);
    return duplication;
}

IDXGIOutputDuplication* CreateDesktopDuplicationForOutput(
    IDXGIOutput* output,
    ID3D11Device* device,
    uint32_t& width,
    uint32_t& height
) {
    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);

    std::wcout << L"Trying output: " << outputDesc.DeviceName << L"\n";

    IDXGIOutput1* output1 = nullptr;
    HRESULT hr = output->QueryInterface(
        __uuidof(IDXGIOutput1),
        reinterpret_cast<void**>(&output1)
    );

    if (FAILED(hr)) {
        std::cerr << "QueryInterface IDXGIOutput1 failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    IDXGIOutputDuplication* duplication = nullptr;
    hr = output1->DuplicateOutput(device, &duplication);
    SafeRelease(output1);

    if (FAILED(hr)) {
        std::cerr << "DuplicateOutput failed for this output: 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    DXGI_OUTDUPL_DESC duplicationDesc{};
    duplication->GetDesc(&duplicationDesc);

    width = duplicationDesc.ModeDesc.Width;
    height = duplicationDesc.ModeDesc.Height;

    std::wcout << L"Capturing output: " << outputDesc.DeviceName << L"\n";
    std::cout
        << "Desktop Duplication ready: "
        << width
        << "x"
        << height
        << "\n";

    return duplication;
}

IDXGIOutputDuplication* CreateBestCaptureDevice(
    IDXGIAdapter1** selectedAdapter,
    ID3D11Device** selectedDevice,
    ID3D11DeviceContext** selectedContext,
    uint32_t& width,
    uint32_t& height
) {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory)
    );

    if (FAILED(hr)) {
        std::cerr << "CreateDXGIFactory1 failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    IDXGIOutputDuplication* selectedDuplication = nullptr;

    for (int pass = 0; pass < 2 && !selectedDuplication; ++pass) {
        const bool preferNvidia = pass == 0;

        for (UINT adapterIndex = 0; !selectedDuplication; ++adapterIndex) {
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);

            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            if (FAILED(hr)) {
                std::cerr << "EnumAdapters1 failed: 0x"
                          << std::hex << hr << std::dec << "\n";
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const bool isNvidia = !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && desc.VendorId == 0x10DE;

            if (preferNvidia != isNvidia) {
                SafeRelease(adapter);
                continue;
            }

            std::wcout
                << L"Adapter "
                << adapterIndex
                << L": "
                << desc.Description
                << L" | VendorId: 0x"
                << std::hex
                << desc.VendorId
                << std::dec
                << L"\n";

            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;

            if (!CreateD3D11Device(adapter, &device, &context)) {
                SafeRelease(adapter);
                continue;
            }

            for (UINT outputIndex = 0; !selectedDuplication; ++outputIndex) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(outputIndex, &output);

                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }

                if (FAILED(hr)) {
                    std::cerr << "EnumOutputs failed: 0x"
                              << std::hex << hr << std::dec << "\n";
                    break;
                }

                selectedDuplication = CreateDesktopDuplicationForOutput(
                    output,
                    device,
                    width,
                    height
                );

                SafeRelease(output);
            }

            if (selectedDuplication) {
                *selectedAdapter = adapter;
                *selectedDevice = device;
                *selectedContext = context;
                break;
            }

            SafeRelease(context);
            SafeRelease(device);
            SafeRelease(adapter);
        }
    }

    SafeRelease(factory);
    return selectedDuplication;
}

bool CopyToCachedTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D** cachedTexture
) {
    if (!device || !context || !source || !cachedTexture) {
        return false;
    }

    if (!*cachedTexture) {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, cachedTexture);

        if (FAILED(hr)) {
            std::cerr << "Create cached desktop texture failed: 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    context->CopyResource(*cachedTexture, source);
    return true;
}
}

int main() {
    constexpr uint16_t kPort = 9000;
    constexpr uint16_t kInputPort = 9001;
    constexpr uint16_t kClipboardPort = 9002;

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::ofstream hostLog(
        "mintdesk-host.log",
        std::ios::out | std::ios::app
    );

    auto log = [&](const std::string& message) {
        std::cout << message << "\n";
        if (hostLog) {
            hostLog << message << "\n";
            hostLog.flush();
        }
    };

    std::cout << "MintDeskHost DXGI -> NV12 -> NVENC -> TCP test\n";
    log("MintDeskHost process started.");

    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    IDXGIOutputDuplication* duplication = CreateBestCaptureDevice(
        &adapter,
        &device,
        &context,
        width,
        height
    );

    GdiCapture gdiCapture;
    bool useGdiCapture = duplication == nullptr;

    if (useGdiCapture) {
        log("DXGI Desktop Duplication unavailable; falling back to GDI capture.");

        adapter = FindNvidiaAdapter();

        if (!adapter) {
            std::cerr << "No NVIDIA adapter found for GDI fallback.\n";
            return 1;
        }

        if (!CreateD3D11Device(adapter, &device, &context)) {
            SafeRelease(adapter);
            return 1;
        }

        if (!gdiCapture.initialize(device, width, height)) {
            std::cerr << "GDI capture initialization failed.\n";
            SafeRelease(context);
            SafeRelease(device);
            SafeRelease(adapter);
            return 1;
        }
    } else {
        if (gdiCapture.initialize(device, width, height)) {
            log("GDI still-frame seed capture ready.");
        } else {
            log("GDI still-frame seed capture unavailable.");
        }
    }

    HostConfig config = LoadHostConfig(width, height);

    log(
        "Host video config: capture " +
        std::to_string(width) +
        "x" +
        std::to_string(height) +
        " -> stream " +
        std::to_string(config.outputWidth) +
        "x" +
        std::to_string(config.outputHeight) +
        " @ " +
        std::to_string(config.fps) +
        " fps"
    );

    VideoConverter converter;

    if (!converter.initialize(device, width, height, config.outputWidth, config.outputHeight)) {
        std::cerr << "VideoConverter initialization failed.\n";
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
        return 1;
    }

    NvEncoder encoder;

    if (!encoder.initialize(device, config.outputWidth, config.outputHeight, config.fps)) {
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
        return 1;
    }

    TcpServer server;
    InputController inputController;
    ClipboardSyncServer clipboardSyncServer;

    if (!server.start(kPort)) {
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
        return 1;
    }

    if (!inputController.start(kInputPort, width, height)) {
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
        return 1;
    }

    if (!clipboardSyncServer.start(kClipboardPort)) {
        inputController.stop();
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
        return 1;
    }

    std::cout
        << "Connect with: ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://<PC_IP>:9000\n";

    uint64_t totalEncodedFrames = 0;
    uint64_t totalBytesSent = 0;

    while (g_running) {
        if (!server.waitForClient()) {
            break;
        }

        encoder.requestKeyFrame();
        log("Client accepted. Requested IDR + SPS/PPS.");
        log("Streaming desktop frames. Press Ctrl+C to stop.");
        log(useGdiCapture ? "Capture mode: GDI fallback." : "Capture mode: DXGI Desktop Duplication.");

        uint32_t intervalFrames = 0;
        auto lastReport = std::chrono::steady_clock::now();
        auto lastTimeoutReport = std::chrono::steady_clock::now();
        bool clientConnected = true;
        ID3D11Texture2D* cachedDesktopTexture = nullptr;
        auto lastDuplicateFrameSent = std::chrono::steady_clock::now();
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(config.fps))
        );
        uint64_t clientFrameCount = 0;

        while (g_running && clientConnected) {
            auto nowBeforeFrame = std::chrono::steady_clock::now();

            if (nowBeforeFrame < nextFrameTime) {
                std::this_thread::sleep_until(nextFrameTime);
            }

            ID3D11Texture2D* desktopTexture = nullptr;
            bool acquiredDxgiFrame = false;
            bool usingCachedFrame = false;

            if (useGdiCapture) {
                if (!gdiCapture.captureFrame(&desktopTexture)) {
                    std::cerr << "GDI captureFrame failed.\n";
                    g_running = false;
                    break;
                }
            } else {
                DXGI_OUTDUPL_FRAME_INFO frameInfo{};
                IDXGIResource* resource = nullptr;

                HRESULT hr = duplication->AcquireNextFrame(
                    5,
                    &frameInfo,
                    &resource
                );

                if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                    auto now = std::chrono::steady_clock::now();
                    double duplicateElapsed = std::chrono::duration<double>(
                        now - lastDuplicateFrameSent
                    ).count();

                    if (duplicateElapsed >= (1.0 / static_cast<double>(config.fps)) &&
                        gdiCapture.captureFrame(&desktopTexture)) {
                        if (!cachedDesktopTexture) {
                            log("Seeded first desktop frame with GDI capture.");
                        }
                        usingCachedFrame = true;
                        SafeRelease(cachedDesktopTexture);
                        if (!CopyToCachedTexture(device, context, desktopTexture, &cachedDesktopTexture)) {
                            log("GDI timeout frame captured; cache copy skipped.");
                        }
                    } else {
                            double timeoutElapsed = std::chrono::duration<double>(
                                now - lastTimeoutReport
                            ).count();

                            if (timeoutElapsed >= 3.0) {
                                log("AcquireNextFrame timeout; no desktop frame available yet.");
                                lastTimeoutReport = now;
                            }
                            continue;
                    }
                }

                if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                    if (hr == DXGI_ERROR_ACCESS_LOST) {
                        log("DXGI duplication access lost.");
                        g_running = false;
                        break;
                    }

                    if (FAILED(hr)) {
                        std::cerr << "AcquireNextFrame failed: 0x"
                                  << std::hex << hr << std::dec << "\n";
                        g_running = false;
                        break;
                    }

                    acquiredDxgiFrame = true;

                    hr = resource->QueryInterface(
                        __uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&desktopTexture)
                    );

                    SafeRelease(resource);

                    if (FAILED(hr)) {
                        std::cerr << "QueryInterface ID3D11Texture2D failed: 0x"
                                  << std::hex << hr << std::dec << "\n";
                        duplication->ReleaseFrame();
                        g_running = false;
                        break;
                    }

                    if (!CopyToCachedTexture(device, context, desktopTexture, &cachedDesktopTexture)) {
                        duplication->ReleaseFrame();
                        g_running = false;
                        break;
                    }
                }
            }

            ID3D11Texture2D* nv12Texture = nullptr;

            if (!converter.convert(desktopTexture, &nv12Texture)) {
                std::cerr << "BGRA -> NV12 conversion failed.\n";
                SafeRelease(desktopTexture);
                if (acquiredDxgiFrame) {
                    duplication->ReleaseFrame();
                }
                g_running = false;
                break;
            }

            std::vector<uint8_t> bitstream;

            if (!encoder.encodeFrame(nv12Texture, bitstream)) {
                log("encodeFrame failed.");
                SafeRelease(nv12Texture);
                SafeRelease(desktopTexture);
                if (acquiredDxgiFrame) {
                    duplication->ReleaseFrame();
                }
                g_running = false;
                break;
            }

            if (clientFrameCount < 3) {
                log(
                    std::string(usingCachedFrame ? "Encoded cached frame: " : "Encoded fresh frame: ") +
                    std::to_string(bitstream.size()) +
                    " bytes"
                );
            }

            if (!bitstream.empty() && !server.sendBytes(bitstream.data(), bitstream.size())) {
                log("sendBytes failed; returning to accept loop.");
                clientConnected = false;
            }

            ++totalEncodedFrames;
            ++intervalFrames;
            ++clientFrameCount;
            totalBytesSent += bitstream.size();
            lastDuplicateFrameSent = std::chrono::steady_clock::now();
            nextFrameTime = lastDuplicateFrameSent + frameDuration;

            SafeRelease(nv12Texture);
            SafeRelease(desktopTexture);

            if (useGdiCapture) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            } else {
                if (acquiredDxgiFrame) {
                    HRESULT hr = duplication->ReleaseFrame();

                    if (FAILED(hr)) {
                        std::cerr << "ReleaseFrame failed: 0x"
                                  << std::hex << hr << std::dec << "\n";
                        g_running = false;
                        break;
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastReport).count();

            if (elapsed >= 1.0) {
                double fps = static_cast<double>(intervalFrames) / elapsed;

                std::cout
                    << "Streaming FPS: "
                    << fps
                    << " | Total frames: "
                    << totalEncodedFrames
                    << " | Sent: "
                    << totalBytesSent
                    << " bytes\n";

                if (hostLog) {
                    hostLog
                        << "Streaming FPS: "
                        << fps
                        << " | Total frames: "
                        << totalEncodedFrames
                        << " | Sent: "
                        << totalBytesSent
                        << " bytes\n";
                    hostLog.flush();
                }

                intervalFrames = 0;
                lastReport = now;
            }
        }

        SafeRelease(cachedDesktopTexture);
        server.disconnectClient();
        log("Client socket closed; back to Waiting for client.");
    }

    clipboardSyncServer.stop();
    inputController.stop();
    SafeRelease(duplication);
    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(adapter);

    if (totalEncodedFrames == 0) {
        return 1;
    }

    std::cout
        << "Stopped. Frames: "
        << totalEncodedFrames
        << " | Sent: "
        << totalBytesSent
        << " bytes\n";

    return 0;
}
