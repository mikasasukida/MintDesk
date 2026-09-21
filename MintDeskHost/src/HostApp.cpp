#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace {
constexpr int kStartButton = 1001;
constexpr int kStopButton = 1002;
constexpr int kFilesButton = 1003;
constexpr int kConfigButton = 1004;
constexpr int kRefreshButton = 1005;
constexpr int kUpdateButton = 1006;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kUpdateFinishedMessage = WM_APP + 2;
constexpr wchar_t kCurrentVersion[] = L"0.2.7";
constexpr wchar_t kManifestUrl[] = L"https://api.github.com/repos/mikasasukida/MintDesk/contents/release/latest.json?ref=main";

HWND g_status = nullptr;
HWND g_ip = nullptr;
HWND g_start = nullptr;
HWND g_stop = nullptr;
HWND g_dropZone = nullptr;
HANDLE g_hostProcess = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_bodyFont = nullptr;
HBRUSH g_panelBrush = nullptr;
bool g_updateRunning = false;

std::filesystem::path AppDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

std::filesystem::path HostExecutable() {
    return AppDirectory() / L"MintDeskHost.exe";
}

std::filesystem::path ReceivedDirectory() {
    return L"D:\\MintDesk\\Received";
}

std::wstring CurrentIPv4() {
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, nullptr, &size);
    if (size == 0) {
        return L"Unavailable";
    }

    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, adapters, &size) != NO_ERROR) {
        return L"Unavailable";
    }

    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
            const DWORD hostOrder = ntohl(ipv4->sin_addr.S_un.S_addr);
            if ((hostOrder >> 24) == 127 || (hostOrder >> 24) == 169 ||
                (hostOrder >> 24) == 198) {
                continue;
            }
            wchar_t result[INET_ADDRSTRLEN]{};
            InetNtopW(AF_INET, &ipv4->sin_addr, result, INET_ADDRSTRLEN);
            return result;
        }
    }
    return L"Unavailable";
}

bool IsHostRunning() {
    return g_hostProcess && WaitForSingleObject(g_hostProcess, 0) == WAIT_TIMEOUT;
}

void SetStatus(const std::wstring& status) {
    if (g_status) {
        SetWindowTextW(g_status, status.c_str());
    }
}

void PostStatus(HWND window, const std::wstring& status) {
    auto* message = new std::wstring(status);
    if (!PostMessageW(window, kStatusMessage, 0, reinterpret_cast<LPARAM>(message))) {
        delete message;
    }
}

std::wstring ExtractJsonString(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const size_t keyPosition = json.find(marker);
    if (keyPosition == std::string::npos) {
        return {};
    }
    const size_t colon = json.find(':', keyPosition + marker.size());
    const size_t firstQuote = json.find('"', colon + 1);
    const size_t secondQuote = json.find('"', firstQuote + 1);
    if (colon == std::string::npos || firstQuote == std::string::npos || secondQuote == std::string::npos) {
        return {};
    }
    const std::string value = json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    return std::wstring(value.begin(), value.end());
}

std::string ExtractJsonAscii(const std::string& json, const std::string& key) {
    const std::wstring value = ExtractJsonString(json, key);
    return std::string(value.begin(), value.end());
}

std::string DecodeGithubContent(const std::string& json) {
    std::string encoded = ExtractJsonAscii(json, "content");
    std::string normalized;
    normalized.reserve(encoded.size());
    for (size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '\\' && index + 1 < encoded.size() && encoded[index + 1] == 'n') {
            ++index;
        } else if (!std::isspace(static_cast<unsigned char>(encoded[index]))) {
            normalized.push_back(encoded[index]);
        }
    }

    DWORD decodedSize = 0;
    if (normalized.empty() || !CryptStringToBinaryA(normalized.c_str(), 0, CRYPT_STRING_BASE64_ANY,
                                                     nullptr, &decodedSize, nullptr, nullptr)) {
        return {};
    }
    std::string decoded(decodedSize, '\0');
    if (!CryptStringToBinaryA(normalized.c_str(), 0, CRYPT_STRING_BASE64_ANY,
                              reinterpret_cast<BYTE*>(decoded.data()), &decodedSize, nullptr, nullptr)) {
        return {};
    }
    decoded.resize(decodedSize);
    return decoded;
}

