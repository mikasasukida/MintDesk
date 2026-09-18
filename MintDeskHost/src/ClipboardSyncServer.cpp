#include "ClipboardSyncServer.h"

#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace {
constexpr std::array<uint8_t, 4> kMagic{'M', 'D', 'C', 'L'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kTypeText = 1;
constexpr uint16_t kTypeImagePng = 2;
constexpr uint16_t kTypeFile = 3;
constexpr uint32_t kHeaderSize = 24;

ULONG_PTR g_gdiplusToken = 0;

void WriteLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void WriteLe32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void WriteLe64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

uint16_t ReadLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* data) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(data[i]) << (i * 8);
    }
    return value;
}

uint64_t ReadLe64(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );
    return result;
}

bool GetPngEncoderClsid(CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);

    if (count == 0 || bytes == 0) {
        return false;
    }

    std::vector<uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());

    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return false;
    }

    for (UINT index = 0; index < count; ++index) {
        if (wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            clsid = encoders[index].Clsid;
            return true;
        }
    }

    return false;
}

std::vector<uint8_t> BitmapToPngBytes(HBITMAP bitmapHandle) {
    std::vector<uint8_t> bytes;
    if (!bitmapHandle) {
        return bytes;
    }

    CLSID pngClsid{};
    if (!GetPngEncoderClsid(pngClsid)) {
        return bytes;
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return bytes;
    }

    Gdiplus::Bitmap bitmap(bitmapHandle, nullptr);
    if (bitmap.Save(stream, &pngClsid, nullptr) != Gdiplus::Ok) {
        stream->Release();
        return bytes;
    }

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        stream->Release();
        return bytes;
    }

    if (stat.cbSize.QuadPart <= 0 ||
        stat.cbSize.QuadPart > static_cast<ULONGLONG>(50ull * 1024ull * 1024ull)) {
        stream->Release();
        return bytes;
    }

    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    bytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));

    ULONG read = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) ||
        read != bytes.size()) {
        bytes.clear();
    }

    stream->Release();
    return bytes;
}

bool TryOpenClipboard() {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    return false;
}
}

ClipboardSyncServer::~ClipboardSyncServer() {
    stop();
}

bool ClipboardSyncServer::start(uint16_t port) {
    stop();

    if (g_gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput input{};
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
            std::cerr << "Clipboard GDI+ startup failed.\n";
            return false;
        }
    }

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "Clipboard socket failed: WSA error " << WSAGetLastError() << "\n";
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

    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "Clipboard bind failed: WSA error " << WSAGetLastError() << "\n";
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Clipboard listen failed: WSA error " << WSAGetLastError() << "\n";
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;
    worker_ = std::thread([this]() {
        run();
    });

    std::cout << "Clipboard sync server listening on 0.0.0.0:" << port << "\n";
    return true;
}

