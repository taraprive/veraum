#include <chrono>
#include <iostream>
#include <string>

#include "src/net/ws_client.h"

using namespace hftarb;

namespace {
void probe(const std::string& label, const std::string& url, const std::string& sub,
           int maxMsgs, int timeoutMs) {
    std::cout << "\n=== " << label << " ===\n  url=" << url << "\n  sub=" << sub << "\n";
    WsClient ws(url, sub);
    int got = 0;
    ws.setStateCallback([](bool up) { std::cout << "  [state] " << (up ? "UP" : "DOWN") << "\n"; });
    ws.setDiagCallback([](const std::string& d) { std::cout << "  [diag] " << d << "\n"; });
    ws.setMessageCallback([&](const std::string& msg) {
        if (got < maxMsgs) {
            std::cout << "  [msg " << got << "] len=" << msg.size() << " " << msg.substr(0, 500) << "\n";
        }
        ++got;
    });
    ws.connect();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < maxMsgs && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws.disconnect();
    std::cout << "  got=" << got << "\n";
}
}  // namespace

int main() {
    probe("binance-testnet combined depth10 (correct host)",
          "wss://stream.testnet.binance.vision/stream?streams=btcusdt@depth10/ethusdt@depth10",
          "", 2, 15000);

    probe("okx books5 pepe+shib",
          "wss://ws.okx.com:8443/ws/v5/public",
          R"({"op":"subscribe","args":[{"channel":"books5","instId":"PEPE-USDT"},{"channel":"books5","instId":"SHIB-USDT"}]})",
          3, 15000);

    probe("mexc increase.depth btc (wbs-api)",
          "wss://wbs-api.mexc.com/ws",
          R"({"method":"SUBSCRIPTION","params":["spot@public.increase.depth.v3.api.btcusdt"]})",
          2, 15000);

    std::cout << "\ndone\n";
    return 0;
}