std::filesystem::path TemporaryFile(const wchar_t* name) {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    return std::filesystem::path(std::wstring(buffer, length)) / name;
}

void CheckForUpdates(HWND window) {
    if (g_updateRunning) {
        return;
    }
    g_updateRunning = true;
    EnableWindow(GetDlgItem(window, kUpdateButton), FALSE);
    SetStatus(L"Checking for updates...");

    std::thread([window]() {
        const auto manifestPath = TemporaryFile(L"mintdesk-latest.json");
        const HRESULT downloadResult = URLDownloadToFileW(
            nullptr,
            kManifestUrl,
            manifestPath.c_str(),
            0,
            nullptr
        );

        std::wstring result;
        if (FAILED(downloadResult)) {
            result = L"Update check failed. Check your internet connection.";
        } else {
            std::ifstream input(manifestPath, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            std::wstring version = ExtractJsonString(json, "version");
            if (version.empty()) {
                const std::string decodedManifest = DecodeGithubContent(json);
                version = ExtractJsonString(decodedManifest, "version");
                if (!version.empty()) {
                    json = decodedManifest;
                }
            }
            if (version.empty()) {
                result = L"Update manifest is invalid.";
            } else if (version == kCurrentVersion) {
                result = L"MintDesk is up to date (v" + std::wstring(kCurrentVersion) + L").";
            } else {
                const std::wstring packageUrl = ExtractJsonString(json, "downloadUrl");
                if (packageUrl.empty()) {
                    result = L"A new version was found, but its download link is missing.";
                } else {
                    const int choice = MessageBoxW(
                        window,
                        (L"New version v" + version + L" is available. Download and install it now?").c_str(),
                        L"MintDesk Update",
                        MB_YESNO | MB_ICONINFORMATION
                    );
                    if (choice == IDYES) {
                        const auto zipPath = TemporaryFile(L"MintDesk-update.zip");
                        PostStatus(window, L"Downloading update...");
                        if (SUCCEEDED(URLDownloadToFileW(nullptr, packageUrl.c_str(), zipPath.c_str(), 0, nullptr))) {
                            const auto scriptPath = TemporaryFile(L"MintDesk-update.ps1");
                            std::wofstream script(scriptPath);
                            const auto appDirectory = AppDirectory().wstring();
                            script << L"$app='" << appDirectory << L"'\n";
                            script << L"$zip='" << zipPath.wstring() << L"'\n";
                            script << L"$stage=Join-Path $env:TEMP 'MintDesk-update-stage'\n";
                            script << L"while (Get-Process -Id " << GetCurrentProcessId() << L" -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }\n";
                            script << L"Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue\n";
                            script << L"Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force\n";
                            script << L"Get-ChildItem -LiteralPath $stage | Where-Object { $_.Name -ne 'MintDeskHost.ini' } | Copy-Item -Destination $app -Recurse -Force\n";
                            script << L"Start-Process (Join-Path $app 'MintDeskHostApp.exe')\n";
                            script.close();

                            ShellExecuteW(
                                nullptr,
                                L"open",
                                L"powershell.exe",
                                (L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath.wstring() + L"\"").c_str(),
                                nullptr,
                                SW_HIDE
                            );
                            PostMessageW(window, WM_CLOSE, 0, 0);
                            return;
                        }
                        result = L"Update download failed.";
                    } else {
                        result = L"Update postponed.";
                    }
                }
            }
        }

        auto* message = new std::wstring(result);
        PostMessageW(window, kUpdateFinishedMessage, 0, reinterpret_cast<LPARAM>(message));
    }).detach();
}

void RefreshUi() {
    if (g_ip) {
        SetWindowTextW(g_ip, (L"IPv4  " + CurrentIPv4() + L"    |    Video 9000    Input 9001    Files 9002").c_str());
    }

    const bool running = IsHostRunning();
    EnableWindow(g_start, running ? FALSE : TRUE);
    EnableWindow(g_stop, running ? TRUE : FALSE);
    SetStatus(running ? L"Host is running and ready for connections" : L"Host is stopped");
}

void StartHost() {
    if (IsHostRunning()) {
        return;
    }

    const auto executable = HostExecutable();
    if (!std::filesystem::exists(executable)) {
        MessageBoxW(nullptr, L"MintDeskHost.exe was not found beside this application.", L"MintDesk", MB_ICONERROR);
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
        executable.c_str(),
        nullptr,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        AppDirectory().c_str(),
        &startup,
        &process
    )) {
        MessageBoxW(nullptr, L"MintDeskHost could not be started.", L"MintDesk", MB_ICONERROR);
        return;
    }

    CloseHandle(process.hThread);
    g_hostProcess = process.hProcess;
    SetStatus(L"Starting capture and network services...");
    RefreshUi();
}

