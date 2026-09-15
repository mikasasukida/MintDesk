#include "TcpServer.h"

#include <ws2tcpip.h>

#include <iostream>

namespace {
void PrintSocketError(const char* message) {
    std::cerr << message << ": WSA error " << WSAGetLastError() << "\n";
}
}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(uint16_t port) {
    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << "\n";
        return false;
    }

    wsaStarted_ = true;

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listenSocket_ == INVALID_SOCKET) {
        PrintSocketError("socket failed");
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

    result = bind(
        listenSocket_,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    );

    if (result == SOCKET_ERROR) {
        PrintSocketError("bind failed");
        return false;
    }

    result = listen(listenSocket_, SOMAXCONN);

    if (result == SOCKET_ERROR) {
        PrintSocketError("listen failed");
        return false;
    }

    std::cout << "TCP server listening on 0.0.0.0:" << port << "\n";
    return true;
}

bool TcpServer::waitForClient() {
    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "TcpServer::waitForClient called before start.\n";
        return false;
    }

    disconnectClient();

    std::cout << "Waiting for client...\n";

    sockaddr_in clientAddress{};
    int clientAddressSize = sizeof(clientAddress);

    clientSocket_ = accept(
        listenSocket_,
        reinterpret_cast<sockaddr*>(&clientAddress),
        &clientAddressSize
    );

    if (clientSocket_ == INVALID_SOCKET) {
        PrintSocketError("accept failed");
        return false;
    }

    BOOL noDelay = TRUE;
    setsockopt(
        clientSocket_,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&noDelay),
        sizeof(noDelay)
    );

    char clientIp[INET_ADDRSTRLEN]{};
    inet_ntop(
        AF_INET,
        &clientAddress.sin_addr,
        clientIp,
        sizeof(clientIp)
    );

    std::cout
        << "Client connected: "
        << clientIp
        << ":"
        << ntohs(clientAddress.sin_port)
        << "\n";

    return true;
}

bool TcpServer::sendBytes(const uint8_t* data, size_t size) {
    if (clientSocket_ == INVALID_SOCKET || !data || size == 0) {
        return false;
    }

    size_t totalSent = 0;

    while (totalSent < size) {
        size_t remaining = size - totalSent;
        int chunkSize = remaining > static_cast<size_t>(INT_MAX)
            ? INT_MAX
            : static_cast<int>(remaining);

        int sent = send(
            clientSocket_,
            reinterpret_cast<const char*>(data + totalSent),
            chunkSize,
            0
        );

        if (sent == SOCKET_ERROR) {
            PrintSocketError("send failed");
            return false;
        }

        if (sent == 0) {
            std::cerr << "send returned 0; client disconnected.\n";
            return false;
        }

        totalSent += static_cast<size_t>(sent);
    }

    return true;
}

SOCKET TcpServer::clientSocket() const {
    return clientSocket_;
}

void TcpServer::disconnectClient() {
    if (clientSocket_ != INVALID_SOCKET) {
        shutdown(clientSocket_, SD_BOTH);
        closesocket(clientSocket_);
        clientSocket_ = INVALID_SOCKET;
    }
}

void TcpServer::stop() {
    disconnectClient();

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}
