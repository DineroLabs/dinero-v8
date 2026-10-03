#pragma once
#include "orchard_wallet.h"
#include <optional>
struct sqlite3;
namespace dinero::orchard {
// Wallet-private serialized state. Owned bytes are wiped on release/move
// assignment; this does not erase caller copies, swap or compiler temporaries.
class WalletStateBytes {
public:
    explicit WalletStateBytes(std::span<const uint8_t> bytes);
    ~WalletStateBytes();
    WalletStateBytes(WalletStateBytes&&) noexcept;
    WalletStateBytes& operator=(WalletStateBytes&&) noexcept;
    WalletStateBytes(const WalletStateBytes&)=delete;
    WalletStateBytes& operator=(const WalletStateBytes&)=delete;
    std::span<const uint8_t> Bytes()const noexcept{return bytes_;}
private:
    friend class WalletSnapshotStore;
    explicit WalletStateBytes(size_t size);
    void Wipe()noexcept;
    std::vector<uint8_t> bytes_;
};
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
class WalletSnapshotStore {
public:
    static constexpr size_t kMaxStateBytes=16*1024*1024;
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
