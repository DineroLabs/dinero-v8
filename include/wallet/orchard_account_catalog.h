#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
struct sqlite3;
namespace dinero { class WalletManager; }
namespace dinero::wallet {
// Seed-authenticated account inventory. Missing legacy metadata and recovered
// identities are not certificates of an empty historical wallet. Authentication
// alone cannot detect rollback of a complete valid wallet backup.
class OrchardAccountCatalog {
public:
    struct Entry {
        uint32_t account;
        uint8_t network;
        std::array<uint8_t,32> genesis;
        uint32_t branch, activation;
        bool operator==(const Entry&) const = default;
    };
    struct Snapshot {
        bool generated;
        uint64_t revision;
        std::vector<Entry> accounts;
        bool operator==(const Snapshot&) const = default;
    };
    static constexpr size_t kMaxAccounts=1024;
    // Caller owns a transaction and the exact wallet/seed lifetime. No writes,
    // schema initialization or live publication. Missing is distinct from error.
    static std::optional<Snapshot> Read(sqlite3*,std::span<const uint8_t> seed);
private:
    friend class dinero::WalletManager;
    friend class OrchardAccountDelivery;
    // Caller owns the complete authenticated account inventory and transaction.
    // Compare the authenticated revision and add exactly one never-used number;
    // commit this with its first encrypted account snapshot, never separately.
    static uint64_t StageAppend(sqlite3*,std::span<const uint8_t>,uint64_t expected,const Entry&);

    // Only genuine first-seed creation may initialize this record. The caller
    // commits seed, sealed initial owner and catalog together with FULL durability.
    static void InitializeForInitialSeed(sqlite3*,std::span<const uint8_t>,bool generated);
};
} // namespace dinero::wallet
