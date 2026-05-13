#pragma once
#include <string>
#include <vector>
#include <cctype>
namespace util {
inline std::string hex(const std::vector<unsigned char>& v){
  static const char* H="0123456789abcdef";
  std::string s; s.resize(v.size()*2);
  for (size_t i=0;i<v.size();++i){ s[2*i]=H[(v[i]>>4)&0xF]; s[2*i+1]=H[v[i]&0xF]; }
  return s;
}
inline bool unhex(const std::string& in, std::vector<unsigned char>& out){
  auto hexv=[](char c)->int{ if('0'<=c&&c<='9')return c-'0'; c=std::tolower(c);
    if('a'<=c&&c<='f')return 10+c-'a'; return -1; };
  if(in.size()%2) return false; out.clear(); out.reserve(in.size()/2);
  for(size_t i=0;i<in.size();i+=2){ int hi=hexv(in[i]), lo=hexv(in[i+1]); if(hi<0||lo<0) return false; out.push_back((hi<<4)|lo); }
  return true;
}

// Alias for compatibility
inline std::vector<uint8_t> HexToBytes(const std::string& hex_str) {
  std::vector<unsigned char> result;
  unhex(hex_str, result);
  return std::vector<uint8_t>(result.begin(), result.end());
}
}
