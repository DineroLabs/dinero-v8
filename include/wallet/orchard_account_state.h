#pragma once
#include "wallet/orchard_operation_queue.h"
#include "wallet/orchard_scan_state.h"
namespace dinero { struct RuntimeOutboxEvent; }
namespace dinero::wallet {
// One encrypted account payload couples scan state, issued-address counters and
// pending spends. Reorg changes derived scan and chain observations, never
// address issuance or pending transaction identities. Caller persists before
// exposing results.
class OrchardAccountState {
public:
  struct DeliveryCheckpoint {
    uint64_t sequence = 0;
    uint256 digest;
    bool operator==(const DeliveryCheckpoint &) const = default;
  };
  const DeliveryCheckpoint &Delivery() const noexcept;
  // Authenticated storage locator, not a source cursor or a validity proof.
  // Zero means no retained parent binding (including legacy snapshots).
  uint64_t ParentSnapshotRevision() const noexcept;
  // This receipt covers this Orchard account only, not other consumers.
  // The event must come from ReadRuntimeOutboxUnderLock with the selected
  // profile. Its digest is a source receipt, not independently certified here.
  // Only these operations advance delivery; there is no cursor-only setter.
  // Persist the returned account with WalletSnapshotStore in the SAME wallet
  // transaction as any other wallet effects before publishing/acknowledging.
  [[nodiscard]] OrchardAccountState
  AdvanceDelivery(const RuntimeOutboxEvent &, const OrchardBlockCandidate &,
                  const consensus::PreparedOrchardState &,
                  std::span<const consensus::VerifiedOrchardAuthorizations>) const;
  [[nodiscard]] OrchardAccountState
  RewindDelivery(const RuntimeOutboxEvent &, const OrchardBlockCandidate &,
                 const OrchardAccountState &authenticated_parent) const;
  // Before activation the pool must be empty. Apply the actual historical
  // body: move its empty scan checkpoint, observe pending transparent-input
  // conflicts, or undo those observations. Never discards addresses/Ready data.
  [[nodiscard]] OrchardAccountState
  ApplyHistoricalDelivery(const RuntimeOutboxEvent &) const;
  struct ArchiveCheckpoint {
    uint64_t count = 0;
    orchard::Hash head{};
    bool operator==(const ArchiveCheckpoint &) const = default;
  };
  const ArchiveCheckpoint &Archive() const noexcept;
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
  // Untracked scan operations refuse an account with a delivery receipt.
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
  // Clears delivery acknowledgment: no cursor may skip the discarded scan.
  // A delivery owner must replay from the source origin or explicitly reconcile
  // the complete rescan before it can report synchronized readiness.
  [[nodiscard]] static OrchardAccountState
  RestoreForRescan(const orchard::WalletStateBytes &, orchard::SigningDomain,
                   const orchard::FullViewingKeyBytes &, uint32_t activation,
                   const uint256 &authenticated_activation_parent);

private:
  friend class OrchardOperationArchive;
  friend class OrchardAccountDelivery;
  static DeliveryCheckpoint ReadDeliveryMetadata(const orchard::WalletStateBytes&,
      orchard::SigningDomain,const orchard::FullViewingKeyBytes&,uint32_t,const uint256&);
  [[nodiscard]] OrchardAccountState WithParentSnapshotRevision(uint64_t) const;
  void CheckDelivery(const RuntimeOutboxEvent &, const OrchardBlockCandidate &,
                     bool connecting) const;
  [[nodiscard]] OrchardAccountState
  AdvanceScan(const consensus::OrchardBlockContext &, const OrchardBlockCandidate &,
              const consensus::PreparedOrchardState &,
              std::span<const consensus::VerifiedOrchardAuthorizations>) const;
  [[nodiscard]] OrchardAccountState
  RewindScan(const OrchardAccountState &) const;
  [[nodiscard]] OrchardAccountState WithArchive(ArchiveCheckpoint) const;
  [[nodiscard]] OrchardAccountState
  RemoveObservedOperation(const orchard::Hash &) const;
  [[nodiscard]] OrchardAccountState
  RestoreArchivedOperation(const orchard::Hash &,
                           const OrchardOperationQueue &) const;
  // Bound recovery only: replay actual selected branch bodies for a restored
  // archive reservation without changing scan, source cursor or parent links.
  [[nodiscard]] OrchardAccountState ObserveReactivatedOperation(
      const orchard::Hash&,uint32_t fork_height,const OrchardWalletRestoreLookups&,
      const std::function<StatusOr<uint256>(uint32_t)>& selected_hash) const;
  void VerifyOperationObservation(const orchard::Hash &,
                                  const OrchardWalletRestoreLookups &) const;
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
