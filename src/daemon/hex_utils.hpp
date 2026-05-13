#pragma once
#include <string>
#include <cstdint>

namespace dinero::hex {
  // Converts 32 bytes to 64-character uppercase hex string
  // Guarantees canonical representation for database/RPC/logs
  inline std::string hex32(const uint8_t* data) {
    static const char* H = "0123456789ABCDEF";
    std::string s; 
    s.reserve(64);
    for (int i = 0; i < 32; ++i) { 
      uint8_t b = data[i]; 
      s.push_back(H[b >> 4]); 
      s.push_back(H[b & 0xF]); 
    }
    return s;
  }
}
