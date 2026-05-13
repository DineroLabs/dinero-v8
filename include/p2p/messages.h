#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <boost/endian/conversion.hpp>
#include "version.h"

namespace din::p2p {

constexpr uint32_t NET_MAGIC = 0xD1A0C0DE; // change per network
inline std::array<char,12> cmd(const char* s){
  std::array<char,12> a{}; std::strncpy(a.data(), s, 12); return a;
}

struct MsgHeader {
  uint32_t magic;
  std::array<char,12> command{};
  uint32_t length{0};
  uint32_t checksum{0};
};

struct Version {
  int32_t  protocol{70016};
  uint64_t services{1}; // NODE_NETWORK
  int64_t  timestamp{0};
  uint64_t nonce{0};
  std::string user_agent{DineroUserAgent()};
  int32_t  start_height{0};
  bool     relay{true};
};

// ---- VarInt / VarStr helpers
inline void put_u8 (std::vector<uint8_t>& b, uint8_t v){ b.push_back(v); }
inline void put_u16(std::vector<uint8_t>& b, uint16_t v){ v=boost::endian::native_to_little(v); auto p=(uint8_t*)&v; b.insert(b.end(),p,p+2); }
inline void put_u32(std::vector<uint8_t>& b, uint32_t v){ v=boost::endian::native_to_little(v); auto p=(uint8_t*)&v; b.insert(b.end(),p,p+4); }
inline void put_u64(std::vector<uint8_t>& b, uint64_t v){ v=boost::endian::native_to_little(v); auto p=(uint8_t*)&v; b.insert(b.end(),p,p+8); }
inline void put_i32(std::vector<uint8_t>& b, int32_t v){ put_u32(b, (uint32_t)v); }
inline void put_i64(std::vector<uint8_t>& b, int64_t v){ put_u64(b, (uint64_t)v); }

inline void put_varint(std::vector<uint8_t>& b, uint64_t v){
  if (v < 0xFD) put_u8(b,(uint8_t)v);
  else if (v <= 0xFFFF){ put_u8(b,0xFD); put_u16(b,(uint16_t)v); }
  else if (v <= 0xFFFFFFFF){ put_u8(b,0xFE); put_u32(b,(uint32_t)v); }
  else { put_u8(b,0xFF); put_u64(b,v); }
}
inline void put_varstr(std::vector<uint8_t>& b, const std::string& s){
  put_varint(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}

// ---- Serialize version (MVP)
inline std::vector<uint8_t> serialize_version(const Version& v){
  std::vector<uint8_t> b; b.reserve(120);
  put_i32(b, v.protocol);
  put_u64(b, v.services);
  put_i64(b, v.timestamp);
  // addr_recv (26 bytes) zeros (services+ip+port) – skip for 3A
  for (int i=0;i<26;i++) b.push_back(0);
  // addr_from (26 bytes) zeros
  for (int i=0;i<26;i++) b.push_back(0);
  put_u64(b, v.nonce);
  put_varstr(b, v.user_agent);
  put_i32(b, v.start_height);
  put_u8 (b, v.relay ? 1 : 0);
  return b;
}


} // namespace din::p2p
