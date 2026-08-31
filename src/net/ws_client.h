#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace hftarb {

// Native WebSocket client over Windows WinHTTP (no third-party library).
// Handles the RFC6455 upgrade handshake, text/binary receive (including
// fragmented frames), automatic reconnect with backoff, and re-subscription
// after every reconnect. One instance is used by exactly one feed thread.
class WsClient {
public:
    WsClient(std::string url, std::string subMessage);

    // Callbacks are invoked on the internal receive thread.
    void setMessageCallback(std::function<void(const std::string&)> cb) { onMessage_ = std::move(cb); }
    void setStateCallback(std::function<void(bool connected)> cb) { onState_ = std::move(cb); }
    void setDiagCallback(std::function<void(const std::string&)> cb) { onDiag_ = std::move(cb); }

    // Blocking connect + subscribe. Returns immediately with true if the
    // handshake succeeded; receive/disconnect/reconnect run on a background
    // thread. Returns false if the handshake itself failed.
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_.load(); }

    void send(const std::string& msg);

private:
    void run();
    void teardown();
    void diag(const std::string& d);
    static std::string makeSecWebSocketKey();

    std::string url_;
    std::string subMessage_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::function<void(const std::string&)> onMessage_;
    std::function<void(bool)> onState_;
    std::function<void(const std::string&)> onDiag_;
    std::mutex callbackMutex_;
    std::vector<std::string> outbound_;
};

}  // namespace hftarb