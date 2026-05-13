#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <sqlite3.h>

namespace din {

/**
 * @brief Descriptor metadata for wallet persistence
 *
 * Immutable wallet policy records. Never mutate - only add, activate, deactivate, or deprecate.
 * Follows Bitcoin Core's safety model: old descriptors remain readable for incoming funds.
 */
struct DescriptorRecord {
    int64_t id = 0;
    std::string descriptor;         // Full descriptor with checksum
    std::string checksum;           // Descriptor checksum (for validation)
    std::string policy;             // BIP84, BIP86, etc.
    uint32_t account = 0;           // Account number
    bool is_change = false;         // Receive (false) or change (true)
    bool is_active = false;         // Currently active for address generation
    int64_t created_at = 0;         // Unix timestamp
    int64_t deprecated_at = 0;      // Unix timestamp (0 = not deprecated)
    std::string metadata;           // JSON metadata (optional)
    std::string label;              // Human-readable label (optional)
};

/**
 * @brief Descriptor persistence layer (storage + selection ONLY)
 *
 * CRITICAL: This class does NOT:
 * - Derive keys
 * - Sign transactions
 * - Generate addresses
 * - Validate PSBTs
 *
 * It ONLY:
 * - Stores descriptor records
 * - Activates/deactivates descriptors
 * - Queries descriptor state
 *
 * This separation ensures descriptor persistence cannot destabilize signing/derivation logic.
 */
class DescriptorStore {
public:
    explicit DescriptorStore(const std::string& db_path);
    ~DescriptorStore();

    // Database lifecycle
    bool initialize();
    void shutdown();

    // Descriptor persistence (immutable - add only, never mutate)
    bool addDescriptor(const DescriptorRecord& record);
    std::optional<DescriptorRecord> getDescriptor(int64_t id) const;
    std::vector<DescriptorRecord> listDescriptors(bool active_only = false) const;

    // Activation (for address generation selection)
    bool setActive(int64_t id, bool active);
    std::optional<DescriptorRecord> getActiveDescriptor(const std::string& policy, bool is_change) const;

    // Deprecation (for migration - never delete!)
    bool deprecateDescriptor(int64_t id);

    // Query helpers
    std::vector<DescriptorRecord> getDescriptorsByPolicy(const std::string& policy) const;
    std::optional<DescriptorRecord> findDescriptor(const std::string& descriptor) const;

    // Migration support (safe policy transitions)
    struct MigrationPlan {
        std::vector<int64_t> descriptors_to_deprecate;  // Old policy descriptors
        std::vector<DescriptorRecord> descriptors_to_add;  // New policy descriptors
        std::string from_policy;  // e.g., "BIP84"
        std::string to_policy;    // e.g., "BIP86"
    };
    bool executeMigration(const MigrationPlan& plan);

private:
    std::string db_path_;
    sqlite3* db_;

    // Schema management
    bool createTables();
    bool rejectRetiredLegacyCoinTypeDescriptors() const;

    // Prepared statements
    bool prepareStatements();
    void finalizeStatements();

    sqlite3_stmt* stmt_add_;
    sqlite3_stmt* stmt_get_;
    sqlite3_stmt* stmt_list_;
    sqlite3_stmt* stmt_set_active_;
    sqlite3_stmt* stmt_get_active_;
    sqlite3_stmt* stmt_deprecate_;
    sqlite3_stmt* stmt_find_;

    // Transaction helpers
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
};

} // namespace din