void ClipboardSyncServer::stop() {
    running_ = false;

    if (listenSocket_ != INVALID_SOCKET) {
        shutdown(listenSocket_, SD_BOTH);
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (worker_.joinable()) {
        worker_.join();
    }
}

void ClipboardSyncServer::run() {
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
                std::cerr << "Clipboard accept failed: WSA error " << WSAGetLastError() << "\n";
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
            << "Clipboard client connected: "
            << clientIp
            << ":"
            << ntohs(clientAddress.sin_port)
            << "\n";

        uint32_t lastSequence = 0;
        std::atomic_bool readerRunning{true};
        std::thread reader([this, clientSocket, &readerRunning]() {
            while (running_ && readerRunning) {
                if (!receiveItem(clientSocket)) {
                    readerRunning = false;
                    break;
                }
            }
        });

        while (running_ && readerRunning) {
            if (!sendCurrentClipboard(clientSocket, lastSequence)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }

        readerRunning = false;
        shutdown(clientSocket, SD_BOTH);
        if (reader.joinable()) {
            reader.join();
        }
        closesocket(clientSocket);
        std::cout << "Clipboard client disconnected.\n";
    }
}

bool ClipboardSyncServer::sendCurrentClipboard(SOCKET clientSocket, uint32_t& lastSequence) {
    uint32_t sequence = GetClipboardSequenceNumber();
    if (sequence == 0 || sequence == lastSequence) {
        return true;
    }

    auto items = readClipboardItems();
    if (items.empty()) {
        lastSequence = sequence;
        return true;
    }

    for (const auto& item : items) {
        if (!sendItem(clientSocket, item)) {
            return false;
        }
    }

    lastSequence = sequence;
    return true;
}

bool ClipboardSyncServer::sendItem(SOCKET clientSocket, const ClipboardItem& item) {
    std::vector<uint8_t> nameBytes(item.name.begin(), item.name.end());
    std::vector<uint8_t> header;
    header.reserve(kHeaderSize);
    header.insert(header.end(), kMagic.begin(), kMagic.end());
    WriteLe16(header, kVersion);
    WriteLe16(header, item.type);
    WriteLe32(header, static_cast<uint32_t>(nameBytes.size()));
    WriteLe64(header, static_cast<uint64_t>(item.payload.size()));
    WriteLe32(header, 0);

    if (!sendAll(clientSocket, header.data(), header.size())) {
        return false;
    }

    if (!nameBytes.empty() && !sendAll(clientSocket, nameBytes.data(), nameBytes.size())) {
        return false;
    }

    if (!item.payload.empty() && !sendAll(clientSocket, item.payload.data(), item.payload.size())) {
        return false;
    }

    const char* typeName = item.type == kTypeText
        ? "text"
        : item.type == kTypeImagePng
            ? "image"
            : "file";
    std::cout
        << "Clipboard sent "
        << typeName
        << " "
        << item.name
        << " ("
        << item.payload.size()
        << " bytes)\n";
    return true;
}

bool ClipboardSyncServer::sendAll(SOCKET clientSocket, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sentTotal = 0;

    while (running_ && sentTotal < size) {
        int chunkSize = static_cast<int>(std::min<size_t>(size - sentTotal, 64 * 1024));
        int sent = send(
            clientSocket,
            reinterpret_cast<const char*>(bytes + sentTotal),
            chunkSize,
            0
        );

        if (sent <= 0) {
            std::cerr << "Clipboard send failed: WSA error " << WSAGetLastError() << "\n";
            return false;
        }

        sentTotal += static_cast<size_t>(sent);
    }

    return sentTotal == size;
}

bool ClipboardSyncServer::receiveAll(SOCKET clientSocket, void* data, size_t size) {
    auto* bytes = static_cast<uint8_t*>(data);
    size_t receivedTotal = 0;

    while (running_ && receivedTotal < size) {
        int chunkSize = static_cast<int>(std::min<size_t>(size - receivedTotal, 64 * 1024));
        int received = recv(
            clientSocket,
            reinterpret_cast<char*>(bytes + receivedTotal),
            chunkSize,
            0
        );
        if (received <= 0) {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }

    return receivedTotal == size;
}

bool ClipboardSyncServer::receiveItem(SOCKET clientSocket) {
    std::array<uint8_t, kHeaderSize> header{};
    if (!receiveAll(clientSocket, header.data(), header.size())) {
        return false;
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
        std::cerr << "Clipboard receive failed: bad magic.\n";
        return false;
    }

    const uint16_t version = ReadLe16(header.data() + 4);
    const uint16_t type = ReadLe16(header.data() + 6);
    const uint32_t nameSize = ReadLe32(header.data() + 8);
    const uint64_t payloadSize = ReadLe64(header.data() + 12);

    if (version != kVersion ||
        nameSize > 4096 ||
        (type != kTypeText && type != kTypeImagePng && type != kTypeFile)) {
        std::cerr << "Clipboard receive rejected: version=" << version
                  << " type=" << type
                  << " name=" << nameSize
                  << " size=" << payloadSize << "\n";
        return false;
    }

    std::cout << "Clipboard incoming packet: type=" << type
              << " nameBytes=" << nameSize
              << " payloadBytes=" << payloadSize << "\n";

    std::string name(nameSize, '\0');
    if (nameSize > 0 && !receiveAll(clientSocket, name.data(), name.size())) {
        return false;
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
    if (!payload.empty() && !receiveAll(clientSocket, payload.data(), payload.size())) {
        return false;
    }

    if (!saveReceivedItem(type, name, payload)) {
        std::cerr << "Clipboard receive could not save " << name << "\n";
        return true;
    }

    std::cout << "Clipboard received " << name << " (" << payload.size()
              << " bytes)\n";
    return true;
}

bool ClipboardSyncServer::saveReceivedItem(
    uint16_t type,
    const std::string& name,
    const std::vector<uint8_t>& payload
) {
    std::wstring wideName;
    int wideSize = MultiByteToWideChar(
        CP_UTF8,
        0,
        name.data(),
        static_cast<int>(name.size()),
        nullptr,
        0
    );
    if (wideSize > 0) {
        wideName.resize(static_cast<size_t>(wideSize));
        MultiByteToWideChar(
            CP_UTF8,
            0,
            name.data(),
            static_cast<int>(name.size()),
            wideName.data(),
            wideSize
        );
    }

    std::filesystem::path cleanName = std::filesystem::path(wideName).filename();
    if (cleanName.empty()) {
        cleanName = type == kTypeImagePng ? L"mintdesk-image.png" : L"mintdesk-file.bin";
    }

    std::filesystem::path directory = L"D:\\MintDesk\\Received";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "Clipboard receive directory failed: " << error.message() << "\n";
        return false;
    }

    std::filesystem::path output = directory / cleanName;
    if (std::filesystem::exists(output, error)) {
        const auto stem = cleanName.stem().wstring();
        const auto extension = cleanName.extension().wstring();
        for (int index = 1; index < 10000; ++index) {
            output = directory / (stem + L"_" + std::to_wstring(index) + extension);
            if (!std::filesystem::exists(output, error)) {
                break;
            }
        }
    }

    std::ofstream file(output, std::ios::binary);
    if (!file) {
        std::cerr << "Clipboard receive open failed: " << output.string() << "\n";
        return false;
    }
    file.write(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<std::streamsize>(payload.size())
    );
    const bool saved = file.good();
    std::cout << "Clipboard receive save path: " << output.string()
              << " result=" << saved << "\n";
    return saved;
}

std::vector<ClipboardSyncServer::ClipboardItem> ClipboardSyncServer::readClipboardItems() {
    std::vector<ClipboardItem> items;

    if (!TryOpenClipboard()) {
        return items;
    }

    if (!readFileDrop(items)) {
        if (!readBitmap(items)) {
            readUnicodeText(items);
        }
    }

    CloseClipboard();
    return items;
}

bool ClipboardSyncServer::readUnicodeText(std::vector<ClipboardItem>& items) {
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        return false;
    }

    auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text) {
        return false;
    }

    std::wstring wideText(text);
    GlobalUnlock(handle);

    std::string utf8 = WideToUtf8(wideText);
    if (utf8.empty()) {
        return false;
    }

    ClipboardItem item{};
    item.type = kTypeText;
    item.name = "clipboard.txt";
    item.payload.assign(utf8.begin(), utf8.end());
    items.push_back(std::move(item));
    return true;
}

