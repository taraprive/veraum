#include "src/net/http_client.h"
#include <iostream>

using namespace hftarb;

int main() {
    HttpClient http("https://www.okx.com");

    // Test 1: public depth (no auth needed)
    auto r1 = http.get("/api/v5/market/books?instId=BTC-USDT&sz=5");
    std::cout << "BTC-USDT depth status=" << r1.status << "\n" << r1.body.substr(0, 500) << "\n\n";

    // Test 2: try ETH
    auto r2 = http.get("/api/v5/market/books?instId=ETH-USDT&sz=5");
    std::cout << "ETH-USDT depth status=" << r2.status << "\n" << r2.body.substr(0, 500) << "\n\n";

    // Test 3: XAU-USDT (might not exist)
    auto r3 = http.get("/api/v5/market/books?instId=XAU-USDT&sz=5");
    std::cout << "XAU-USDT depth status=" << r3.status << "\n" << r3.body.substr(0, 500) << "\n";
}
