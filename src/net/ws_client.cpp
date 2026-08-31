#include "src/net/ws_client.h"

#include <winhttp.h>

#include <cstdlib>
#include <cstring>
#include <thread>

namespace hftarb {

namespace {

std::wstring toWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

std::string wsLastWinError(const char* what) {
    return std::string(what) + " (winerr " + std::to_string(GetLastError()) + ")";
}

std::string parseWsUrl(const std::string& url, std::string& host, std::string& path,
                       unsigned short& port, bool& useTls) {
    std::string s = url;
    if (s.rfind("wss://", 0) == 0) {
        useTls = true;
        s = s.substr(6);
    } else if (s.rfind("ws://", 0) == 0) {
        useTls = false;
        s = s.substr(5);
    } else {
        useTls = true;
    }
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        path = s.substr(slash);
        if (path.empty()) path = "/";
        s = s.substr(0, slash);
    } else {
        path = "/";
    }
    port = useTls ? 443 : 80;
    auto colon = s.rfind(':');
    if (colon != std::string::npos && colon + 1 < s.size()) {
        try {
            port = static_cast<unsigned short>(std::stoi(s.substr(colon + 1)));
            s = s.substr(0, colon);
        } catch (...) {
            return "bad port in " + url;
        }
    } else if (colon != std::string::npos) {
        return "bad port in " + url;
    }
    host = s;
    return "";
}

// RFC6455 Sec-WebSocket-Key: base64 of 16 random bytes.
std::string randomWebSocketKey() {
    unsigned char bytes[16];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<unsigned char>(rand());
    }
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(24);
    for (int i = 0; i < 16; i += 3) {
        unsigned v = static_cast<unsigned>(bytes[i]) << 16;
        if (i + 1 < 16) v |= static_cast<unsigned>(bytes[i + 1]) << 8;
        if (i + 2 < 16) v |= static_cast<unsigned>(bytes[i + 2]);
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += (i + 1 < 16) ? b64[(v >> 6) & 63] : '=';
        out += (i + 2 < 16) ? b64[v & 63] : '=';
    }
    return out;
}

}  // namespace

WsClient::WsClient(std::string url, std::string subMessage)
    : url_(std::move(url)), subMessage_(std::move(subMessage)) {}

bool WsClient::connect() {
    if (running_.exchange(true)) return connected_.load();
    thread_ = std::thread(&WsClient::run, this);
    return true;
}

void WsClient::disconnect() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void WsClient::send(const std::string& msg) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    outbound_.push_back(msg);
}

void WsClient::diag(const std::string& d) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onDiag_) onDiag_(d);
}

std::string WsClient::makeSecWebSocketKey() {
    return randomWebSocketKey();
}

void WsClient::run() {
    while (running_.load()) {
        std::string host, path, err;
        unsigned short port = 443;
        bool useTls = true;
        err = parseWsUrl(url_, host, path, port, useTls);
        if (!err.empty()) {
            diag(err);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }

        diag("WinHttpOpen");
        HINTERNET session = WinHttpOpen(L"HFT-Arbitrage/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            diag("WinHttpOpen failed: " + wsLastWinError("open"));
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 0);
        HINTERNET conn = WinHttpConnect(session, toWide(host).c_str(), port, 0);
        if (!conn) diag("WinHttpConnect failed: " + wsLastWinError("conn"));
        HINTERNET req = nullptr;
        if (conn) {
            req = WinHttpOpenRequest(conn, L"GET", toWide(path).c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     useTls ? WINHTTP_FLAG_SECURE : 0);
            if (!req) diag("WinHttpOpenRequest failed: " + wsLastWinError("req"));
            // Mark the request as a WebSocket upgrade so WinHTTP uses HTTP/1.1
            // upgrade semantics instead of negotiating HTTP/2 (which would break
            // the RFC6455 handshake and fail WinHttpReceiveResponse with 12152).
            if (req &&
                !WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
                diag("SetOption UPGRADE_TO_WEB_SOCKET failed: " + wsLastWinError("opt"));
        }
        HINTERNET ws = nullptr;
        if (req) {
            std::wstring hdr = L"Connection: Upgrade\r\nUpgrade: websocket\r\n"
                               L"Sec-WebSocket-Version: 13\r\n"
                               L"Sec-WebSocket-Key: " + toWide(randomWebSocketKey()) + L"\r\n";
            BOOL sent = WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1, nullptr, 0, 0, 0);
            if (!sent) diag("WinHttpSendRequest failed: " + wsLastWinError("send"));
            if (sent && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0;
                DWORD statusLen = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                                    WINHTTP_NO_HEADER_INDEX);
                diag("handshake status=" + std::to_string(status));
                if (status == 101) {
                    ws = WinHttpWebSocketCompleteUpgrade(req, 0);
                    if (!ws) diag("CompleteUpgrade failed: " + wsLastWinError("upgrade"));
                }
            } else if (sent) {
                diag("WinHttpReceiveResponse failed: " + wsLastWinError("recv"));
            }
        }
        if (!ws) {
            if (conn) WinHttpCloseHandle(conn);
            if (req) WinHttpCloseHandle(req);
            WinHttpCloseHandle(session);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        // The request handle has served its purpose; per the MSDN WebSocket
        // sample it is released once the upgrade completes. The connection
        // handle must remain open for the life of the WebSocket (WS frames are
        // carried over that authenticated TCP/TLS connection).
        if (req) WinHttpCloseHandle(req);
        diag("connected");

        // Send subscription (and any queued outbound control frames).
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (!subMessage_.empty()) {
                WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     const_cast<char*>(subMessage_.data()),
                                     static_cast<DWORD>(subMessage_.size()));
            }
            for (const auto& m : outbound_) {
                WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     const_cast<char*>(m.data()),
                                     static_cast<DWORD>(m.size()));
            }
            outbound_.clear();
        }

        connected_.store(true);
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (onState_) onState_(true);
        }

        // Receive loop; reassemble fragmented messages until the final frame.
        std::string fragment;
        char buf[65536];
        bool wsOpen = true;
        while (wsOpen && running_.load()) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            // WinHttpWebSocketReceive returns a WinHTTP error code (DWORD), not
            // a BOOL: ERROR_SUCCESS (0) means a buffer was received.
            DWORD wsErr = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &bytesRead, &type);
            if (wsErr != ERROR_SUCCESS) {
                diag("WebSocketReceive failed: " + std::to_string(wsErr));
                break;
            }
            switch (type) {
                case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
                case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: {
                    std::string msg;
                    msg.reserve(fragment.size() + bytesRead);
                    msg += fragment;
                    msg.append(buf, bytesRead);
                    fragment.clear();
                    // OKX keepalive: the server sends a bare text "ping" every
                    // ~30s and the client must reply with a bare "pong".
                    if (msg == "ping") {
                        WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             const_cast<char*>("pong"), 4);
                    } else {
                        std::lock_guard<std::mutex> lock(callbackMutex_);
                        if (onMessage_) onMessage_(msg);
                    }
                    break;
                }
                case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
                case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                    fragment.append(buf, bytesRead);
                    break;
                case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
                    wsOpen = false;
                    break;
            }
        }

        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (onState_) onState_(false);
        }
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);

        if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

void WsClient::teardown() {
    running_.store(false);
}

}  // namespace hftarb