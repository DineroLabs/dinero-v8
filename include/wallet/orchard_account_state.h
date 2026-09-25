#pragma once
#include "wallet/orchard_operation_queue.h"
#include "wallet/orchard_scan_state.h"
namespace dinero::wallet {
// One encrypted account payload couples scan state, issued-address counters and
// pending spends. Reorg changes derived scan and chain observations, never
// address issuance or pending transaction identities. Caller persists before
// exposing results.
class OrchardAccountState {
public:
  enum class OperationOutcome : uint8_t { Confirmed = 1, Conflicted = 2 };
  struct OperationObservation {
    OperationOutcome outcome;
    uint32_t height;
    uint256 block_hash;
    orchard::Hash transaction_id; // Included tx, or the first conflicting tx.
    bool operator==(const OperationObservation &) const = default;
  };
  [[nodiscard]] static OrchardAccountState
  Begin(orchard::SigningDomain, const orchard::FullViewingKeyBytes &,
        uint32_t activation, const uint256 &parent);
  [[nodiscard]] std::pair<OrchardAccountState, orchard::WalletReceiver>
      IssueReceiver(orchard::WalletScope) const;
  [[nodiscard]] OrchardAccountState
  Advance(const consensus::OrchardBlockContext &, const OrchardBlockCandidate &,
          const consensus::PreparedOrchardState &,
          std::span<const consensus::VerifiedOrchardAuthorizations>) const;
  // Parent must be the authenticated retained common ancestor. Same-height
  // replacement requires rewinding first; an identical checkpoint is a no-op.
  [[nodiscard]] OrchardAccountState
  RewindScanFrom(const OrchardAccountState &retained_parent) const;
  [[nodiscard]] OrchardAccountState
  Reserve(const orchard::Hash &, const orchard::WalletProvingIntent &) const;
  [[nodiscard]] OrchardAccountState
  SetReady(const orchard::Hash &,
           const consensus::VerifiedOrchardAuthorizations &) const;
  [[nodiscard]] OrchardAccountState CancelReserved(const orchard::Hash &) const;
  const OrchardWalletScanState &Scan() const noexcept;
  const OrchardOperationQueue &Operations() const noexcept;
  // Derived selected-chain observations, not permission to delete signed
  // bytes, release reservations, or relay. Rewind/rescan reverses these only.
  const std::map<orchard::Hash, OperationObservation> &
  Observations() const noexcept;
  [[nodiscard]] orchard::WalletStateBytes Encode() const;
  [[nodiscard]] static OrchardAccountState
  Restore(const orchard::WalletStateBytes &, orchard::SigningDomain,
          const orchard::FullViewingKeyBytes &, uint32_t activation,
          const storage::OrchardStoredState &,
          const OrchardWalletRestoreLookups &);
  // For divergent/stale derived cache: keep authenticated issuance and pending
  // operations, reset only the scanner to the known activation parent. A full
  // rescan and fresh node admission are required before spending/broadcast.
  [[nodiscard]] static OrchardAccountState
  RestoreForRescan(const orchard::WalletStateBytes &, orchard::SigningDomain,
                   const orchard::FullViewingKeyBytes &, uint32_t activation,
                   const uint256 &authenticated_activation_parent);

private:
  struct Data;
  explicit OrchardAccountState(std::shared_ptr<const Data> data)
      : data_(std::move(data)) {}
  static std::shared_ptr<Data>
  ReadMetadata(const orchard::WalletStateBytes &, orchard::SigningDomain,
               const orchard::FullViewingKeyBytes &, uint32_t, const uint256 &,
               std::span<const uint8_t> &scan_bytes);
  std::shared_ptr<const Data> data_;
};
} // namespace dinero::wallet
