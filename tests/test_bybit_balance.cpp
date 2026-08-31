#include "src/util/hmac_sha256.h"
#include "src/core/timestamp.h"
#include "src/net/http_client.h"
#include "src/util/config.h"
#include <iostream>

using namespace hftarb;

int main() {
    AppConfig cfg = ConfigLoader::load("config.json", "exchanges.json", "credentials.json");

    std::string apiKey = cfg.apiKeys.count("bybit") ? cfg.apiKeys.at("bybit") : "";
    std::string apiSecret = cfg.apiSecrets.count("bybit") ? cfg.apiSecrets.at("bybit") : "";

    std::cout << "apiKey=" << apiKey.substr(0,6) << "... apiSecret=" << apiSecret.substr(0,6) << "...\n\n";

    HttpClient http("https://api-testnet.bybit.com");

    auto sign = [&](long long ts, const std::string& payload) -> std::string {
        std::string data = std::to_string(ts) + apiKey + "5000" + payload;
        return hmacSha256Hex(apiSecret, data);
    };

    const long long ts = Timestamps::wallMs();
    std::string query = "accountType=UNIFIED&timestamp=" + std::to_string(ts) + "&recvWindow=5000";
    std::string sig = sign(ts, query);
    HeaderList headers = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", sig},
        {"X-BAPI-TIMESTAMP", std::to_string(ts)},
        {"X-BAPI-RECV-WINDOW", "5000"}
    };

    std::string path = "/v5/account/wallet-balance?" + query;
    auto r = http.get(path, headers);
    std::cout << "wallet-balance (UNIFIED_TRADE) status=" << r.status << "\n" << r.body << "\n\n";

    const long long ts2 = Timestamps::wallMs();
    std::string query2 = "timestamp=" + std::to_string(ts2) + "&recvWindow=5000";
    std::string sig2 = sign(ts2, query2);
    HeaderList headers2 = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", sig2},
        {"X-BAPI-TIMESTAMP", std::to_string(ts2)},
        {"X-BAPI-RECV-WINDOW", "5000"}
    };
    auto r2 = http.get("/v5/account/wallet-balance?" + query2, headers2);
    std::cout << "wallet-balance (all) status=" << r2.status << "\n" << r2.body.substr(0, 2000) << "\n";

    const long long ts3 = Timestamps::wallMs();
    std::string query3 = "accountType=FUND&timestamp=" + std::to_string(ts3) + "&recvWindow=5000";
    std::string sig3 = sign(ts3, query3);
    HeaderList headers3 = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", sig3},
        {"X-BAPI-TIMESTAMP", std::to_string(ts3)},
        {"X-BAPI-RECV-WINDOW", "5000"}
    };
    auto r3 = http.get("/v5/account/wallet-balance?" + query3, headers3);
    std::cout << "\nwallet-balance (FUND) status=" << r3.status << "\n" << r3.body.substr(0, 2000) << "\n";

    const long long ts4 = Timestamps::wallMs();
    std::string query4 = "accountType=SPOT&timestamp=" + std::to_string(ts4) + "&recvWindow=5000";
    std::string sig4 = sign(ts4, query4);
    HeaderList headers4 = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", sig4},
        {"X-BAPI-TIMESTAMP", std::to_string(ts4)},
        {"X-BAPI-RECV-WINDOW", "5000"}
    };
    auto r4 = http.get("/v5/account/wallet-balance?" + query4, headers4);
    std::cout << "\nwallet-balance (SPOT) status=" << r4.status << "\n" << r4.body.substr(0, 2000) << "\n";
}
