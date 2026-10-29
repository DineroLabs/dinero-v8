#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
struct sqlite3;
namespace dinero { class WalletManager; class ChainstateService; }
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
        // A distinct, profile-scoped origin, never a claim of Core generation
        // or current selected-chain scan completeness.
        struct Preactivation {
            uint8_t network;
            std::array<uint8_t,32> genesis;
            uint32_t branch, activation, height;
            std::array<uint8_t,32> block;
            bool operator==(const Preactivation&) const = default;
        };
        std::optional<Preactivation> preactivation{};
        struct RecoveredAccount0 {
            Preactivation origin; // Actual activation parent, not a caller birthday.
            uint32_t height;
            std::array<uint8_t,32> block;
            uint64_t sequence;
            std::array<uint8_t,32> digest;
            bool operator==(const RecoveredAccount0&) const = default;
        };
        std::optional<RecoveredAccount0> recovered{};
        // Complete for the declared scope only. Recovery certifies account0,
        // never arbitrary account discovery or a new seed's provenance.
        bool KnownInventory() const { return generated || preactivation.has_value() || recovered.has_value(); }
        bool Covers(const Entry& e) const {
            const auto* p=preactivation?&*preactivation:recovered&&e.account==0?&recovered->origin:nullptr;
            return generated || (p && p->network==e.network && p->genesis==e.genesis &&
                p->branch==e.branch && p->activation==e.activation);
        }
        bool operator==(const Snapshot&) const = default;
    };
    static constexpr size_t kMaxAccounts=1024;
    // Caller owns a transaction and the exact wallet/seed lifetime. No writes,
    // schema initialization or live publication. Missing is distinct from error.
    static std::optional<Snapshot> Read(sqlite3*,std::span<const uint8_t> seed);
private:
    friend class dinero::WalletManager;
    friend class OrchardAccountDelivery;
    friend class dinero::ChainstateService;
    // Only the selected service may supply this checkpoint. Existing unknown
    // initial catalog and every empty storage row are checked; no repair/DDL.
    static Snapshot StagePreactivation(sqlite3*,std::span<const uint8_t>,
                                      const Snapshot::Preactivation&);
    static Snapshot RequireUnknownEmpty(sqlite3*,std::span<const uint8_t>);
    static Snapshot StageRecoveredAccount0(sqlite3*,std::span<const uint8_t>,const Snapshot::RecoveredAccount0&);
    // Caller owns the complete authenticated account inventory and transaction.
    // Compare the authenticated revision and add exactly one never-used number;
    // commit this with its first encrypted account snapshot, never separately.
    static uint64_t StageAppend(sqlite3*,std::span<const uint8_t>,uint64_t expected,const Entry&);

    // Only genuine first-seed creation may initialize this record. The caller
    // commits seed, sealed initial owner and catalog together with FULL durability.
    static void InitializeForInitialSeed(sqlite3*,std::span<const uint8_t>,bool generated);
};
} // namespace dinero::wallet
