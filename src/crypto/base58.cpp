#include "base58.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>

// Forward declaration for SHA256 - use the existing global function
extern void sha256(const uint8_t* data, size_t len, uint8_t out32[32]);

namespace dinero::b58 {
static constexpr char ALPHABET[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void dbl_sha256(uint8_t out[32], const uint8_t* in, size_t len) {
  uint8_t tmp[32];
  sha256(in, len, tmp);
  sha256(tmp, 32, out);
}

std::string encode_check(const uint8_t* payload, size_t payload_len) {
  uint8_t chk_full[32];
  dbl_sha256(chk_full, payload, payload_len);
  uint8_t chk[4]; std::memcpy(chk, chk_full, 4);

  // Concatenate payload || checksum
  std::vector<uint8_t> buf; buf.reserve(payload_len + 4);
  buf.insert(buf.end(), payload, payload + payload_len);
  buf.insert(buf.end(), chk, chk+4);

  // Count leading zeros
  size_t zeros = 0; while (zeros < buf.size() && buf[zeros] == 0) zeros++;

  // Big number division by 58
  std::vector<uint8_t> tmp(buf.begin(), buf.end());
  std::vector<char> encoded;
  while (!tmp.empty() && !(tmp.size()==1 && tmp[0]==0)) {
    int carry = 0;
    for (size_t i=0;i<tmp.size();i++) {
      int val = (carry << 8) + tmp[i];
      tmp[i] = static_cast<uint8_t>(val / 58);
      carry  = val % 58;
    }
    encoded.push_back(ALPHABET[carry]);
    // strip leading zeros in tmp
    size_t i=0; while (i<tmp.size() && tmp[i]==0) i++;
    tmp.erase(tmp.begin(), tmp.begin()+i);
  }
  // Add leading '1's for each leading zero byte
  std::string out; out.reserve(zeros + encoded.size());
  out.append(zeros, '1');
  for (auto it = encoded.rbegin(); it != encoded.rend(); ++it) out.push_back(*it);
  return out;
}

bool decode_check(const std::string& encoded, std::vector<uint8_t>& payload) {
  payload.clear();
  
  if (encoded.empty()) return false;
  
  // Create character map for decoding
  static int8_t decode_map[256];
  static bool map_initialized = false;
  
  if (!map_initialized) {
    // Initialize all to -1
    std::fill(decode_map, decode_map + 256, -1);
    
    // Map base58 alphabet to values
    for (int i = 0; i < 58; i++) {
      decode_map[static_cast<uint8_t>(ALPHABET[i])] = i;
    }
    map_initialized = true;
  }
  
  // Count leading '1's (zeros)
  size_t leading_zeros = 0;
  for (char c : encoded) {
    if (c != '1') break;
    leading_zeros++;
  }
  
  // Decode base58
  std::vector<uint8_t> temp;
  temp.reserve(encoded.size() * 733 / 1000 + 1); // log(58) / log(256), rounded up
  
  for (char c : encoded) {
    int8_t val = decode_map[static_cast<uint8_t>(c)];
    if (val < 0) return false; // Invalid character
    
    int carry = val;
    for (size_t i = 0; i < temp.size(); i++) {
      carry += temp[i] * 58;
      temp[i] = carry & 0xff;
      carry >>= 8;
    }
    while (carry > 0) {
      temp.push_back(carry & 0xff);
      carry >>= 8;
    }
  }
  
  // Add leading zeros
  payload.assign(leading_zeros, 0);
  
  // Reverse and append
  payload.insert(payload.end(), temp.rbegin(), temp.rend());
  
  // Verify checksum (last 4 bytes)
  if (payload.size() < 4) return false;
  
  size_t payload_len = payload.size() - 4;
  uint8_t chk_full[32];
  dbl_sha256(chk_full, payload.data(), payload_len);
  
  if (std::memcmp(chk_full, payload.data() + payload_len, 4) != 0) {
    return false; // Checksum mismatch
  }
  
  // Remove checksum from payload
  payload.resize(payload_len);
  return true;
}

} // namespace dinero::b58
