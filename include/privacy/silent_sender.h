#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include "privacy/silent_payments.h"

namespace din::sp {

struct InputKey {
  // X-only P for P2TR; compressed SEC1 for legacy segwit/PKH
  std::array<uint8_t,32> tap_xonly{}; // for P2TR
  std::array<uint8_t,33> sec1{};      // for non-tap inputs
  bool is_tap{false};
  // The corresponding *private* key (host or HWW must supply the even-Y variant for taproot)
  std::array<uint8_t,32> priv{};
};

struct DeriveParams {
  std::vector<InputKey> inputs;
  std::array<uint8_t,36> outpoint_L_le{};   // txid(LE)||vout(LE) of smallest outpoint
  Address receiver;                          // scan+spend
  uint32_t k_index{0};                       // 0 for first output
};

// Returns x-only 32-byte taproot output key for P_k
std::array<uint8_t,32> derive_sp_taproot_key(const DeriveParams&);

} // namespace din::sp
