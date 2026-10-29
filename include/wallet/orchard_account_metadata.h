#pragma once
#include "wallet/orchard_operation_queue.h"
namespace dinero::wallet {
class OrchardAccountState;
// Chain-independent ownership fields of an account payload. The caller must
// first authenticate the envelope with WalletSnapshotStore under the real
// wallet/seed/SQLite owner. This decoder validates framing, domain, viewing-key
// identity and operation bodies; it does not authenticate the enclosing bytes,
// restore or certify the scan, prove selected-chain observations, certify a
// complete account catalog, release reservations or authorize spending.
class OrchardAccountMetadata {
public:
  struct DeliveryCheckpoint {
    uint64_t sequence = 0;
    uint256 digest;
    bool operator==(const DeliveryCheckpoint &) const = default;
  };
  struct ArchiveCheckpoint {
    uint64_t count = 0;
    orchard::Hash head{};
    bool operator==(const ArchiveCheckpoint &) const = default;
  };
  enum class OperationOutcome : uint8_t { Confirmed = 1, Conflicted = 2 };
  struct OperationObservation {
    OperationOutcome outcome;
    uint32_t height;
    uint256 block_hash;
    orchard::Hash transaction_id;
    bool operator==(const OperationObservation &) const = default;
  };
  [[nodiscard]] static OrchardAccountMetadata Read(
      const orchard::WalletStateBytes &, orchard::SigningDomain,
      const orchard::FullViewingKeyBytes &, uint32_t activation);
  const OrchardOperationQueue &Operations() const noexcept { return operations; }
  const std::map<orchard::Hash, OperationObservation> &Observations() const noexcept {
    return observations;
  }
  const ArchiveCheckpoint &Archive() const noexcept { return archive; }
  const DeliveryCheckpoint &Delivery() const noexcept { return delivery; }
  uint64_t ParentSnapshotRevision() const noexcept { return parent_snapshot_revision; }
protected:
  explicit OrchardAccountMetadata(orchard::SigningDomain d)
      : operations(OrchardOperationQueue::Empty(d)) {}
  OrchardOperationQueue operations;
  std::map<orchard::Hash, OperationObservation> observations;
  ArchiveCheckpoint archive;
  DeliveryCheckpoint delivery;
  uint64_t parent_snapshot_revision = 0;
  std::array<orchard::DiversifierIndex, 2> next{};
  std::array<bool, 2> exhausted{};
private:
  friend class OrchardAccountState;
  // The scan span borrows the supplied plaintext only during full restoration.
  // The public metadata value never stores a reference to decrypted bytes.
  static OrchardAccountMetadata Read(
      const orchard::WalletStateBytes &, orchard::SigningDomain,
      const orchard::FullViewingKeyBytes &, uint32_t,
      std::span<const uint8_t> &scan_bytes);
};
} // namespace dinero::wallet
