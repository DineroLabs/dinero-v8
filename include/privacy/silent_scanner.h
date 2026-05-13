#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include "privacy/silent_payments.h"

namespace din::sp {

struct TxView {
  // Simplified view: extract eligible input pubkeys & all taproot outputs
  std::vector<std::array<uint8_t,32>> tap_outputs_xonly; // outputs
  std::vector<std::array<uint8_t,33>> input_pubkeys_sec1; // P2WPKH/P2PKH/etc
  std::vector<std::array<uint8_t,32>> input_tap_xonly;    // P2TR inputs' output keys
  std::array<uint8_t,36> outpoint_L_le{};
};

struct Detection {
  size_t output_index;
  std::array<uint8_t,32> tap_output_xonly;
  std::array<uint8_t,32> spend_priv_candidate; // b_spend + t_k  (computed offline or cached)
};

class Scanner {
public:
  explicit Scanner(const std::array<uint8_t,32>& scan_priv,
                   const std::array<uint8_t,33>& spend_pub);
  // Returns matches for this tx (usually 0 or 1)
  std::vector<Detection> scan_tx(const TxView& tx);
private:
  std::array<uint8_t,32> b_scan_;
  std::array<uint8_t,33> B_spend_;
};

} // namespace din::sp
