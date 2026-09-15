#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <thread>

class InputController {
public:
    InputController() = default;
    ~InputController();

    InputController(const InputController&) = delete;
    InputController& operator=(const InputController&) = delete;

    bool start(uint16_t port, uint32_t captureWidth, uint32_t captureHeight);
    void stop();

private:
    void run();
    void runClient(SOCKET clientSocket);
    bool receiveExact(void* data, int size);
    void handlePointer(uint32_t action, float normalizedX, float normalizedY);
    void handleWheel(int32_t delta, float normalizedX, float normalizedY);
    void handleText(uint32_t codePoint);
    void handleKey(uint32_t virtualKey, uint32_t action);
    void handleRelativeMouse(float deltaX, float deltaY);
    void handleMouseButton(uint32_t action);

    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET socket_ = INVALID_SOCKET;
    uint32_t captureWidth_ = 0;
    uint32_t captureHeight_ = 0;
    std::atomic_bool running_{false};
    std::thread worker_;
};
