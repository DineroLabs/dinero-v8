#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace din {

struct ExecutionContext; // your existing DI root

// Spendable input the wallet is willing to expose in a CoinJoin round.
struct CJInput {
  std::string txid; // hex (display)
  uint32_t    vout{0};
  int64_t     value{0}; // una
  enum class Type { P2WPKH, P2TR } type{Type::P2WPKH};

  // Optional derivation/pubkey metadata for PSBT signing (not sent to coordinator unless protocol needs)
  std::vector<uint8_t> pubkey33;        // for P2WPKH
  std::array<uint8_t,32> xonly{};       // for P2TR
  std::array<uint8_t,4>  master_fpr{};  // BIP32 master fingerprint
  std::vector<uint32_t>  path;          // derivation path
};

// Wallet-provided hooks
using SelectInputsFn = std::function<std::vector<CJInput>(int64_t target_value, int64_t feerate_una_vb)>;
using BuildEqualOutputScriptFn = std::function<std::vector<uint8_t>()>; // returns scriptPubKey for our equal-amount output
using BuildChangeScriptFn      = std::function<std::vector<uint8_t>()>;
using SignPsbtFn               = std::function<std::string(const std::string& psbt_b64)>; // returns updated (partially) signed PSBT b64

struct CJParams {
  std::string coordinator_url;  // base URL; e.g., https://host:port/cj
  int64_t     target_amt;       // una for equal output
  int64_t     fee_rate_una_vb;  // una/vB target (min)
  int         min_peers{3};     // minimal peers for round
};

enum class CJPhase { Unknown, Registering, Waiting, PsbtReady, Signing, Submitted, Done, Failed };

struct CJStatus {
  std::string round_id;
  CJPhase     phase{CJPhase::Unknown};
  int         peers{0};
  bool        done{false};
  std::string detail;       // human-readable status/reason
  std::string psbt_b64;     // present in PsbtReady/Signing
  std::string txid;         // filled in Done
};

class CoinJoinClient {
public:
  CoinJoinClient(ExecutionContext* ctx,
                 SelectInputsFn select_inputs,
                 BuildEqualOutputScriptFn build_equal_spk,
                 BuildChangeScriptFn build_change_spk,
                 SignPsbtFn sign_psbt);

  // Start a round: registers our inputs & desired equal-output script with the coordinator. Returns round_id.
  std::string join_round(const CJParams& p);

  // Poll progress and perform automatic steps (fetch PSBT, sign, submit) when appropriate.
  CJStatus poll(const std::string& round_id);

  // Ask coordinator to cancel our participation.
  bool cancel(const std::string& round_id);

private:
  ExecutionContext* ctx_;
  SelectInputsFn select_inputs_;
  BuildEqualOutputScriptFn build_equal_spk_;
  BuildChangeScriptFn build_change_spk_;
  SignPsbtFn sign_psbt_;

  // Internal helpers
  CJStatus fetch_status(const std::string& base, const std::string& round_id);
  std::string fetch_psbt(const std::string& base, const std::string& round_id);
  bool submit_signed(const std::string& base, const std::string& round_id, const std::string& psbt_b64);
  std::string http_get_json(const std::string& url_path);
  std::string http_post_json(const std::string& url_path, const std::string& json_body);
};

} // namespace din
