#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

namespace dinero::b58 {
// Base58Check: versioned payload -> Base58 string
std::string encode_check(const uint8_t* payload, size_t len);

// Base58Check: Base58 string -> versioned payload
bool decode_check(const std::string& encoded, std::vector<uint8_t>& payload);

// Convenience: 4-byte version + 74-byte key payload (xpub/xprv)
inline std::string encode_check(const uint8_t* ver4, const uint8_t* data, size_t n) {
  std::string s; s.resize(4+n);
  std::memcpy(s.data(), ver4, 4);
  std::memcpy(s.data()+4, data, n);
  return encode_check(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
} // namespace dinero::b58
