/**
 * Shielded note store — SQLite-backed, wallet-layer.
 * See include/wallet/shielded_note_store.h.
 */

#include "wallet/shielded_note_store.h"

#include <sqlite3.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dinero::wallet {

namespace sh = consensus::shielded;

namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS shielded_notes ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  value_una       INTEGER NOT NULL,"
    "  secret_key      BLOB NOT NULL,"
    "  public_key      BLOB NOT NULL,"
    "  randomness      BLOB NOT NULL,"
    "  commitment      BLOB NOT NULL UNIQUE,"
    "  leaf_index      INTEGER,"
    "  nullifier       BLOB,"
    "  confirmed       INTEGER NOT NULL DEFAULT 0,"
    "  spent           INTEGER NOT NULL DEFAULT 0,"
    "  created_height  INTEGER NOT NULL DEFAULT 0,"
    "  confirmed_height INTEGER NOT NULL DEFAULT 0,"
    "  spent_height    INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_shielded_notes_unspent "
    "  ON shielded_notes(spent) WHERE spent = 0;"
    "CREATE INDEX IF NOT EXISTS idx_shielded_notes_leaf_index "
    "  ON shielded_notes(leaf_index) WHERE leaf_index IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_shielded_notes_nullifier "
    "  ON shielded_notes(nullifier) WHERE nullifier IS NOT NULL;";

constexpr const char* kCreateLeavesSql =
    "CREATE TABLE IF NOT EXISTS shielded_tree_leaves ("
    "  leaf_index      INTEGER PRIMARY KEY NOT NULL,"
    "  commitment      BLOB NOT NULL,"
    "  created_height  INTEGER NOT NULL"
    ");";

// Defined later in this translation unit; forward-declared so the #324 heal
// below can bind/read hashes and reuse them.
void BindHash(sqlite3_stmt* s, int idx, const sh::Hash& h);
bool ReadHash(sqlite3_stmt* s, int col, sh::Hash& out);

