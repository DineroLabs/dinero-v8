#include "shielded_migration_metadata.h"
#include <sqlite3.h>
#include <algorithm>
#include <charconv>
#include <climits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>

namespace dinero::storage::detail {
namespace {
void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
std::string Uri(const std::filesystem::path& path) {
    const char hex[] = "0123456789ABCDEF"; std::string out = "file:";
    for (unsigned char c : path.string()) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out + "?mode=ro&immutable=1";
}
class Reader {
    uint64_t remaining_;
    const uint64_t field_limit_;
    Database db_{nullptr, &sqlite3_close};
public:
    Reader(const std::filesystem::path& path, const ShieldedCompanionLimits& limits)
        : remaining_(limits.max_sqlite_steps), field_limit_(limits.max_metadata_value_bytes) {
        Require(std::filesystem::is_regular_file(path), "required SQLite companion missing");
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(Uri(path).c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr);
        db_.reset(raw); Require(rc == SQLITE_OK && sqlite3_db_readonly(db_.get(), "main") == 1, "cannot open immutable SQLite companion");
        Require(sqlite3_db_config(db_.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) == SQLITE_OK &&
                sqlite3_db_config(db_.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK, "cannot restrict SQLite reader");
        sqlite3_limit(db_.get(), SQLITE_LIMIT_LENGTH, std::max<int>(65536, static_cast<int>(field_limit_)));
        sqlite3_limit(db_.get(), SQLITE_LIMIT_SQL_LENGTH, 65536);
        sqlite3_progress_handler(db_.get(), 1000, [](void* state) {
            auto& budget = static_cast<Reader*>(state)->remaining_;
            if (budget <= 1000) { budget = 0; return 1; }
            budget -= 1000; return 0;
        }, this);
        auto check = Prepare("PRAGMA quick_check(1)");
        Require(sqlite3_step(check.get()) == SQLITE_ROW && Text(check.get(), 0) == "ok" &&
                sqlite3_step(check.get()) == SQLITE_DONE, "SQLite integrity check failed or exhausted budget");
    }
    Statement Prepare(const std::string& sql) {
        sqlite3_stmt* raw = nullptr; const int rc = sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw, nullptr);
        Statement result(raw, &sqlite3_finalize); Require(rc == SQLITE_OK, "cannot read SQLite companion schema"); return result;
    }
    std::string Text(sqlite3_stmt* row, int column) const {
        Require(sqlite3_column_type(row, column) == SQLITE_TEXT, "metadata must be text");
        const int size = sqlite3_column_bytes(row, column);
        Require(size >= 0 && uint64_t(size) <= field_limit_, "metadata field budget exceeded");
        const auto* bytes = sqlite3_column_text(row, column);
        Require(bytes != nullptr, "cannot read metadata text");
        std::string value(reinterpret_cast<const char*>(bytes), size);
        Require(value.find('\0') == std::string::npos, "embedded NUL in metadata"); return value;
    }
    int64_t Version() {
        auto query = Prepare("PRAGMA user_version");
        Require(sqlite3_step(query.get()) == SQLITE_ROW && sqlite3_column_type(query.get(), 0) == SQLITE_INTEGER, "missing SQLite version");
        const auto value = sqlite3_column_int64(query.get(), 0);
        Require(sqlite3_step(query.get()) == SQLITE_DONE, "ambiguous SQLite version"); return value;
    }
    void Table(const char* name) {
        auto query = Prepare("SELECT type,sql FROM sqlite_schema WHERE name=?1");
        Require(sqlite3_bind_text(query.get(), 1, name, -1, SQLITE_STATIC) == SQLITE_OK, "cannot bind table name");
        Require(sqlite3_step(query.get()) == SQLITE_ROW && Text(query.get(), 0) == "table", "required ordinary SQLite table missing");
        auto declaration = Text(query.get(), 1);
        std::transform(declaration.begin(), declaration.end(), declaration.begin(), [](unsigned char c) {
            return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : char(c);
        });
        Require(declaration.find("VIRTUAL") == std::string::npos && sqlite3_step(query.get()) == SQLITE_DONE, "unsupported SQLite table");
    }
};
using Metadata = std::map<std::string, std::string>;
uint32_t Height(const std::string& value) {
    uint32_t height = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), height);
    Require(!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
            height <= INT32_MAX && value == std::to_string(height), "invalid metadata height"); return height;
}
bool Hex(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

ExternalMigrationState InspectMigrationMetadata(const std::filesystem::path& root, const ShieldedCompanionLimits& limits) {
    Require(limits.max_metadata_rows && limits.max_metadata_value_bytes && limits.max_metadata_value_bytes <= INT_MAX &&
            limits.max_sqlite_steps >= 1000 && limits.max_ancestry_headers, "explicit metadata budgets required");
    Reader reader(root / "blockchain/utxo", limits);
    Require(reader.Version() == 1, "unsupported UTXO SQLite schema version"); reader.Table("utxo_metadata");
    Metadata rows; auto query = reader.Prepare("SELECT key,value FROM utxo_metadata"); int rc;
    while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
        Require(rows.size() < limits.max_metadata_rows, "metadata row budget exceeded");
        auto key = reader.Text(query.get(), 0), value = reader.Text(query.get(), 1);
        Require(!key.empty() && rows.emplace(key, value).second, "duplicate or empty metadata key");
    }
    Require(rc == SQLITE_DONE, "SQLite metadata scan failed or exhausted budget");
    const std::set<std::string> known{
        "assumeutxo_active", "assumeutxo_base_block", "assumeutxo_base_height", "assumeutxo_lifecycle_state",
        "assumeutxo_fatal_reason", "assumeutxo_fully_validated", "assumeutxo_lc_base_block", "assumeutxo_lc_base_height",
        "assumeutxo_lc_progress_height", "assumeutxo_expected_commitment", "assumeutxo_expected_utreexo_root", "assumeutxo_expected_shielded_root"};
    for (const auto& [key, value] : rows)
        Require(key.rfind("assumeutxo_", 0) != 0 || known.count(key), "unknown AssumeUTXO metadata");
    auto get = [&](const char* key) -> std::optional<std::string> {
        const auto found = rows.find(key); if (found == rows.end()) return {}; return found->second;
    };
    auto nonempty = [&](const char* key) { const auto value = get(key); return value && !value->empty(); };
    Require(!nonempty("reorg_in_progress") && !nonempty("incomplete_reorg_recovery_tip") &&
            !nonempty("assumeutxo_fatal_reason"), "pending reorg/recovery or fatal state");
    const auto active = get("assumeutxo_active");
    Require(!active || *active == "false", "active or ambiguous AssumeUTXO import");
    const auto state = get("assumeutxo_lifecycle_state");
    Require(!state || *state == "disabled" || *state == "fully_validated", "incomplete or unknown AssumeUTXO lifecycle");
    ExternalMigrationState result{{}, {}, limits.max_ancestry_headers};
    if (state && *state == "fully_validated") {
        const auto flag = get("assumeutxo_fully_validated"), hash = get("assumeutxo_lc_base_block"), height = get("assumeutxo_lc_base_height");
        Require(flag && *flag == "true" && hash && Hex(*hash) && *hash != std::string(64, '0') && height,
                "incomplete fully-validated metadata");
        result.promoted_base = ProtectedMigrationBase{Height(*height), *hash};
        const auto progress = get("assumeutxo_lc_progress_height");
        Require(!progress || Height(*progress) >= result.promoted_base->height, "validation progress below base");
        const auto old_hash = get("assumeutxo_base_block"), old_height = get("assumeutxo_base_height");
        Require(bool(old_hash) == bool(old_height), "partial active base metadata");
        if (old_hash) Require(*old_hash == *hash && Height(*old_height) == result.promoted_base->height, "conflicting lifecycle bases");
        for (const char* key : {"assumeutxo_expected_commitment", "assumeutxo_expected_utreexo_root", "assumeutxo_expected_shielded_root"}) {
            const auto value = get(key); Require(!value || Hex(*value), "invalid persisted expected commitment");
        }
    } else {
        for (const auto& [key, value] : rows)
            if (known.count(key) && key != "assumeutxo_active" && key != "assumeutxo_lifecycle_state" && key != "assumeutxo_fatal_reason")
                throw std::runtime_error("orphaned AssumeUTXO metadata");
    }
    if (const auto height = get("wallet_snapshot_recovery_base_height")) result.wallet_base_height = Height(*height);
    const auto cache = root / "blockchain/shielded_nullifiers.db";
    if (std::filesystem::exists(cache)) {
        Reader nullifiers(cache, limits); const auto version = nullifiers.Version();
        Require(version == 0 || version == 1, "unsupported nullifier cache version"); nullifiers.Table("nullifiers");
        auto first = nullifiers.Prepare("SELECT nullifier,block_height FROM nullifiers LIMIT 1");
        const int step = sqlite3_step(first.get()); Require(step == SQLITE_ROW || step == SQLITE_DONE, "cannot inspect nullifier provenance");
        Require(version == 1 || step == SQLITE_DONE, "populated unstamped nullifier store requires separate reconciliation");
    }
    return result;
}
} // namespace dinero::storage::detail