void StopHost() {
    if (!IsHostRunning()) {
        return;
    }
    TerminateProcess(g_hostProcess, 0);
    WaitForSingleObject(g_hostProcess, 2000);
    CloseHandle(g_hostProcess);
    g_hostProcess = nullptr;
    RefreshUi();
}

void OpenPath(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool PutFilesOnClipboard(const std::vector<std::wstring>& files) {
    if (files.empty() || !OpenClipboard(nullptr)) {
        return false;
    }

    EmptyClipboard();
    size_t characters = sizeof(DROPFILES) / sizeof(wchar_t) + 1;
    for (const auto& file : files) {
        characters += file.size() + 1;
    }

    HGLOBAL memory = GlobalAlloc(GHND, characters * sizeof(wchar_t));
    if (!memory) {
        CloseClipboard();
        return false;
    }

    auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!dropFiles) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;
    auto* target = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(dropFiles) + sizeof(DROPFILES));
    for (const auto& file : files) {
        std::copy(file.begin(), file.end(), target);
        target += file.size() + 1;
    }
    *target = L'\0';
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_HDROP, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void QueueDroppedFiles(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> files;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        files.push_back(std::move(path));
    }
    DragFinish(drop);
    if (PutFilesOnClipboard(files)) {
        SetStatus(files.size() == 1 ? L"File queued for Android transfer" : L"Files queued for Android transfer");
    } else {
        SetStatus(L"Could not queue dropped file");
    }
}

