#pragma once

#include <winsock2.h>

#include <cstdint>
#include <cstddef>

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start(uint16_t port);
    bool waitForClient();
    bool sendBytes(const uint8_t* data, size_t size);
    SOCKET clientSocket() const;
    void disconnectClient();
    void stop();

private:
    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET clientSocket_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
};
