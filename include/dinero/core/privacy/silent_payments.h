#pragma once
#include <array>
#include <string>
#include <vector>

namespace din::sp {

struct Address {
  // Compressed SEC1 pubkeys (33 bytes each)
  std::array<uint8_t,33> scan_pub{};
  std::array<uint8_t,33> spend_pub{}; // already label-tweaked if you support labels
};

enum class Net { Main, Test, Regtest };

std::string encode_bech32m(const Address& a, Net n);   // HRP: dsp/tdsp/rdsp
Address      decode_bech32m(const std::string& s, Net expected);

// Utility: derive labeled spend pubkey: B_m = B_spend + H(label)·G
std::array<uint8_t,33> label_spend_pub(const std::array<uint8_t,33>& spend_pub,
                                       const std::array<uint8_t,32>& scan_priv,
                                       uint32_t label);

} // namespace din::sp
