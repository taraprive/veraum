#include "src/exchange/okx_adapter.h"
#include <iostream>

using namespace hftarb;

int main() {
    OkxAdapter okx("https://www.okx.com",
                    "41c4744c-31da-4a81-ac3b-76a91bf490d2",
                    "6E829AF9BF527AA66B2E529633E0AFC0",
                    "Wwe121212$", true);

    auto balances = okx.fetchBalances();
    if (balances) {
        std::cout << "OKX balances:\n";
        for (const auto& [asset, qty] : *balances) {
            std::cout << "  " << asset << " = " << qty << "\n";
        }
    } else {
        std::cout << "Failed to fetch balances\n";
    }

    // Also test depth
    auto depth = okx.fetchDepth("BTC/USDT", 5);
    if (depth) {
        std::cout << "\nBTC/USDT depth: " << depth->bids.size() << " bids, "
                  << depth->asks.size() << " asks\n";
        if (!depth->bids.empty()) std::cout << "  best bid: " << depth->bids[0].first << "\n";
        if (!depth->asks.empty()) std::cout << "  best ask: " << depth->asks[0].first << "\n";
    } else {
        std::cout << "Failed to fetch depth\n";
    }
}
