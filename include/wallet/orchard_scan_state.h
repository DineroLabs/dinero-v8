#pragma once
#include "consensus/orchard_state_transition.h"
#include "orchard_wallet.h"
#include "orchard_wallet_storage.h"
#include "primitives/orchard_block_reader.h"
#include <memory>
namespace dinero::wallet {
struct ScannedOrchardNote {
  std::shared_ptr<const orchard::WalletNote> note;
  std::shared_ptr<const orchard::WalletWitness> witness;
  std::shared_ptr<const consensus::VerifiedOrchardAuthorizations> origin;
  uint32_t action_index;
  orchard::WalletScope scope;
  uint32_t created_height;
  uint256 created_block;
};
// All callbacks use the SAME locked selected-chain view as the supplied
// checkpoint. Origin must establish selected-ancestor membership and exact
// transaction inclusion from authenticated block bytes. It must not trust a
// txindex entry alone or interpret a database read failure as absence.
struct OrchardWalletRestoreLookups {
  std::function<std::shared_ptr<const consensus::VerifiedOrchardAuthorizations>(
      uint32_t height, const uint256 &block, const orchard::Hash &txid)>
      origin;
  std::function<StatusOr<bool>(const uint256 &)> spent_nullifier;
};
// Immutable derived wallet view. Host supplies a fully validated selected block
// and holds its chain/wallet snapshot contract. The checks here bind body,
// authorization coverage, nullifiers and frontier; they do NOT validate PoW or
// replace block admission. Publish only after wallet persistence succeeds.
class OrchardWalletScanState {
public:
  static OrchardWalletScanState Begin(orchard::SigningDomain,
                                      const orchard::FullViewingKeyBytes &,
                                      uint32_t activation_height,
                                      const uint256 &authenticated_parent);
  [[nodiscard]] OrchardWalletScanState
  Advance(const consensus::OrchardBlockContext &, const OrchardBlockCandidate &,
          const consensus::PreparedOrchardState &,
          std::span<const consensus::VerifiedOrchardAuthorizations>) const;
  const storage::OrchardStoredState &Checkpoint() const noexcept;
  const std::vector<ScannedOrchardNote> &Notes() const noexcept;
  uint64_t BalanceUna() const noexcept;
  [[nodiscard]] orchard::WalletStateBytes Encode() const;
  // Consumes only authenticated/decrypted snapshot bytes. A different chain
  // checkpoint is an error requiring rollback/rescan, never spendable state.
  // Notes are decrypted again from authenticated origins; openings/keys are
  // not serialized. This scan state excludes issued-address counters and
  // pending-operation reservations, which must NOT rewind on a chain reorg.
  [[nodiscard]] static OrchardWalletScanState
  Restore(const orchard::WalletStateBytes &, orchard::SigningDomain,
          const orchard::FullViewingKeyBytes &, uint32_t activation_height,
          const storage::OrchardStoredState &selected_checkpoint,
          const OrchardWalletRestoreLookups &);

private:
  struct Data;
  explicit OrchardWalletScanState(std::shared_ptr<const Data> data)
      : data_(std::move(data)) {}
  std::shared_ptr<const Data> data_;
};
} // namespace dinero::wallet
