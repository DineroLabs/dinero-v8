#include "wallet/descriptor_store.h"
#include "wallet/retired_coin_type_guard.h"
#include "sqlite_open.h"
#include <iostream>
#include <ctime>

namespace din {

DescriptorStore::DescriptorStore(const std::string& db_path)
    : db_path_(db_path), db_(nullptr),
      stmt_add_(nullptr), stmt_get_(nullptr), stmt_list_(nullptr),
      stmt_set_active_(nullptr), stmt_get_active_(nullptr),
      stmt_deprecate_(nullptr), stmt_find_(nullptr) {}

DescriptorStore::~DescriptorStore() {
    shutdown();
}

bool DescriptorStore::initialize() {
    auto result = open_sqlite(db_path_);
    if (result.rc != SQLITE_OK || !result.db) {
        std::cerr << "Failed to open descriptor store: " << result.errmsg << std::endl;
        return false;
    }

    db_ = result.db;

    if (!createTables()) {
        shutdown();
        return false;
    }

    if (!rejectRetiredLegacyCoinTypeDescriptors()) {
        shutdown();
        return false;
    }

    if (!prepareStatements()) {
        shutdown();
        return false;
    }

    return true;
}

void DescriptorStore::shutdown() {
    finalizeStatements();

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DescriptorStore::createTables() {
    // Exact schema from user specification
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS wallet_descriptors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            descriptor TEXT NOT NULL,
            checksum TEXT NOT NULL,
            policy TEXT NOT NULL,
            account INTEGER NOT NULL,
            is_change BOOLEAN NOT NULL,
            is_active BOOLEAN NOT NULL,
            created_at INTEGER NOT NULL,
            deprecated_at INTEGER,
            metadata TEXT,
            label TEXT
        );

        CREATE INDEX IF NOT EXISTS idx_descriptors_active ON wallet_descriptors(is_active);
        CREATE INDEX IF NOT EXISTS idx_descriptors_policy ON wallet_descriptors(policy);
        CREATE INDEX IF NOT EXISTS idx_descriptors_account ON wallet_descriptors(account);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_descriptors_unique ON wallet_descriptors(policy, account, is_change, descriptor);
    )";

    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create descriptor tables: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DescriptorStore::rejectRetiredLegacyCoinTypeDescriptors() const {
    if (!db_) return false;

    const std::string sql =
        "SELECT descriptor FROM wallet_descriptors WHERE descriptor LIKE '%" +
        std::to_string(dinero::wallet::RETIRED_LEGACY_COIN_TYPE) + "%' LIMIT 100";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to scan descriptor store for retired coin type: "
                  << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* descriptor_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string descriptor = descriptor_cstr ? descriptor_cstr : "";
        if (dinero::wallet::TextContainsRetiredLegacyCoinTypePathComponent(descriptor)) {
            sqlite3_finalize(stmt);
            std::cerr << dinero::wallet::RetiredLegacyCoinTypeError(
                             "Refusing to load descriptor store")
                      << "; restore/rederive descriptors with coin_type 1448."
                      << std::endl;
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DescriptorStore::prepareStatements() {
    // ADD descriptor
    const char* sql_add = R"(
        INSERT INTO wallet_descriptors
        (descriptor, checksum, policy, account, is_change, is_active, created_at, deprecated_at, metadata, label)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    if (sqlite3_prepare_v2(db_, sql_add, -1, &stmt_add_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare ADD statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // GET descriptor by ID
    const char* sql_get = "SELECT * FROM wallet_descriptors WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql_get, -1, &stmt_get_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare GET statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // LIST descriptors
    const char* sql_list = "SELECT * FROM wallet_descriptors ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(db_, sql_list, -1, &stmt_list_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare LIST statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // SET ACTIVE
    const char* sql_set_active = "UPDATE wallet_descriptors SET is_active = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql_set_active, -1, &stmt_set_active_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare SET ACTIVE statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // GET ACTIVE descriptor
    const char* sql_get_active = "SELECT * FROM wallet_descriptors WHERE policy = ? AND is_change = ? AND is_active = 1 LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql_get_active, -1, &stmt_get_active_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare GET ACTIVE statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // DEPRECATE descriptor
    const char* sql_deprecate = "UPDATE wallet_descriptors SET deprecated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql_deprecate, -1, &stmt_deprecate_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare DEPRECATE statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // FIND descriptor by string
    const char* sql_find = "SELECT * FROM wallet_descriptors WHERE descriptor = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql_find, -1, &stmt_find_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare FIND statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    return true;
}

void DescriptorStore::finalizeStatements() {
    if (stmt_add_) { sqlite3_finalize(stmt_add_); stmt_add_ = nullptr; }
    if (stmt_get_) { sqlite3_finalize(stmt_get_); stmt_get_ = nullptr; }
    if (stmt_list_) { sqlite3_finalize(stmt_list_); stmt_list_ = nullptr; }
    if (stmt_set_active_) { sqlite3_finalize(stmt_set_active_); stmt_set_active_ = nullptr; }
    if (stmt_get_active_) { sqlite3_finalize(stmt_get_active_); stmt_get_active_ = nullptr; }
    if (stmt_deprecate_) { sqlite3_finalize(stmt_deprecate_); stmt_deprecate_ = nullptr; }
    if (stmt_find_) { sqlite3_finalize(stmt_find_); stmt_find_ = nullptr; }
}

bool DescriptorStore::addDescriptor(const DescriptorRecord& record) {
    if (!db_ || !stmt_add_) return false;
    if (dinero::wallet::TextContainsRetiredLegacyCoinTypePathComponent(record.descriptor)) {
        std::cerr << dinero::wallet::RetiredLegacyCoinTypeError("Refusing to store descriptor")
                  << std::endl;
        return false;
    }

    sqlite3_reset(stmt_add_);

    // Bind parameters in order
    sqlite3_bind_text(stmt_add_, 1, record.descriptor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_, 2, record.checksum.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_, 3, record.policy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_add_, 4, record.account);
    sqlite3_bind_int(stmt_add_, 5, record.is_change ? 1 : 0);
    sqlite3_bind_int(stmt_add_, 6, record.is_active ? 1 : 0);
    sqlite3_bind_int64(stmt_add_, 7, record.created_at > 0 ? record.created_at : static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(stmt_add_, 8, record.deprecated_at);
    sqlite3_bind_text(stmt_add_, 9, record.metadata.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_, 10, record.label.c_str(), -1, SQLITE_TRANSIENT);

    return sqlite3_step(stmt_add_) == SQLITE_DONE;
}

std::optional<DescriptorRecord> DescriptorStore::getDescriptor(int64_t id) const {
    if (!db_ || !stmt_get_) return std::nullopt;

    sqlite3_reset(stmt_get_);
    sqlite3_bind_int64(stmt_get_, 1, id);

    if (sqlite3_step(stmt_get_) == SQLITE_ROW) {
        DescriptorRecord record;
        record.id = sqlite3_column_int64(stmt_get_, 0);
        record.descriptor = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 1));
        record.checksum = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 2));
        record.policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 3));
        record.account = sqlite3_column_int(stmt_get_, 4);
        record.is_change = sqlite3_column_int(stmt_get_, 5) != 0;
        record.is_active = sqlite3_column_int(stmt_get_, 6) != 0;
        record.created_at = sqlite3_column_int64(stmt_get_, 7);
        record.deprecated_at = sqlite3_column_int64(stmt_get_, 8);

        const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 9));
        record.metadata = metadata ? metadata : "";

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 10));
        record.label = label ? label : "";

        return record;
    }

    return std::nullopt;
}

