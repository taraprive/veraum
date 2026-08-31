#include "test_framework.h"

#include "src/util/hmac_sha256.h"

using namespace hftarb;

TEST(hmac_rfc4231_case1) {
    // RFC 4231 test case 1: 20-byte key of 0x0b, data "Hi There".
    const std::string key(20, static_cast<char>(0x0b));
    const std::string hex = hmacSha256Hex(key, "Hi There");
    CHECK(hex == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(hmac_rfc4231_case2) {
    // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?".
    const std::string hex = hmacSha256Hex("Jefe", "what do ya want for nothing?");
    CHECK(hex == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(hmac_empty_inputs) {
    const std::string hex = hmacSha256Hex("", "");
    CHECK(hex == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

TEST(hmac_hex_length_and_case) {
    const std::string hex = hmacSha256Hex("key", "value");
    CHECK(hex.size() == 64);
    bool lower = true;
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) lower = false;
    }
    CHECK(lower);
}
