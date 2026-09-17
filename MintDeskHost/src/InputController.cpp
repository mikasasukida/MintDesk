#include "InputController.h"

#include <windows.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr std::array<char, 4> kMagic{'M', 'D', 'I', 'N'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kTypePointer = 1;
constexpr uint16_t kTypeWheel = 2;
constexpr uint16_t kTypeText = 3;
constexpr uint16_t kTypeKey = 4;
constexpr uint16_t kTypeRelativeMouse = 5;
constexpr uint16_t kTypeMouseButton = 6;
constexpr uint32_t kPointerPayloadSize = 12;
constexpr uint32_t kWheelPayloadSize = 12;
constexpr uint32_t kTextPayloadSize = 4;
constexpr uint32_t kKeyPayloadSize = 4;
constexpr uint32_t kKeyEventPayloadSize = 8;
constexpr uint32_t kRelativeMousePayloadSize = 8;
constexpr uint32_t kMouseButtonPayloadSize = 4;

constexpr uint32_t kPointerMove = 0;
constexpr uint32_t kPointerDown = 1;
constexpr uint32_t kPointerUp = 2;
constexpr uint32_t kPointerRightDown = 3;
constexpr uint32_t kPointerRightUp = 4;

constexpr uint32_t kKeyActionPress = 0;
constexpr uint32_t kKeyActionDown = 1;
constexpr uint32_t kKeyActionUp = 2;

uint16_t ReadLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0])
        | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

float ReadLeFloat(const uint8_t* data) {
    uint32_t value = ReadLe32(data);
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void SendMouseInput(DWORD flags, LONG x, LONG y) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.dx = x;
    input.mi.dy = y;
    SendInput(1, &input, sizeof(input));
}

void SendMouseWheel(int32_t delta) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    SendInput(1, &input, sizeof(input));
}

void SendVirtualKey(WORD virtualKey) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[1].type = INPUT_KEYBOARD;

    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    if (scanCode != 0) {
        DWORD flags = KEYEVENTF_SCANCODE;
        if ((scanCode & 0xFF00) != 0) {
            flags |= KEYEVENTF_EXTENDEDKEY;
        }
        inputs[0].ki.wScan = static_cast<WORD>(scanCode & 0xFF);
        inputs[0].ki.dwFlags = flags;
        inputs[1].ki.wScan = static_cast<WORD>(scanCode & 0xFF);
        inputs[1].ki.dwFlags = flags | KEYEVENTF_KEYUP;
    } else {
        inputs[0].ki.wVk = virtualKey;
        inputs[1].ki.wVk = virtualKey;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    }

    SendInput(2, inputs, sizeof(INPUT));
}

void SendVirtualKeyDown(WORD virtualKey) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    if (scanCode != 0) {
        input.ki.wScan = static_cast<WORD>(scanCode & 0xFF);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if ((scanCode & 0xFF00) != 0) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    } else {
        input.ki.wVk = virtualKey;
    }
    SendInput(1, &input, sizeof(input));
}

void SendVirtualKeyUp(WORD virtualKey) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    if (scanCode != 0) {
        input.ki.wScan = static_cast<WORD>(scanCode & 0xFF);
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if ((scanCode & 0xFF00) != 0) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    } else {
        input.ki.wVk = virtualKey;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(input));
}

void SendUnicodeUnit(WCHAR codeUnit) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = codeUnit;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = codeUnit;
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void SendUnicodeCodePoint(uint32_t codePoint) {
    if (codePoint == 0) {
        return;
    }

    if (codePoint <= 0xFFFF) {
        SendUnicodeUnit(static_cast<WCHAR>(codePoint));
        return;
    }

    if (codePoint <= 0x10FFFF) {
        codePoint -= 0x10000;
        WCHAR high = static_cast<WCHAR>(0xD800 + (codePoint >> 10));
        WCHAR low = static_cast<WCHAR>(0xDC00 + (codePoint & 0x3FF));
        SendUnicodeUnit(high);
        SendUnicodeUnit(low);
    }
}
}

InputController::~InputController() {
    stop();
}

