#pragma once
#include "orchard_wallet.h"
#include <optional>
struct sqlite3;
namespace dinero::orchard {
struct WalletStorageIdentity {
    WalletNetwork network;
    Hash genesis;
    Hash wallet_id; // Stable opaque wallet identifier, not a note identifier.
    uint32_t account;
};
struct LoadedWalletState { uint64_t revision; WalletStateBytes state; };
// Borrows the existing wallet SQLite connection. Caller holds the wallet lock
// and owns its lifetime. Never opens a datadir, commits, rolls back the outer
// transaction or publishes memory. Require durable SQLite mode and an outer
// transaction for schema/write calls. Seed is from the unlocked wallet.
// Existing encrypted snapshots may be authenticated on a read-only main
// connection; every schema/write call still requires a writable connection.
class WalletSnapshotStore {
public:
    static constexpr size_t kMaxStateBytes=WalletStateBytes::kMaxBytes;
    static void InitializeSchemaUnderTransaction(sqlite3*);
    WalletSnapshotStore(sqlite3*,WalletStorageIdentity,std::span<const uint8_t> seed);
    ~WalletSnapshotStore();
    WalletSnapshotStore(const WalletSnapshotStore&)=delete;
    WalletSnapshotStore& operator=(const WalletSnapshotStore&)=delete;
    [[nodiscard]] std::optional<LoadedWalletState> Read()const;
    // Replace the ENTIRE shielded wallet snapshot in the caller's transaction
    // alongside its ordinary wallet records. Revision0 means absent. Return is
    // a staged revision, not durable success. Publish only after outer commit.
    [[nodiscard]] uint64_t StageReplace(uint64_t expected_revision,const WalletStateBytes&);
    // Retain the authenticated previous revision and replace the current state
    // in the same caller-owned transaction. Retained revisions are immutable;
    // missing history is an error, never permission to invent a parent scan.
    // Existing non-retaining writers remain readable but may leave gaps.
    [[nodiscard]] uint64_t StageReplaceRetaining(uint64_t expected_revision,const WalletStateBytes&);
    [[nodiscard]] LoadedWalletState ReadRetained(uint64_t revision) const;
private:
    std::vector<uint8_t> AssociatedData(uint64_t revision)const;
    std::vector<uint8_t> Seal(uint64_t revision,std::span<const uint8_t>)const;
    WalletStateBytes Open(uint64_t revision,std::span<const uint8_t>)const;
    sqlite3* const db_;
    const WalletStorageIdentity identity_;
    std::array<uint8_t,32> key_{};
};
} // namespace dinero::orchard
