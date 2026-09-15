#include <jni.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <android/log.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

namespace {
constexpr const char* kTag = "MintDeskNativeGns";

class GnsHelloClient {
public:
    std::string run(const std::string& host, int port) {
        log_ = "GNS client starting: " + host + ":" + std::to_string(port) + "\n";

        SteamDatagramErrMsg errMsg{};
        if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
            return std::string("GNS init failed: ") + errMsg;
        }

        sockets_ = SteamNetworkingSockets();
        if (sockets_ == nullptr) {
            GameNetworkingSockets_Kill();
            return "SteamNetworkingSockets() returned null";
        }

        SteamNetworkingIPAddr address{};
        if (!address.ParseString(host.c_str())) {
            GameNetworkingSockets_Kill();
            return "Invalid host/IP: " + host;
        }
        address.m_port = static_cast<uint16>(port);

        SteamNetworkingConfigValue_t option{};
        option.SetPtr(
                k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                reinterpret_cast<void*>(connectionStatusChanged)
        );

        active_ = this;
        connection_ = sockets_->ConnectByIPAddress(address, 1, &option);
        if (connection_ == k_HSteamNetConnection_Invalid) {
            active_ = nullptr;
            GameNetworkingSockets_Kill();
            return "ConnectByIPAddress failed";
        }

        const SteamNetworkingMicroseconds startUs =
                SteamNetworkingUtils()->GetLocalTimestamp();
        while (!done_) {
            pollMessages();
            sockets_->RunCallbacks();

            const SteamNetworkingMicroseconds nowUs =
                    SteamNetworkingUtils()->GetLocalTimestamp();
            if (nowUs - startUs > 5000000) {
                log_ += "Timed out waiting for GNS hello.\n";
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (connection_ != k_HSteamNetConnection_Invalid) {
            sockets_->CloseConnection(connection_, 0, "android client finished", true);
            connection_ = k_HSteamNetConnection_Invalid;
        }

        active_ = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        GameNetworkingSockets_Kill();
        return log_;
    }

private:
    static GnsHelloClient* active_;

    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    bool done_ = false;
    bool sentHello_ = false;
    std::string log_;

    static void connectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
        if (active_ != nullptr) {
            active_->onConnectionStatusChanged(info);
        }
    }

    void onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                log_ += "Connecting...\n";
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                log_ += "Connected.\n";
                send("hello from MintDesk Android GNS client");
                sentHello_ = true;
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                log_ += std::string("Disconnected: ") + info->m_info.m_szEndDebug + "\n";
                if (connection_ == info->m_hConn) {
                    connection_ = k_HSteamNetConnection_Invalid;
                }
                done_ = true;
                break;

            default:
                break;
        }
    }

    void send(const char* text) {
        sockets_->SendMessageToConnection(
                connection_,
                text,
                static_cast<uint32>(std::strlen(text)),
                k_nSteamNetworkingSend_Reliable,
                nullptr
        );
        log_ += std::string("Sent: ") + text + "\n";
    }

    void pollMessages() {
        while (!done_ && connection_ != k_HSteamNetConnection_Invalid) {
            ISteamNetworkingMessage* message = nullptr;
            const int count = sockets_->ReceiveMessagesOnConnection(connection_, &message, 1);
            if (count == 0) {
                break;
            }
            if (count < 0) {
                log_ += "ReceiveMessagesOnConnection failed.\n";
                done_ = true;
                break;
            }

            std::string text(
                    static_cast<const char*>(message->m_pData),
                    static_cast<size_t>(message->m_cbSize)
            );
            log_ += "Received: " + text + "\n";
            message->Release();

            if (sentHello_) {
                done_ = true;
            }
        }
    }
};

GnsHelloClient* GnsHelloClient::active_ = nullptr;

std::string RunGnsSelfTest() {
    SteamDatagramErrMsg errMsg{};
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        return std::string("GNS init failed: ") + errMsg;
    }

    ISteamNetworkingUtils* utils = SteamNetworkingUtils();
    const SteamNetworkingMicroseconds nowUs =
            utils != nullptr ? utils->GetLocalTimestamp() : 0;

    GameNetworkingSockets_Kill();

    return "GNS init OK, timestampUs=" + std::to_string(nowUs);
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mintdesk_client_NativeGns_selfTest(JNIEnv* env, jobject /* thiz */) {
    const std::string result = RunGnsSelfTest();
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", result.c_str());
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mintdesk_client_NativeGns_clientHello(
        JNIEnv* env,
        jobject /* thiz */,
        jstring host,
        jint port
) {
    const char* rawHost = env->GetStringUTFChars(host, nullptr);
    const std::string hostText = rawHost != nullptr ? rawHost : "";
    env->ReleaseStringUTFChars(host, rawHost);

    GnsHelloClient client;
    const std::string result = client.run(hostText, static_cast<int>(port));
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", result.c_str());
    return env->NewStringUTF(result.c_str());
}
