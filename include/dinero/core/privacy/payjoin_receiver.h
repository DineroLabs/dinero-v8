#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <array>

namespace din {

struct ExecutionContext; // your existing context

// Spendable UTXO owned by *receiver* (this node's wallet)
struct ReceiverUtxo {
  std::string txid;                // hex (big-endian display)
  uint32_t    vout{0};
  int64_t     value{0};            // una
  std::vector<uint8_t> scriptPubKey; // raw spk
  enum class Type { P2WPKH, P2TR } type{Type::P2WPKH};
  // Optional derivation for PSBT in/out metadata
  std::vector<uint8_t> pubkey33;       // for P2WPKH
  std::array<uint8_t,32> xonly{};      // for P2TR
  std::array<uint8_t,4>  master_fpr{}; // BIP32 master fingerprint
  std::vector<uint32_t>  path;         // derivation path
};

// Change script provider (receiver's wallet provides a destination for change)
using GetChangeScriptFn = std::function<std::vector<uint8_t>()>;

// Pick a receiver UTXO (policy: confirmed, not locked, >= min)
using SelectUtxoFn = std::function<std::optional<ReceiverUtxo>(int64_t min_value)>;

struct PayjoinOffer {
  std::string endpoint; // e.g., https://host:port/payjoin
  int64_t amount;       // invoice amount (una)
  std::vector<uint8_t> invoice_spk; // scriptPubKey expected in the sender's tx output
};

class PayjoinReceiver {
public:
  PayjoinReceiver(ExecutionContext* ctx,
                  PayjoinOffer offer,
                  SelectUtxoFn select,
                  GetChangeScriptFn get_change_spk,
                  int64_t min_relay_feerate_una_vb = 1);
  // Merge + sign receiver input, return updated PSBT (base64)
  // form_psbt_b64: the sender POSTs PSBT b64
  // min_feerate_una_vb: sender's required minimum feerate (fallback to min_relay if <=0)
  // disable_output_substitution: if true, we will *only* touch the payee output + add one change
  std::string handle(const std::string& form_psbt_b64,
                     int64_t min_feerate_una_vb,
                     bool disable_output_substitution);

private:
  ExecutionContext* ctx_;
  PayjoinOffer offer_;
  SelectUtxoFn select_;
  GetChangeScriptFn get_change_spk_;
  int64_t min_relay_feerate_una_vb_;

  // helpers (implemented in .cpp)
  static int64_t dust_threshold_p2wpkh(int64_t min_relay_feerate_una_vb);
  static int64_t dust_threshold_p2tr(int64_t min_relay_feerate_una_vb);
};

} // namespace din
