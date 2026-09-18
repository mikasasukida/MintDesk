#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class ClipboardSyncServer {
public:
    ClipboardSyncServer() = default;
    ~ClipboardSyncServer();

    ClipboardSyncServer(const ClipboardSyncServer&) = delete;
    ClipboardSyncServer& operator=(const ClipboardSyncServer&) = delete;

    bool start(uint16_t port);
    void stop();

private:
    struct ClipboardItem {
        uint16_t type = 0;
        std::string name;
        std::vector<uint8_t> payload;
    };

    void run();
    bool sendCurrentClipboard(SOCKET clientSocket, uint32_t& lastSequence);
    bool sendItem(SOCKET clientSocket, const ClipboardItem& item);
    bool sendAll(SOCKET clientSocket, const void* data, size_t size);
    bool receiveItem(SOCKET clientSocket);
    bool receiveAll(SOCKET clientSocket, void* data, size_t size);
    bool saveReceivedItem(uint16_t type, const std::string& name, const std::vector<uint8_t>& payload);
    std::vector<ClipboardItem> readClipboardItems();
    bool readUnicodeText(std::vector<ClipboardItem>& items);
    bool readBitmap(std::vector<ClipboardItem>& items);
    bool readFileDrop(std::vector<ClipboardItem>& items);

    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic_bool running_{false};
    std::thread worker_;
};