bool ClipboardSyncServer::readBitmap(std::vector<ClipboardItem>& items) {
    HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!bitmap) {
        return false;
    }

    std::vector<uint8_t> png = BitmapToPngBytes(bitmap);
    if (png.empty()) {
        return false;
    }

    ClipboardItem item{};
    item.type = kTypeImagePng;
    item.name = "clipboard.png";
    item.payload = std::move(png);
    items.push_back(std::move(item));
    return true;
}

bool ClipboardSyncServer::readFileDrop(std::vector<ClipboardItem>& items) {
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) {
        return false;
    }

    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    bool added = false;

    for (UINT index = 0; index < count; ++index) {
        UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }

        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);

        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            continue;
        }

        uint64_t fileSize = std::filesystem::file_size(path, error);
        if (error || fileSize == 0) {
            std::wcout
                << L"Clipboard file skipped: "
                << path
                << L" size="
                << fileSize
                << L"\n";
            continue;
        }

        std::ifstream input(path.c_str(), std::ios::binary);
        if (!input) {
            continue;
        }

        ClipboardItem item{};
        item.type = kTypeFile;
        item.name = WideToUtf8(std::filesystem::path(path).filename().wstring());
        item.payload.resize(static_cast<size_t>(fileSize));
        input.read(reinterpret_cast<char*>(item.payload.data()), static_cast<std::streamsize>(item.payload.size()));

        if (input.gcount() == static_cast<std::streamsize>(item.payload.size())) {
            items.push_back(std::move(item));
            added = true;
        }
    }

    return added;
}
