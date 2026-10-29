#pragma once
#include "consensus/orchard_coin_snapshot.h"
#include "primitives/transaction_reader.h"

namespace dinero::consensus {
// Draft post-activation profile. No historical/live rule changes. Final loaded
// node/platform qualification must confirm these limits before activation.
inline constexpr uint32_t kOrchardMaxBlockBundles = 8;
inline constexpr uint32_t kOrchardMaxBlockActions = 32;
inline constexpr uint32_t kOrchardMaxBlockSigops = 80000;
struct OrchardResourceUsage {
  uint32_t bundles = 0;
  uint32_t actions = 0;
  uint32_t sigops = 0;
  bool operator==(const OrchardResourceUsage &) const = default;
};
enum class OrchardResourceErrorCode { Bundles, Actions, Sigops, InputShape };
class OrchardResourceError : public std::runtime_error {
public:
  explicit OrchardResourceError(OrchardResourceErrorCode code)
      : std::runtime_error(
            "Orchard block resource budget exceeded or malformed"),
        code_(code) {}
  OrchardResourceErrorCode Code() const noexcept { return code_; }

private:
  OrchardResourceErrorCode code_;
};
// Bounded opcode walk. Push data never counts as opcodes. Truncated pushes end
// counting; script validity is checked separately. No pointer beyond the span.
uint32_t CountOrchardProfileScriptSigops(std::span<const uint8_t> script,
                                         bool accurate_multisig,
                                         bool tapscript);
// Transactional accumulators: failure leaves usage unchanged. First charge all
// transaction bodies in a block BEFORE any signature/proof verification. Then
// charge authenticated, ordered prevout input work before verifying each tx.
// These do not validate scripts, proofs, prevout provenance or spendability.
void AccumulateOrchardTransactionResources(const ParsedTransaction &,
                                           OrchardResourceUsage &);
void AccumulateOrchardInputResources(const ParsedTransaction &,
                                     std::span<const UTXOEntry>,
                                     OrchardResourceUsage &);
} // namespace dinero::consensus
