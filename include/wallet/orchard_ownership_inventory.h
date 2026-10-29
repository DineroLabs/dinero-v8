#pragma once
#include "wallet/orchard_account_catalog.h"
#include "wallet/orchard_archive_reader.h"
namespace dinero::wallet {
// One authenticated SQLite snapshot of the catalog and all present current /
// reached / retained owners. This is not a selected-chain scan certificate or
// authority to release archived inputs. Caller pins the wallet/seed and owns
// the transaction. No writes, enrollment, callbacks, commit or live publication.
class OrchardOwnershipInventory {
public:
  struct RetainedAccount {
    uint64_t revision;
    OrchardAccountMetadata metadata;
  };
  struct RetainedArchive {
    orchard::Hash id;
    OrchardArchiveReader::Record record;
  };
  struct Account {
    OrchardAccountCatalog::Entry entry;
    orchard::WalletStorageIdentity identity;
    OrchardArchiveReader::Captured current;
    std::vector<RetainedAccount> retained_accounts;
    std::vector<RetainedArchive> retained_archives;
  };
  struct CurrentInput {
    uint32_t account;
    orchard::Hash operation;
    orchard::ResolvedInput input;
  };
  struct Snapshot {
    orchard::Hash wallet_id;
    OrchardAccountCatalog::Snapshot catalog;
    std::vector<Account> accounts;
    // Current operations only. Archived inputs remain in their authenticated
    // records and require the existing selected-cause reconciliation owner.
    std::vector<CurrentInput> current_inputs;
    size_t current_rows=0,retained_rows=0;
  };
  static constexpr size_t kMaxRows=65536;
  [[nodiscard]] static Snapshot Read(sqlite3*,std::span<const uint8_t> seed);
};
} // namespace dinero::wallet
