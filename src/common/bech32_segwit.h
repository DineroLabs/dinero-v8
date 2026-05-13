#pragma once
#include "bech32.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace segwit {

// Forward declaration of convertbits from bech32.cpp
bool convertbits(std::vector<uint8_t>& out, const std::vector<uint8_t>& in, int frombits, int tobits, bool pad);

inline bool ConvertBits_5to8(std::vector<uint8_t>& out,
                             const std::vector<uint8_t>::const_iterator begin,
                             const std::vector<uint8_t>::const_iterator end) {
  std::vector<uint8_t> input(begin, end);
  return bech32::convertbits(out, input, 5, 8, false); // no pad on decode
}

inline void ConvertBits_8to5(std::vector<uint8_t>& out,
                             const uint8_t* p, const uint8_t* q) {
  std::vector<uint8_t> input(p, q);
  bech32::convertbits(out, input, 8, 5, true); // pad on encode
}

inline bech32::Encoding encoding_for_witver(int v) {
  // BIP-350: v0 -> bech32; v>=1 -> bech32m
  return v == 0 ? bech32::Encoding::BECH32 : bech32::Encoding::BECH32M;
}

inline bool DecodeSegwit(const std::string& hrp_expected,
                         const std::string& addr,
                         int& witver_out,
                         std::vector<uint8_t>& program_out,
                         bech32::Encoding& enc_out) {
  // Use bech32::Decode which properly handles checksum validation and program extraction
  auto result = bech32::Decode(hrp_expected, addr);
  if (!result) return false;

  // **Checksum variant must match witness version** (BIP-350)
  if ((result->witver == 0 && result->encoding != bech32::Encoding::BECH32) ||
      (result->witver >  0 && result->encoding != bech32::Encoding::BECH32M)) return false;

  witver_out = result->witver; 
  program_out = result->program; 
  enc_out = result->encoding;
  return true;
}

inline std::string EncodeSegwit(const std::string& hrp, int witver,
                                const std::vector<uint8_t>& program) {
  if (witver == 0 && !(program.size() == 20 || program.size() == 32))
    throw std::runtime_error("invalid v0 program len");

  std::vector<uint8_t> data;
  data.push_back(static_cast<uint8_t>(witver));
  ConvertBits_8to5(data, program.data(), program.data()+program.size());
  return bech32::Encode(hrp, witver, program, encoding_for_witver(witver)); // Pass encoding
}

} // namespace segwit