std::vector<DescriptorRecord> DescriptorStore::listDescriptors(bool active_only) const {
    std::vector<DescriptorRecord> descriptors;
    if (!db_) return descriptors;

    // Use dynamic query for active_only filter
    const char* sql = active_only
        ? "SELECT * FROM wallet_descriptors WHERE is_active = 1 ORDER BY created_at DESC"
        : "SELECT * FROM wallet_descriptors ORDER BY created_at DESC";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return descriptors;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DescriptorRecord record;
        record.id = sqlite3_column_int64(stmt, 0);
        record.descriptor = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.checksum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.account = sqlite3_column_int(stmt, 4);
        record.is_change = sqlite3_column_int(stmt, 5) != 0;
        record.is_active = sqlite3_column_int(stmt, 6) != 0;
        record.created_at = sqlite3_column_int64(stmt, 7);
        record.deprecated_at = sqlite3_column_int64(stmt, 8);

        const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        record.metadata = metadata ? metadata : "";

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        record.label = label ? label : "";

        descriptors.push_back(record);
    }

    sqlite3_finalize(stmt);
    return descriptors;
}

bool DescriptorStore::setActive(int64_t id, bool active) {
    if (!db_ || !stmt_set_active_) return false;

    sqlite3_reset(stmt_set_active_);
    sqlite3_bind_int(stmt_set_active_, 1, active ? 1 : 0);
    sqlite3_bind_int64(stmt_set_active_, 2, id);

    return sqlite3_step(stmt_set_active_) == SQLITE_DONE;
}

