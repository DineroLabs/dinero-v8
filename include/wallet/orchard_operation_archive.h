#pragma once
#include "wallet/orchard_account_state.h"
namespace dinero::wallet {
// Borrows the account's SQLite connection; caller holds wallet/selected-chain
// locks and owns the outer transaction. Returned state/revision are staged.
class OrchardOperationArchive {
public:
  class Cursor {
  public:
    uint64_t Remaining() const noexcept { return remaining_; }

  private:
    friend class OrchardOperationArchive;
    Cursor(OrchardAccountState::ArchiveCheckpoint root, uint64_t n,
           orchard::Hash id)
        : root_(root), remaining_(n), next_(id) {}
    OrchardAccountState::ArchiveCheckpoint root_;
    uint64_t remaining_;
    orchard::Hash next_;
  };
  class LocatedOperation {
  public:
    const orchard::Hash &Id() const noexcept { return id_; }
    uint64_t Sequence() const noexcept { return sequence_; }

  private:
    friend class OrchardOperationArchive;
    LocatedOperation(OrchardAccountState::ArchiveCheckpoint root, uint64_t n,
                     orchard::Hash id)
        : root_(root), sequence_(n), id_(id) {}
    OrchardAccountState::ArchiveCheckpoint root_;
    uint64_t sequence_;
    orchard::Hash id_;
  };
  struct Page {
    std::vector<LocatedOperation> entries;
    Cursor next;
  };
  struct Record {
    uint64_t revision;
    uint64_t sequence;
    orchard::Hash previous;
    OrchardOperationQueue operation; // Exactly one authenticated entry.
    OrchardAccountState::OperationObservation observation;
  };
  struct Staged {
    uint64_t revision;
    OrchardAccountState account;
  };
  static void InitializeSchemaUnderTransaction(sqlite3 *);
  OrchardOperationArchive(sqlite3 *, orchard::WalletStorageIdentity,
                          orchard::SigningDomain,
                          std::span<const uint8_t> seed);
  [[nodiscard]] bool Contains(const orchard::Hash &) const;
  [[nodiscard]] Cursor Begin(const OrchardAccountState &) const;
  // Follows authenticated predecessor links, at most 64 records per call.
  // Missing records fail, not end-of-list. Host retains the captured wallet
  // snapshot/chain checkpoint until enumeration and reconciliation finish.
  [[nodiscard]] Page List(Cursor, size_t limit = 32) const;
  [[nodiscard]] Record Read(const orchard::Hash &) const;
  [[nodiscard]] Staged StageCompleted(uint64_t expected_revision,
                                      const OrchardAccountState &,
                                      const orchard::Hash &,
                                      const OrchardWalletRestoreLookups &);
  // Reacquire pending reservations only for a disconnected recorded cause.
  // Errors/missing selected hashes are local failures, never proof of a reorg.
  // Capacity/conflicting reservations leave the encrypted archive untouched.
  // The host must finish reconciling archived records before new selection.
  [[nodiscard]] Staged StageReactivate(
      uint64_t expected_revision, const OrchardAccountState &,
      const LocatedOperation &,
      const std::function<StatusOr<uint256>(uint32_t)> &selected_hash);

private:
  friend class OrchardAccountDelivery;
  orchard::WalletStorageIdentity RecordIdentity(const orchard::Hash &) const;
  void CheckCurrent(uint64_t, const OrchardAccountState &) const;
  sqlite3 *db_;
  orchard::WalletStorageIdentity identity_;
  orchard::SigningDomain domain_;
  orchard::WalletStateBytes seed_;
};
} // namespace dinero::wallet
