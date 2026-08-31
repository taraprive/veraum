#pragma once

#include <string>

namespace hftarb {

// RFC 2104 HMAC-SHA256 built on the Windows CNG provider (bcrypt.dll).
// Returns the 32-byte digest as a 64-char lowercase hex string.
// On provider/alloc failure returns an empty string.
std::string hmacSha256Hex(const std::string& key, const std::string& data);

// Same HMAC-SHA256 but returns the 32-byte digest as a base64-encoded string.
std::string hmacSha256Base64(const std::string& key, const std::string& data);

}  // namespace hftarb