// #324 one-time heal: older daemons re-appended every chain leaf on each
// rescan/restart (AppendChainLeaf deduped on leaf_index, which grew every
// pass), so shielded_tree_leaves accumulated exact duplicates appended AFTER
// the originals — doubling the tree and drifting note leaf positions, which
// corrupts spend Merkle paths. The distinct originals therefore occupy
// leaf_index 0..N-1 (N = distinct commitments); everything at leaf_index >= N
// is a duplicate. Drop the duplicates and re-point notes at the canonical
// (first-occurrence) leaf. Idempotent and a no-op once clean.
void DedupChainLeaves(sqlite3* db) {
    if (!db) return;
    // Cheap guard: only act when duplicates are present.
    bool has_dups = false;
    sqlite3_stmt* g = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT (SELECT COUNT(*) FROM shielded_tree_leaves) > "
            "(SELECT COUNT(DISTINCT commitment) FROM shielded_tree_leaves)",
            -1, &g, nullptr) == SQLITE_OK) {
        if (sqlite3_step(g) == SQLITE_ROW) has_dups = (sqlite3_column_int(g, 0) != 0);
        sqlite3_finalize(g);
    }
    if (!has_dups) return;
    // Duplication compounds across restarts (the tree grows between re-appends),
    // so duplicates are NOT a clean block after the originals. Rebuild the tree
    // canonically: the distinct commitments in first-occurrence order
    // (MIN(leaf_index) = the order they were first scanned = consensus order),
    // renumbered 0..N-1. A TEMP table with an INTEGER PRIMARY KEY gets
    // sequential ids in the inserted (ORDER BY) order without window functions.
    const char* sql =
        "BEGIN;"
        "DROP TABLE IF EXISTS _shielded_canon;"
        "CREATE TEMP TABLE _shielded_canon ("
        "  new_li INTEGER PRIMARY KEY, commitment BLOB, h INTEGER);"
        "INSERT INTO _shielded_canon (commitment, h) "
        "  SELECT commitment, MIN(created_height) FROM shielded_tree_leaves "
        "  GROUP BY commitment ORDER BY MIN(leaf_index);"
        // Re-point notes at the canonical 0-based leaf (rowid is 1-based).
        "UPDATE shielded_notes SET leaf_index = ("
        "  SELECT c.new_li - 1 FROM _shielded_canon c "
        "  WHERE c.commitment = shielded_notes.commitment) "
        "WHERE leaf_index IS NOT NULL AND EXISTS ("
        "  SELECT 1 FROM _shielded_canon c "
        "  WHERE c.commitment = shielded_notes.commitment);"
        // Replace the leaf table with the canonical 0..N-1 set.
        "DELETE FROM shielded_tree_leaves;"
        "INSERT INTO shielded_tree_leaves (leaf_index, commitment, created_height) "
        "  SELECT new_li - 1, commitment, h FROM _shielded_canon;"
        "DROP TABLE _shielded_canon;"
        "COMMIT;";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }
    // The nullifier is position-dependent: nullifier = ComputeNullifier(sk,
    // leaf_index). The corrupted wallet stored each note's nullifier for its
    // pre-dedup (drifted) leaf, so after renumbering leaf_index above it no
    // longer matches what the spend builder computes (MarkSpentByNullifier
    // would miss the row). Recompute every confirmed note's nullifier for its
    // now-canonical leaf — exactly what ConfirmNote does.
    struct NoteFix { sh::Hash sk; uint64_t li; sh::Hash commitment; };
    std::vector<NoteFix> fixes;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT secret_key, leaf_index, commitment FROM shielded_notes "
            "WHERE confirmed = 1 AND leaf_index IS NOT NULL",
            -1, &sel, nullptr) == SQLITE_OK) {
        while (sqlite3_step(sel) == SQLITE_ROW) {
            NoteFix f{};
            if (ReadHash(sel, 0, f.sk) &&
                ReadHash(sel, 2, f.commitment)) {
                f.li = static_cast<uint64_t>(sqlite3_column_int64(sel, 1));
                fixes.push_back(f);
            }
        }
        sqlite3_finalize(sel);
    }
    for (auto& f : fixes) {
        const sh::Hash nf = sh::ComputeNullifier(f.sk, f.li);
        sqlite3_stmt* up = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE shielded_notes SET nullifier = ? WHERE commitment = ?",
                -1, &up, nullptr) == SQLITE_OK) {
            BindHash(up, 1, nf);
            BindHash(up, 2, f.commitment);
            sqlite3_step(up);
            sqlite3_finalize(up);
        }
    }
}

bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::string(name) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

void BindHash(sqlite3_stmt* s, int idx, const sh::Hash& h) {
    sqlite3_bind_blob(s, idx, h.data(), sh::HASH_BYTES, SQLITE_TRANSIENT);
}

bool ReadHash(sqlite3_stmt* s, int col, sh::Hash& out) {
    const void* p = sqlite3_column_blob(s, col);
    const int len = sqlite3_column_bytes(s, col);
    if (!p || len != static_cast<int>(sh::HASH_BYTES)) return false;
    std::memcpy(out.data(), p, sh::HASH_BYTES);
    return true;
}

