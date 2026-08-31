#include "src/util/hmac_sha256.h"

#include <windows.h>
#include <bcrypt.h>

namespace hftarb {

std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    NTSTATUS st = 0;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[32] = {0};

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                     BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st == 0) {
        st = BCryptCreateHash(alg, &hash, nullptr, 0,
                              reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                              static_cast<ULONG>(key.size()), 0);
    }
    if (st == 0) {
        st = BCryptHashData(hash,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                            static_cast<ULONG>(data.size()), 0);
    }
    if (st == 0) {
        st = BCryptFinishHash(hash, digest, static_cast<ULONG>(sizeof(digest)), 0);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return {};

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = kHex[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

std::string hmacSha256Base64(const std::string& key, const std::string& data) {
    NTSTATUS st = 0;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[32] = {0};

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                     BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st == 0) {
        st = BCryptCreateHash(alg, &hash, nullptr, 0,
                              reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                              static_cast<ULONG>(key.size()), 0);
    }
    if (st == 0) {
        st = BCryptHashData(hash,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                            static_cast<ULONG>(data.size()), 0);
    }
    if (st == 0) {
        st = BCryptFinishHash(hash, digest, static_cast<ULONG>(sizeof(digest)), 0);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return {};

    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.resize(44);
    for (int i = 0; i < 32; i += 3) {
        unsigned int n = (static_cast<unsigned>(digest[i]) << 16);
        if (i + 1 < 32) n |= (static_cast<unsigned>(digest[i + 1]) << 8);
        if (i + 2 < 32) n |= static_cast<unsigned>(digest[i + 2]);
        int idx = i / 3 * 4;
        out[idx]     = kTable[(n >> 18) & 0x3F];
        out[idx + 1] = kTable[(n >> 12) & 0x3F];
        out[idx + 2] = (i + 1 < 32) ? kTable[(n >> 6) & 0x3F] : '=';
        out[idx + 3] = (i + 2 < 32) ? kTable[n & 0x3F] : '=';
    }
    return out;
}

}  // namespace hftarb
