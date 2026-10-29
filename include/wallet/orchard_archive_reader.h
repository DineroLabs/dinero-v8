#pragma once
#include "wallet/orchard_account_metadata.h"
namespace dinero::wallet {
// Read-only authenticated ownership records. Caller pins the real wallet/seed
// and owns the SQLite transaction. No scan initialization, selected-chain
// callback, writes, commit, reservation release or reactivation authority.
class OrchardArchiveReader {
public:
  struct Record {
    uint64_t revision;
    uint64_t sequence;
    orchard::Hash previous;
    OrchardOperationQueue operation;
    OrchardAccountMetadata::OperationObservation observation;
  };
  struct Archived {
    orchard::Hash id;
    orchard::WalletStorageIdentity identity;
    Record record;
  };
  struct Captured {
    uint64_t revision;
    OrchardAccountMetadata metadata;
    std::vector<Archived> archive; // Authenticated head-to-predecessor order.
  };
  static constexpr size_t kMaxCapturedRecords = 65536;
  OrchardArchiveReader(sqlite3*,orchard::WalletStorageIdentity,
                       orchard::SigningDomain,std::span<const uint8_t> seed);
  [[nodiscard]] bool Contains(const orchard::Hash&) const;
  [[nodiscard]] Record Read(const orchard::Hash&) const;
  // Same authenticated record format at a strictly earlier retained revision.
  // Requires the caller snapshot; never creates or promotes an archive owner.
  [[nodiscard]] Record ReadRetained(const orchard::Hash&,uint64_t revision) const;
  // Authenticate the current account and every reached archive record under
  // one caller snapshot. Missing/deleted links, wrong sequence or capacity
  // refuse; never return a prefix. Unreached SQL rows and complete catalog /
  // retained-history reconciliation are separate required inventory checks.
  [[nodiscard]] Captured CaptureCurrent(const orchard::FullViewingKeyBytes&,
                                        uint32_t activation) const;
protected:
  friend class OrchardAccountDelivery;
  orchard::WalletStorageIdentity RecordIdentity(const orchard::Hash&) const;
  sqlite3* db_;
  orchard::WalletStorageIdentity identity_;
  orchard::SigningDomain domain_;
  orchard::WalletStateBytes seed_;
};
} // namespace dinero::wallet