std::optional<DescriptorRecord> DescriptorStore::getActiveDescriptor(const std::string& policy, bool is_change) const {
    if (!db_ || !stmt_get_active_) return std::nullopt;

    sqlite3_reset(stmt_get_active_);
    sqlite3_bind_text(stmt_get_active_, 1, policy.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_get_active_, 2, is_change ? 1 : 0);

    if (sqlite3_step(stmt_get_active_) == SQLITE_ROW) {
        DescriptorRecord record;
        record.id = sqlite3_column_int64(stmt_get_active_, 0);
        record.descriptor = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_active_, 1));
        record.checksum = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_active_, 2));
        record.policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_active_, 3));
        record.account = sqlite3_column_int(stmt_get_active_, 4);
        record.is_change = sqlite3_column_int(stmt_get_active_, 5) != 0;
        record.is_active = sqlite3_column_int(stmt_get_active_, 6) != 0;
        record.created_at = sqlite3_column_int64(stmt_get_active_, 7);
        record.deprecated_at = sqlite3_column_int64(stmt_get_active_, 8);

        const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_active_, 9));
        record.metadata = metadata ? metadata : "";

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_active_, 10));
        record.label = label ? label : "";

        return record;
    }

    return std::nullopt;
}

bool DescriptorStore::deprecateDescriptor(int64_t id) {
    if (!db_ || !stmt_deprecate_) return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));

    sqlite3_reset(stmt_deprecate_);
    sqlite3_bind_int64(stmt_deprecate_, 1, now);
    sqlite3_bind_int64(stmt_deprecate_, 2, id);

    return sqlite3_step(stmt_deprecate_) == SQLITE_DONE;
}

std::vector<DescriptorRecord> DescriptorStore::getDescriptorsByPolicy(const std::string& policy) const {
    std::vector<DescriptorRecord> descriptors;
    if (!db_) return descriptors;

    const char* sql = "SELECT * FROM wallet_descriptors WHERE policy = ? ORDER BY created_at DESC";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return descriptors;
    }

    sqlite3_bind_text(stmt, 1, policy.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DescriptorRecord record;
        record.id = sqlite3_column_int64(stmt, 0);
        record.descriptor = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.checksum = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.account = sqlite3_column_int(stmt, 4);
        record.is_change = sqlite3_column_int(stmt, 5) != 0;
        record.is_active = sqlite3_column_int(stmt, 6) != 0;
        record.created_at = sqlite3_column_int64(stmt, 7);
        record.deprecated_at = sqlite3_column_int64(stmt, 8);

        const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        record.metadata = metadata ? metadata : "";

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        record.label = label ? label : "";

        descriptors.push_back(record);
    }

    sqlite3_finalize(stmt);
    return descriptors;
}

std::optional<DescriptorRecord> DescriptorStore::findDescriptor(const std::string& descriptor) const {
    if (!db_ || !stmt_find_) return std::nullopt;

    sqlite3_reset(stmt_find_);
    sqlite3_bind_text(stmt_find_, 1, descriptor.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt_find_) == SQLITE_ROW) {
        DescriptorRecord record;
        record.id = sqlite3_column_int64(stmt_find_, 0);
        record.descriptor = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find_, 1));
        record.checksum = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find_, 2));
        record.policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find_, 3));
        record.account = sqlite3_column_int(stmt_find_, 4);
        record.is_change = sqlite3_column_int(stmt_find_, 5) != 0;
        record.is_active = sqlite3_column_int(stmt_find_, 6) != 0;
        record.created_at = sqlite3_column_int64(stmt_find_, 7);
        record.deprecated_at = sqlite3_column_int64(stmt_find_, 8);

        const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find_, 9));
        record.metadata = metadata ? metadata : "";

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt_find_, 10));
        record.label = label ? label : "";

        return record;
    }

    return std::nullopt;
}

bool DescriptorStore::beginTransaction() {
    if (!db_) return false;
    return sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool DescriptorStore::commitTransaction() {
    if (!db_) return false;
    return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool DescriptorStore::rollbackTransaction() {
    if (!db_) return false;
    return sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool DescriptorStore::executeMigration(const MigrationPlan& plan) {
    if (!db_) return false;

    // Atomic migration: all or nothing
    if (!beginTransaction()) {
        return false;
    }

    // Step 1: Deprecate old descriptors (never delete!)
    for (int64_t id : plan.descriptors_to_deprecate) {
        if (!deprecateDescriptor(id)) {
            rollbackTransaction();
            return false;
        }
        // Also deactivate
        if (!setActive(id, false)) {
            rollbackTransaction();
            return false;
        }
    }

    // Step 2: Add new descriptors
    for (const auto& record : plan.descriptors_to_add) {
        if (!addDescriptor(record)) {
            rollbackTransaction();
            return false;
        }
    }

    // Commit migration atomically
    return commitTransaction();
}

} // namespace din