bool InputController::start(
    uint16_t port,
    uint32_t captureWidth,
    uint32_t captureHeight
) {
    stop();

    captureWidth_ = captureWidth;
    captureHeight_ = captureHeight;
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "Input socket failed: WSA error " << WSAGetLastError() << "\n";
        return false;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(
        listenSocket_,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddr),
        sizeof(reuseAddr)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int result = bind(
        listenSocket_,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    );

    if (result == SOCKET_ERROR) {
        std::cerr << "Input bind failed: WSA error " << WSAGetLastError() << "\n";
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    result = listen(listenSocket_, SOMAXCONN);

    if (result == SOCKET_ERROR) {
        std::cerr << "Input listen failed: WSA error " << WSAGetLastError() << "\n";
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;

    worker_ = std::thread([this]() {
        run();
    });

    std::cout << "Input control server listening on 0.0.0.0:" << port << "\n";
    return true;
}

void InputController::stop() {
    running_ = false;

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
        }
    }

    if (listenSocket_ != INVALID_SOCKET) {
        shutdown(listenSocket_, SD_BOTH);
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    if (clientWorker_.joinable()) {
        clientWorker_.join();
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket_ = INVALID_SOCKET;
    }
}

bool InputController::receiveExact(SOCKET clientSocket, void* data, int size) {
    auto* out = static_cast<char*>(data);
    int receivedTotal = 0;

    while (running_ && receivedTotal < size) {
        int received = recv(
            clientSocket,
            out + receivedTotal,
            size - receivedTotal,
            0
        );

        if (received <= 0) {
            return false;
        }

        receivedTotal += received;
    }

    return receivedTotal == size;
}

void InputController::run() {
    while (running_) {
        sockaddr_in clientAddress{};
        int clientAddressSize = sizeof(clientAddress);
        SOCKET clientSocket = accept(
            listenSocket_,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &clientAddressSize
        );

        if (!running_) {
            if (clientSocket != INVALID_SOCKET) {
                closesocket(clientSocket);
            }
            break;
        }

        if (clientSocket == INVALID_SOCKET) {
            if (running_) {
                std::cerr << "Input accept failed: WSA error " << WSAGetLastError() << "\n";
            }
            continue;
        }

        BOOL noDelay = TRUE;
        setsockopt(
            clientSocket,
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<const char*>(&noDelay),
            sizeof(noDelay)
        );

        char clientIp[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddress.sin_addr, clientIp, sizeof(clientIp));
        std::cout
            << "Input client connected: "
            << clientIp
            << ":"
            << ntohs(clientAddress.sin_port)
            << "\n";

        SOCKET previousSocket = INVALID_SOCKET;

        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            previousSocket = socket_;
        }

        if (previousSocket != INVALID_SOCKET) {
            std::cout << "Replacing previous input client.\n";
            shutdown(previousSocket, SD_BOTH);
        }

        if (clientWorker_.joinable()) {
            clientWorker_.join();
        }

        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            socket_ = clientSocket;
        }

        clientWorker_ = std::thread([this, clientSocket]() {
            runClient(clientSocket);

            {
                std::lock_guard<std::mutex> lock(socketMutex_);
                if (socket_ == clientSocket) {
                    socket_ = INVALID_SOCKET;
                }
            }

            shutdown(clientSocket, SD_BOTH);
            closesocket(clientSocket);
            std::cout << "Input client disconnected.\n";
        });
    }

    if (clientWorker_.joinable()) {
        clientWorker_.join();
    }

    running_ = false;
}

void InputController::runClient(SOCKET clientSocket) {
    while (running_) {
        std::array<uint8_t, 12> header{};

        if (!receiveExact(clientSocket, header.data(), static_cast<int>(header.size()))) {
            break;
        }

        std::cout
            << "Input header: "
            << static_cast<char>(header[0])
            << static_cast<char>(header[1])
            << static_cast<char>(header[2])
            << static_cast<char>(header[3])
            << " payload="
            << ReadLe32(header.data() + 8)
            << "\n";

        if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
            std::cerr << "Input packet magic mismatch.\n";
            break;
        }

        uint16_t version = ReadLe16(header.data() + 4);
        uint16_t type = ReadLe16(header.data() + 6);
        uint32_t payloadSize = ReadLe32(header.data() + 8);

        if (version != kVersion || payloadSize > 1024) {
            std::cerr
                << "Unsupported input packet. version="
                << version
                << " payloadSize="
                << payloadSize
                << "\n";
            break;
        }

        std::vector<uint8_t> payload(payloadSize);

        if (payloadSize > 0 &&
            !receiveExact(clientSocket, payload.data(), static_cast<int>(payload.size()))) {
            break;
        }

        if (type == kTypePointer && payloadSize == kPointerPayloadSize) {
            uint32_t action = ReadLe32(payload.data());
            float x = ReadLeFloat(payload.data() + 4);
            float y = ReadLeFloat(payload.data() + 8);
            handlePointer(action, x, y);
        } else if (type == kTypeWheel && payloadSize == kWheelPayloadSize) {
            int32_t delta = static_cast<int32_t>(ReadLe32(payload.data()));
            float x = ReadLeFloat(payload.data() + 4);
            float y = ReadLeFloat(payload.data() + 8);
            handleWheel(delta, x, y);
        } else if (type == kTypeText && payloadSize == kTextPayloadSize) {
            handleText(ReadLe32(payload.data()));
        } else if (type == kTypeKey && payloadSize == kKeyPayloadSize) {
            handleKey(ReadLe32(payload.data()), kKeyActionPress);
        } else if (type == kTypeKey && payloadSize == kKeyEventPayloadSize) {
            uint32_t action = ReadLe32(payload.data());
            uint32_t virtualKey = ReadLe32(payload.data() + 4);
            handleKey(virtualKey, action);
        } else if (type == kTypeRelativeMouse && payloadSize == kRelativeMousePayloadSize) {
            float deltaX = ReadLeFloat(payload.data());
            float deltaY = ReadLeFloat(payload.data() + 4);
            handleRelativeMouse(deltaX, deltaY);
        } else if (type == kTypeMouseButton && payloadSize == kMouseButtonPayloadSize) {
            handleMouseButton(ReadLe32(payload.data()));
        }
    }
}