bool MigrateLegacySchema(sqlite3* db) {
    char* err = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }

    const char* rename_sql =
        "ALTER TABLE shielded_notes RENAME TO shielded_notes_legacy;";
    if (sqlite3_exec(db, rename_sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db, kCreateSql, nullptr, nullptr, &err) != SQLITE_OK ||
        sqlite3_exec(db, kCreateLeavesSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    const char* copy_notes_sql =
        "INSERT INTO shielded_notes ("
        "  id, value_una, secret_key, public_key, randomness, commitment,"
        "  leaf_index, nullifier, confirmed, spent, created_height,"
        "  confirmed_height, spent_height"
        ") "
        "SELECT id, value_una, secret_key, public_key, randomness, commitment,"
        "       leaf_index, NULL, 1, spent, created_height, created_height,"
        "       CASE WHEN spent <> 0 THEN created_height ELSE 0 END "
        "FROM shielded_notes_legacy;";
    if (sqlite3_exec(db, copy_notes_sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    const char* copy_leaves_sql =
        "INSERT OR IGNORE INTO shielded_tree_leaves (leaf_index, commitment, created_height) "
        "SELECT leaf_index, commitment, created_height "
        "FROM shielded_notes_legacy;";
    if (sqlite3_exec(db, copy_leaves_sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db, "DROP TABLE shielded_notes_legacy", nullptr, nullptr, &err) != SQLITE_OK ||
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

} // namespace

ShieldedNoteStore::~ShieldedNoteStore() { Close(); }

ShieldedNoteStore::OpenResult ShieldedNoteStore::Open(const std::string& path) {
    Close();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        Close();
        return OpenResult::IoError;
    }
    char* err = nullptr;
    if (sqlite3_exec(db_, kCreateSql, nullptr, nullptr, &err) != SQLITE_OK ||
        sqlite3_exec(db_, kCreateLeavesSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        Close();
        return OpenResult::SchemaError;
    }
    if (!ColumnExists(db_, "shielded_notes", "confirmed") ||
        !ColumnExists(db_, "shielded_notes", "nullifier") ||
        !ColumnExists(db_, "shielded_notes", "confirmed_height") ||
        !ColumnExists(db_, "shielded_notes", "spent_height")) {
        if (!MigrateLegacySchema(db_)) {
            Close();
            return OpenResult::SchemaError;
        }
    }
    // #324: heal wallets corrupted by the pre-fix re-append-on-rescan bug.
    DedupChainLeaves(db_);
    return OpenResult::Ok;
}

void ShieldedNoteStore::Close() noexcept {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool ShieldedNoteStore::AddNote(uint64_t value_una,
                                const sh::Hash& secret_key,
                                const sh::Hash& public_key,
                                const sh::Hash& randomness,
                                const sh::Hash& commitment,
                                uint64_t leaf_index,
                                uint32_t created_height) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO shielded_notes "
        "(value_una, secret_key, public_key, randomness, commitment, "
        " leaf_index, nullifier, confirmed, spent, created_height, confirmed_height, spent_height) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 1, 0, ?, ?, 0)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(value_una));
    BindHash(stmt, 2, secret_key);
    BindHash(stmt, 3, public_key);
    BindHash(stmt, 4, randomness);
    BindHash(stmt, 5, commitment);
    sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(leaf_index));
    const auto nullifier = sh::ComputeNullifier(secret_key, leaf_index);
    BindHash(stmt, 7, nullifier);
    sqlite3_bind_int(stmt, 8, static_cast<int>(created_height));
    sqlite3_bind_int(stmt, 9, static_cast<int>(created_height));

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool ShieldedNoteStore::AddPendingNote(uint64_t value_una,
                                       const sh::Hash& secret_key,
                                       const sh::Hash& public_key,
                                       const sh::Hash& randomness,
                                       const sh::Hash& commitment,
                                       uint32_t created_height) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO shielded_notes "
        "(value_una, secret_key, public_key, randomness, commitment, "
        " confirmed, spent, created_height, confirmed_height, spent_height) "
        "VALUES (?, ?, ?, ?, ?, 0, 0, ?, 0, 0)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(value_una));
    BindHash(stmt, 2, secret_key);
    BindHash(stmt, 3, public_key);
    BindHash(stmt, 4, randomness);
    BindHash(stmt, 5, commitment);
    sqlite3_bind_int(stmt, 6, static_cast<int>(created_height));

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool ShieldedNoteStore::MarkSpent(uint64_t leaf_index) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE shielded_notes SET spent = 1 WHERE leaf_index = ? AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(leaf_index));
    sqlite3_step(stmt);
    const bool changed = (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return changed;
}

bool ShieldedNoteStore::MarkSpentByNullifier(const sh::Hash& nullifier,
                                             uint32_t spent_height) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE shielded_notes SET spent = 1, spent_height = ? "
        "WHERE nullifier = ? AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, static_cast<int>(spent_height));
    BindHash(stmt, 2, nullifier);
    sqlite3_step(stmt);
    const bool changed = (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return changed;
}

int ShieldedNoteStore::UnmarkAllPendingSpent() {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE shielded_notes SET spent = 0 "
        "WHERE spent = 1 AND spent_height = 0 AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changed;
}

bool ShieldedNoteStore::UnmarkSpentByNullifier(const sh::Hash& nullifier) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE shielded_notes SET spent = 0, spent_height = 0 "
        "WHERE nullifier = ? AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindHash(stmt, 1, nullifier);
    sqlite3_step(stmt);
    const bool changed = (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return changed;
}

bool ShieldedNoteStore::ConfirmNote(const sh::Hash& commitment,
                                    uint64_t leaf_index,
                                    uint32_t confirmed_height) {
    if (!db_) return false;

    sqlite3_stmt* query = nullptr;
    const char* query_sql =
        "SELECT secret_key FROM shielded_notes WHERE commitment = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, query_sql, -1, &query, nullptr) != SQLITE_OK) return false;
    BindHash(query, 1, commitment);
    if (sqlite3_step(query) != SQLITE_ROW) {
        sqlite3_finalize(query);
        return false;
    }

    sh::Hash secret_key{};
    const bool have_secret = ReadHash(query, 0, secret_key);
    sqlite3_finalize(query);
    if (!have_secret) return false;

    const auto nullifier = sh::ComputeNullifier(secret_key, leaf_index);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE shielded_notes "
        "SET leaf_index = ?, nullifier = ?, confirmed = 1, spent = 0, "
        "    confirmed_height = ?, spent_height = 0 "
        "WHERE commitment = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(leaf_index));
    BindHash(stmt, 2, nullifier);
    sqlite3_bind_int(stmt, 3, static_cast<int>(confirmed_height));
    BindHash(stmt, 4, commitment);
    sqlite3_step(stmt);
    const bool changed = (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return changed;
}

bool ShieldedNoteStore::UnconfirmNote(const sh::Hash& commitment) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE shielded_notes "
        "SET leaf_index = NULL, nullifier = NULL, confirmed = 0, "
        "    spent = 0, confirmed_height = 0, spent_height = 0 "
        "WHERE commitment = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindHash(stmt, 1, commitment);
    sqlite3_step(stmt);
    const bool changed = (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return changed;
}

std::vector<ShieldedNote> ShieldedNoteStore::ListUnspent() const {
    std::vector<ShieldedNote> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, value_una, secret_key, public_key, randomness, "
        "       commitment, leaf_index, nullifier, confirmed, spent, "
        "       created_height, confirmed_height, spent_height "
        "FROM shielded_notes WHERE spent = 0 AND confirmed = 1 ORDER BY value_una DESC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ShieldedNote n{};
        n.id             = sqlite3_column_int64(stmt, 0);
        n.value_una      = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        ReadHash(stmt, 2, n.secret_key);
        ReadHash(stmt, 3, n.public_key);
        ReadHash(stmt, 4, n.randomness);
        ReadHash(stmt, 5, n.commitment);
        n.leaf_index     = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        ReadHash(stmt, 7, n.nullifier);
        n.confirmed      = (sqlite3_column_int(stmt, 8) != 0);
        n.spent          = (sqlite3_column_int(stmt, 9) != 0);
        n.created_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
        n.confirmed_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
        n.spent_height   = static_cast<uint32_t>(sqlite3_column_int(stmt, 12));
        out.push_back(n);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ShieldedNote> ShieldedNoteStore::ListAll() const {
    std::vector<ShieldedNote> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, value_una, secret_key, public_key, randomness, "
        "       commitment, leaf_index, nullifier, confirmed, spent, "
        "       created_height, confirmed_height, spent_height "
        "FROM shielded_notes ORDER BY id ASC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ShieldedNote n{};
        n.id             = sqlite3_column_int64(stmt, 0);
        n.value_una      = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        ReadHash(stmt, 2, n.secret_key);
        ReadHash(stmt, 3, n.public_key);
        ReadHash(stmt, 4, n.randomness);
        ReadHash(stmt, 5, n.commitment);
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            n.leaf_index = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        }
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            ReadHash(stmt, 7, n.nullifier);
        }
        n.confirmed      = (sqlite3_column_int(stmt, 8) != 0);
        n.spent          = (sqlite3_column_int(stmt, 9) != 0);
        n.created_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
        n.confirmed_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
        n.spent_height   = static_cast<uint32_t>(sqlite3_column_int(stmt, 12));
        out.push_back(n);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<ShieldedNote> ShieldedNoteStore::GetByLeafIndex(uint64_t leaf_index) const {
    if (!db_) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, value_una, secret_key, public_key, randomness, "
        "       commitment, leaf_index, nullifier, confirmed, spent, "
        "       created_height, confirmed_height, spent_height "
        "FROM shielded_notes WHERE leaf_index = ? AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(leaf_index));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    ShieldedNote n{};
    n.id             = sqlite3_column_int64(stmt, 0);
    n.value_una      = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    ReadHash(stmt, 2, n.secret_key);
    ReadHash(stmt, 3, n.public_key);
    ReadHash(stmt, 4, n.randomness);
    ReadHash(stmt, 5, n.commitment);
    n.leaf_index     = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
    ReadHash(stmt, 7, n.nullifier);
    n.confirmed      = (sqlite3_column_int(stmt, 8) != 0);
    n.spent          = (sqlite3_column_int(stmt, 9) != 0);
    n.created_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
    n.confirmed_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
    n.spent_height   = static_cast<uint32_t>(sqlite3_column_int(stmt, 12));
    sqlite3_finalize(stmt);
    return n;
}

uint64_t ShieldedNoteStore::GetBalance() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT COALESCE(SUM(value_una), 0) FROM shielded_notes WHERE spent = 0 AND confirmed = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    uint64_t bal = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        bal = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return bal;
}

bool ShieldedNoteStore::AppendChainLeaf(const sh::Hash& commitment,
                                        uint64_t leaf_index,
                                        uint32_t created_height) {
    if (!db_) return false;
    // #324: a commitment already present in the tree must NEVER be re-appended
    // at a new leaf_index on rescan/restart. The table's only UNIQUE key is
    // leaf_index (PRIMARY KEY), which grows every rescan (GetChainLeafCount),
    // so `INSERT OR IGNORE` never caught the duplicate — the same commitment
    // got re-inserted at a fresh index, doubling the tree and drifting every
    // note's leaf position, corrupting spend Merkle paths. Dedup on commitment
    // as an idempotent no-op: return success WITHOUT inserting so the caller's
    // count-delta check (new_count == leaf_index+1) is false and it skips the
    // in-memory tree append.
    {
        sqlite3_stmt* chk = nullptr;
        const char* q =
            "SELECT 1 FROM shielded_tree_leaves WHERE commitment = ? LIMIT 1";
        if (sqlite3_prepare_v2(db_, q, -1, &chk, nullptr) == SQLITE_OK) {
            BindHash(chk, 1, commitment);
            const bool exists = (sqlite3_step(chk) == SQLITE_ROW);
            sqlite3_finalize(chk);
            if (exists) return true;
        }
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT OR IGNORE INTO shielded_tree_leaves (leaf_index, commitment, created_height) "
        "VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(leaf_index));
    BindHash(stmt, 2, commitment);
    sqlite3_bind_int(stmt, 3, static_cast<int>(created_height));
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool ShieldedNoteStore::TruncateChainLeaves(uint64_t new_size) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM shielded_tree_leaves WHERE leaf_index >= ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(new_size));
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<sh::Hash> ShieldedNoteStore::LoadChainLeaves() const {
    std::vector<sh::Hash> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT commitment FROM shielded_tree_leaves ORDER BY leaf_index ASC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sh::Hash leaf{};
        if (ReadHash(stmt, 0, leaf)) {
            out.push_back(leaf);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

uint64_t ShieldedNoteStore::GetChainLeafCount() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM shielded_tree_leaves";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

} // namespace dinero::wallet
