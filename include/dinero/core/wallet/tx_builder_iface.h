#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "wallet/tx_size_estimator.h"

namespace din {

struct TxIn  { std::string txid; uint32_t vout; std::optional<std::string> redeem_script; };
struct TxOut { std::string address; int64_t value; }; // una, not floats

enum class TxBuildErr {
  InsufficientFunds,
  InvalidAddress,
  DustOutput,
  InternalError
};

struct TxBuildRequest {
  std::vector<TxIn>  inputs;
  std::vector<TxOut> outputs;
  int64_t fee_rate_una_vb{0};   // una per vbyte
  bool replace_by_fee{false};   // RBF opt-in
};

struct TxBuildResult {
  std::string psbt_base64;
  int64_t fee_paid{0};
  int64_t change_output_value{0};
};

struct ITxBuilder {
  virtual ~ITxBuilder() = default;
  virtual std::optional<TxBuildResult> create(const TxBuildRequest& req, TxBuildErr& err) = 0;
  
  /**
   * @brief Estimate transaction size for fee calculation
   * 
   * @param req Transaction build request
   * @return Estimated size in virtual bytes, or 0 if estimation fails
   */
  virtual uint64_t estimateSize(const TxBuildRequest& req) = 0;
};

} // namespace din