void InputController::handlePointer(
    uint32_t action,
    float normalizedX,
    float normalizedY
) {
    if (captureWidth_ == 0 || captureHeight_ == 0) {
        return;
    }

    normalizedX = std::clamp(normalizedX, 0.0f, 1.0f);
    normalizedY = std::clamp(normalizedY, 0.0f, 1.0f);

    int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (virtualWidth <= 1 || virtualHeight <= 1) {
        return;
    }

    int desktopX = virtualX + static_cast<int>(normalizedX * (captureWidth_ - 1));
    int desktopY = virtualY + static_cast<int>(normalizedY * (captureHeight_ - 1));
    SetCursorPos(desktopX, desktopY);

    static uint64_t pointerEvents = 0;
    ++pointerEvents;

    if (action != kPointerMove || pointerEvents <= 10 || pointerEvents % 60 == 0) {
        const char* actionName = action == kPointerDown
            ? "down"
            : action == kPointerUp
                ? "up"
                : action == kPointerRightDown
                    ? "right down"
                    : action == kPointerRightUp
                        ? "right up"
                        : action == kPointerMove
                            ? "move"
                            : "unknown";
        std::cout
            << "Pointer "
            << actionName
            << " -> "
            << desktopX
            << ","
            << desktopY
            << " ("
            << normalizedX
            << ","
            << normalizedY
            << ")\n";
    }

    LONG absoluteX = static_cast<LONG>(
        (static_cast<long long>(desktopX - virtualX) * 65535) / (virtualWidth - 1)
    );
    LONG absoluteY = static_cast<LONG>(
        (static_cast<long long>(desktopY - virtualY) * 65535) / (virtualHeight - 1)
    );

    DWORD flags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    SendMouseInput(flags, absoluteX, absoluteY);

    if (action == kPointerDown) {
        SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0);
    } else if (action == kPointerUp) {
        SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0);
    } else if (action == kPointerRightDown) {
        SendMouseInput(MOUSEEVENTF_RIGHTDOWN, 0, 0);
    } else if (action == kPointerRightUp) {
        SendMouseInput(MOUSEEVENTF_RIGHTUP, 0, 0);
    } else if (action != kPointerMove) {
        std::cerr << "Unknown pointer action: " << action << "\n";
    }
}

void InputController::handleWheel(
    int32_t delta,
    float normalizedX,
    float normalizedY
) {
    handlePointer(kPointerMove, normalizedX, normalizedY);
    SendMouseWheel(delta);
    std::cout
        << "Wheel "
        << delta
        << " at ("
        << normalizedX
        << ","
        << normalizedY
        << ")\n";
}

void InputController::handleText(uint32_t codePoint) {
    SendUnicodeCodePoint(codePoint);
    std::cout << "Text codepoint U+" << std::hex << codePoint << std::dec << "\n";
}

void InputController::handleKey(uint32_t virtualKey, uint32_t action) {
    WORD vk = static_cast<WORD>(virtualKey);

    if (action == kKeyActionDown) {
        SendVirtualKeyDown(vk);
        std::cout << "Key down VK " << virtualKey << "\n";
    } else if (action == kKeyActionUp) {
        SendVirtualKeyUp(vk);
        std::cout << "Key up VK " << virtualKey << "\n";
    } else {
        SendVirtualKey(vk);
        std::cout << "Key press VK " << virtualKey << "\n";
    }
}

void InputController::handleRelativeMouse(float deltaX, float deltaY) {
    LONG dx = static_cast<LONG>(deltaX);
    LONG dy = static_cast<LONG>(deltaY);

    if (dx == 0 && dy == 0) {
        return;
    }

    SendMouseInput(MOUSEEVENTF_MOVE, dx, dy);
}

void InputController::handleMouseButton(uint32_t action) {
    if (action == kPointerDown) {
        SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0);
    } else if (action == kPointerUp) {
        SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0);
    } else if (action == kPointerRightDown) {
        SendMouseInput(MOUSEEVENTF_RIGHTDOWN, 0, 0);
    } else if (action == kPointerRightUp) {
        SendMouseInput(MOUSEEVENTF_RIGHTUP, 0, 0);
    } else {
        std::cerr << "Unknown mouse button action: " << action << "\n";
    }
}
