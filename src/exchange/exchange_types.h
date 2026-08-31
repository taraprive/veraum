#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hftarb {

struct DepthBook {
    std::string symbol;
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;
    long long lastUpdateId = 0;
};

}  // namespace hftarb