LRESULT CALLBACK DropZoneProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static POINT dragOffset{};
    switch (message) {
    case WM_CREATE:
        DragAcceptFiles(window, TRUE);
        return 0;
    case WM_LBUTTONDOWN: {
        dragOffset.x = static_cast<short>(LOWORD(lParam));
        dragOffset.y = static_cast<short>(HIWORD(lParam));
        SetCapture(window);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == window && (wParam & MK_LBUTTON)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            HWND parent = GetParent(window);
            ScreenToClient(parent, &cursor);
            SetWindowPos(window, nullptr, cursor.x - dragOffset.x, cursor.y - dragOffset.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_DROPFILES:
        QueueDroppedFiles(reinterpret_cast<HDROP>(wParam));
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH background = CreateSolidBrush(RGB(30, 39, 50));
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        HPEN border = CreatePen(PS_DASH, 2, RGB(88, 166, 255));
        HGDIOBJ oldPen = SelectObject(dc, border);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, 8, 8, bounds.right - 8, bounds.bottom - 8);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(border);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(225, 235, 245));
        SelectObject(dc, g_bodyFont);
        RECT text = bounds;
        InflateRect(&text, -18, -18);
        DrawTextW(dc, L"拖到这里上传文件\n\n大文件和小文件都支持", -1, &text,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    const bool enabled = IsWindowEnabled(item->hwndItem) != FALSE;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    HBRUSH brush = CreateSolidBrush(!enabled ? RGB(48, 54, 61) : pressed ? RGB(48, 126, 204) : RGB(44, 96, 150));
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, enabled ? RGB(240, 246, 252) : RGB(139, 148, 158));
    wchar_t text[128]{};
    GetWindowTextW(item->hwndItem, text, 128);
    DrawTextW(item->hDC, text, -1, const_cast<RECT*>(&item->rcItem), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_panelBrush = CreateSolidBrush(RGB(22, 27, 34));
        g_titleFont = CreateFontW(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        auto makeStatic = [&](const wchar_t* text, int x, int y, int width, int height, HFONT font) {
            HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height, window, nullptr, nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return control;
        };
        auto makeButton = [&](const wchar_t* text, int id, int x, int y, int width) {
            HWND control = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, width, 42, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            return control;
        };

        makeStatic(L"MintDesk", 36, 30, 400, 42, g_titleFont);
        makeStatic(L"Remote desktop host", 38, 74, 400, 26, g_bodyFont);
        makeStatic(L"This PC", 38, 144, 300, 30, g_bodyFont);
        g_ip = makeStatic(L"Detecting network...", 38, 178, 430, 28, g_bodyFont);
        g_status = makeStatic(L"Host is stopped", 38, 238, 430, 32, g_bodyFont);
        g_dropZone = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"MintDeskDropZone", nullptr,
                                     WS_POPUP | WS_BORDER,
                                     0, 0, 220, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_start = makeButton(L"Start Host", kStartButton, 38, 302, 150);
        g_stop = makeButton(L"Stop", kStopButton, 202, 302, 120);
        makeButton(L"Open received files", kFilesButton, 38, 378, 210);
        makeButton(L"Open config", kConfigButton, 264, 378, 150);
        makeButton(L"Refresh", kRefreshButton, 430, 378, 120);
        makeButton(L"Check updates", kUpdateButton, 568, 378, 150);
        makeStatic(L"Use the upload box for files. Small clipboard copies are still supported.", 38, 450, 700, 30, g_bodyFont);
        SetTimer(window, kTimerId, 1000, nullptr);
        RefreshUi();
        return 0;
    }
    case WM_TIMER:
        RefreshUi();
        return 0;
    case WM_DROPFILES: {
        QueueDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    }
    case kStatusMessage: {
        auto* status = reinterpret_cast<std::wstring*>(lParam);
        if (status) {
            SetStatus(*status);
            delete status;
        }
        return 0;
    }
    case kUpdateFinishedMessage: {
        auto* status = reinterpret_cast<std::wstring*>(lParam);
        if (status) {
            g_updateRunning = false;
            EnableWindow(GetDlgItem(window, kUpdateButton), TRUE);
            SetStatus(*status);
            delete status;
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kStartButton: StartHost(); return 0;
        case kStopButton: StopHost(); return 0;
        case kFilesButton: OpenPath(ReceivedDirectory()); return 0;
        case kConfigButton: {
            const auto config = AppDirectory() / L"MintDeskHost.ini";
            if (!std::filesystem::exists(config)) {
                WritePrivateProfileStringW(L"MintDesk", L"resolution", L"native", config.c_str());
                WritePrivateProfileStringW(L"MintDesk", L"fps", L"60", config.c_str());
            }
            ShellExecuteW(nullptr, L"open", config.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case kRefreshButton: RefreshUi(); return 0;
        case kUpdateButton: CheckForUpdates(window); return 0;
        default: break;
        }
        break;
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(201, 209, 217));
        return reinterpret_cast<LRESULT>(g_panelBrush);
    case WM_CLOSE:
        StopHost();
        if (g_dropZone) {
            DestroyWindow(g_dropZone);
            g_dropZone = nullptr;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerId);
        DeleteObject(g_titleFont);
        DeleteObject(g_bodyFont);
        DeleteObject(g_panelBrush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const wchar_t className[] = L"MintDeskHostAppWindow";
    const wchar_t dropZoneClass[] = L"MintDeskDropZone";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = CreateSolidBrush(RGB(13, 17, 23));
    RegisterClassW(&windowClass);
    WNDCLASSW dropZoneWindowClass{};
    dropZoneWindowClass.hInstance = instance;
    dropZoneWindowClass.lpfnWndProc = DropZoneProc;
    dropZoneWindowClass.lpszClassName = dropZoneClass;
    dropZoneWindowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    dropZoneWindowClass.hbrBackground = CreateSolidBrush(RGB(30, 39, 50));
    RegisterClassW(&dropZoneWindowClass);

    HWND window = CreateWindowExW(
        0,
        className,
        L"MintDesk Host",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        780,
        560,
        nullptr,
        nullptr,
        instance,
        nullptr
    );
    if (!window) {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    if (g_dropZone) {
        RECT hostBounds{};
        GetWindowRect(window, &hostBounds);
        SetWindowPos(g_dropZone, HWND_TOPMOST, hostBounds.right - 245, hostBounds.top + 120,
                     220, 160, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
