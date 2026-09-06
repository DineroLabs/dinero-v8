/**
 * Nullifier set — SQLite-backed, O(1) membership check.
 * See include/consensus/shielded/nullifier_set.h.
 */

#include "consensus/shielded/nullifier_set.h"

#include <optional>

#include <sqlite3.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace dinero::consensus::shielded {

namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS nullifiers ("
    "  nullifier BLOB PRIMARY KEY NOT NULL,"
    "  block_height INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_nullifiers_height "
    "  ON nullifiers(block_height);";

} // namespace

NullifierSet::~NullifierSet() { Close(); }

NullifierSet::OpenResult NullifierSet::Open(const std::string& path) {
    Close();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        Close();
        return OpenResult::IoError;
    }
    // Read user_version BEFORE creating the schema. A legacy file predating
    // ChainDB authority leaves it at sqlite's default of 0; every file this
    // build creates or migrates is stamped kCacheSchemaVersion.
    //
    // nullopt, not 0, when the read fails. Defaulting to 0 here meant "we
    // could not read it" and "this is an unstamped legacy file" were the same
    // value, and the branch below acts on that difference irreversibly.
    const auto existing_version = TryReadUserVersion();

    char* err = nullptr;
    if (sqlite3_exec(db_, kCreateSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        Close();
        return OpenResult::SchemaError;
    }

    // Only ask for the count when the decision can actually need it, but pass
    // its failure through rather than flattening it to 0.
    const auto row_count = TryCount();

    switch (DecideProvenance(existing_version, row_count)) {
        case OpenDecision::AlreadyCache:
            // Already declared a cache. Its rows are NEVER authoritative,
            // whatever ChainDB's count happens to be -- which is what closes
            // the crash window: a post-crash cache looks identical to a legacy
            // file by row count alone, and differs only here.
            provenance_ = Provenance::Cache;
            return OpenResult::Ok;

        case OpenDecision::LegacyCandidate:
            // Unstamped AND populated: a genuine pre-authority database. This
            // is the only state in which sqlite rows may be promoted, and only
            // once.
            provenance_ = Provenance::LegacyCandidate;
            return OpenResult::Ok;

        case OpenDecision::StampFresh:
            // Unstamped and empty: nothing to migrate. Stamp it now so a crash
            // that fills it cannot later be mistaken for legacy.
            provenance_ = Provenance::FreshCache;
            if (!MarkAsCache()) {
                // The stamp is the whole mechanism. If it did not land, do not
                // report a decided state -- reopening would see an unstamped
                // file and could reach a different conclusion.
                provenance_ = Provenance::Unknown;
                Close();
                return OpenResult::IoError;
            }
            return OpenResult::Ok;

        case OpenDecision::Indeterminate:
        default:
            // A read this decision depends on failed. Do not stamp, do not
            // guess: both wrong answers are unrecoverable. Guessing "cache"
            // stamps a real legacy set into permanent non-authority; guessing
            // "legacy" promotes crash residue and makes honest notes
            // unspendable. Refusing to open is the only reversible outcome --
            // a transient SQLITE_BUSY then costs a retry, not the user's
            // shielded history.
            provenance_ = Provenance::Unknown;
            Close();
            return OpenResult::Indeterminate;
    }
}

// The rule, as a pure function. Exhaustively testable, including the states
// that only arise under sqlite failure.
NullifierSet::OpenDecision NullifierSet::DecideProvenance(
    std::optional<int> user_version, std::optional<uint64_t> row_count) {
    if (!user_version.has_value()) return OpenDecision::Indeterminate;
    if (*user_version >= kCacheSchemaVersion) return OpenDecision::AlreadyCache;
    // Unstamped. The count now decides, so an unreadable count is fatal to the
    // decision -- it is exactly the case that used to read as "empty".
    if (!row_count.has_value()) return OpenDecision::Indeterminate;
    return (*row_count > 0) ? OpenDecision::LegacyCandidate
                            : OpenDecision::StampFresh;
}

// One scalar read, one place that decides what "failed" means.
//
// Both callers below are load-bearing for whether a database's rows may ever
// be treated as authoritative, and both used to answer 0 on failure. Sharing
// this helper means there is a single rc check to get right -- and a single
// one to break, so a test that reaches either caller defends both.
std::optional<int64_t> NullifierSet::ScalarQuery(const char* sql) const {
    if (!db_) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const int rc = sqlite3_step(stmt);
    const int64_t value =
        (rc == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW) return std::nullopt;  // ONLY a row is an answer
    return value;
}

std::optional<int> NullifierSet::TryReadUserVersion() const {
    const auto v = ScalarQuery("PRAGMA user_version");
    if (!v.has_value()) return std::nullopt;
    return static_cast<int>(*v);
}

std::optional<uint64_t> NullifierSet::TryCount() const {
    const auto v = ScalarQuery("SELECT COUNT(*) FROM nullifiers");
    if (!v.has_value()) return std::nullopt;
    return static_cast<uint64_t>(*v);
}

void NullifierSet::Close() noexcept {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// FAILS CLOSED. Both consensus callers use this as the double-spend gate
// (`if (Contains(nullifier)) reject`), so the only safe answer when we cannot
// prove a nullifier is absent is "present" — reject the spend.
//
// Previously every failure path returned false, i.e. "not spent, go ahead":
// a null handle, a failed prepare, or any sqlite error (lock contention, disk
// pressure, corruption) silently ADMITTED a double-spend with nothing logged
// and nothing propagated to the caller. Only SQLITE_DONE — the database
// positively answering "no such row" — may return false.
bool NullifierSet::Contains(const Hash& nullifier) const {
    if (!db_) return true;  // cannot prove unspent

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM nullifiers WHERE nullifier = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return true;  // cannot prove unspent
    }
    sqlite3_bind_blob(stmt, 1, nullifier.data(), HASH_BYTES, SQLITE_STATIC);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_ROW)  return true;   // present: reject the spend
    if (rc == SQLITE_DONE) return false;  // positively absent: the ONLY false
    return true;                          // any error: cannot prove unspent
}

bool NullifierSet::Insert(const Hash& nullifier, uint32_t block_height) {
    if (!db_) return false;
    if (Contains(nullifier)) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO nullifiers (nullifier, block_height) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(stmt, 1, nullifier.data(), HASH_BYTES, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(block_height));
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool NullifierSet::MarkAsCache() {
    if (!db_) return false;
    const std::string sql =
        "PRAGMA user_version = " + std::to_string(kCacheSchemaVersion);
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }
    provenance_ = Provenance::Cache;
    return true;
}

bool NullifierSet::InsertBatch(const std::vector<std::pair<Hash, uint32_t>>& entries) {
    if (!db_) return false;
    if (entries.empty()) return true;

    // BEGIN IMMEDIATE, not deferred: take the write lock up front so the
    // transaction cannot fail to upgrade partway through.
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    bool ok = true;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO nullifiers (nullifier, block_height) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        ok = false;
    } else {
        for (const auto& [nullifier, height] : entries) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_blob(stmt, 1, nullifier.data(), HASH_BYTES, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(height));
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                // Covers a duplicate inside the batch (UNIQUE violation) as
                // well as any I/O fault. Either way the whole batch is void.
                ok = false;
                break;
            }
        }
        sqlite3_finalize(stmt);
    }

    if (!ok) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

void NullifierSet::RollbackAbove(uint32_t height) {
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM nullifiers WHERE block_height > ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(height));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void NullifierSet::Clear() {
    if (!db_) return;
    char* err = nullptr;
    sqlite3_exec(db_, "DELETE FROM nullifiers", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

// Diagnostics only. Reports 0 for both "empty" and "unreadable"; branch on
// TryCount() instead, which keeps those apart.
uint64_t NullifierSet::Size() const { return TryCount().value_or(0); }

std::vector<uint8_t> NullifierSet::SerializeContent() const {
    std::vector<uint8_t> out;
    if (!db_) return out;

    constexpr uint32_t kTag      = 0x4653434E;  // 'NSCF' little-endian
    constexpr uint16_t kVersion  = 1;

    auto write_u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    auto write_u16 = [&out](uint16_t v) {
        for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    auto write_u64 = [&out](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };

    write_u32(kTag);
    write_u16(kVersion);

    // First pass: count, so the header is set before we stream entries.
    uint64_t count = Size();
    write_u64(count);

    // Stream sorted (block_height ASC, nullifier ASC). sqlite ORDER BY
    // makes the output deterministic across nodes at the same tip.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT block_height, nullifier FROM nullifiers "
        "ORDER BY block_height ASC, nullifier ASC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // Best effort — return what we have so far. The header still
        // distinguishes "0 entries" from "N entries we couldn't read"
        // because count was queried before this prepare.
        return out;
    }
    uint64_t streamed = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto height = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
        const void* blob = sqlite3_column_blob(stmt, 1);
        const int blob_len = sqlite3_column_bytes(stmt, 1);
        if (blob == nullptr || blob_len != static_cast<int>(HASH_BYTES)) {
            // Malformed row — abort the stream rather than emit a
            // truncated entry that would corrupt the hash.
            sqlite3_finalize(stmt);
            return std::vector<uint8_t>{};  // signal error via empty
        }
        write_u32(height);
        const uint8_t* bytes = static_cast<const uint8_t*>(blob);
        out.insert(out.end(), bytes, bytes + HASH_BYTES);
        ++streamed;
    }
    sqlite3_finalize(stmt);

    // If the count we wrote into the header doesn't match what we
    // streamed (a concurrent insert/delete between Size() and the
    // ORDER BY scan, which shouldn't happen on the consensus path
    // but could on a debug RPC call), return empty so the caller's
    // hash is undefined rather than wrong.
    if (streamed != count) {
        return std::vector<uint8_t>{};
    }
    return out;
}

bool NullifierSet::DeserializeContent(const std::vector<uint8_t>& bytes) {
    if (!db_) return false;
    Clear();                          // start from a clean table
    if (bytes.empty()) return true;   // empty content == empty set (valid)

    constexpr uint32_t kTag = 0x4653434E;  // 'NSCF' little-endian
    size_t off = 0;
    auto need = [&](size_t n) { return off + n <= bytes.size(); };
    auto read_u16 = [&]() { uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= static_cast<uint16_t>(bytes[off++]) << (i * 8); return v; };
    auto read_u32 = [&]() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(bytes[off++]) << (i * 8); return v; };
    auto read_u64 = [&]() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(bytes[off++]) << (i * 8); return v; };

    if (!need(4 + 2 + 8)) return false;
    const uint32_t tag = read_u32();
    const uint16_t version = read_u16();
    const uint64_t count = read_u64();
    if (tag != kTag || version != 1) { Clear(); return false; }

    for (uint64_t i = 0; i < count; ++i) {
        if (!need(4 + HASH_BYTES)) { Clear(); return false; }
        const uint32_t height = read_u32();
        Hash nf{};
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                  bytes.begin() + static_cast<std::ptrdiff_t>(off + HASH_BYTES),
                  nf.begin());
        off += HASH_BYTES;
        if (!Insert(nf, height)) { Clear(); return false; }
    }
    if (off != bytes.size()) { Clear(); return false; }  // trailing garbage
    return true;
}

void NullifierSet::InterruptForTesting() {
    if (db_) sqlite3_interrupt(db_);
}

bool NullifierSet::ForEach(const Visitor& visit) const {
    if (!db_) return false;
    if (!visit) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT block_height, nullifier FROM nullifiers "
        "ORDER BY block_height ASC, nullifier ASC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    // Only SQLITE_DONE means "you have seen every row".
    //
    // The loop below previously exited on anything that was not SQLITE_ROW and
    // then returned success. SQLITE_BUSY, SQLITE_IOERR, SQLITE_CORRUPT and
    // SQLITE_INTERRUPT all take that exit, so ordinary lock contention
    // produced a TRUNCATED row set reported as a complete one — and
    // AccumulateNullifierSet's fail-closed guard, which exists precisely so a
    // local fault cannot look like an attacker having deleted every
    // nullifier, never ran because this function told it everything was fine.
    bool ok = true;
    bool stopped_by_visitor = false;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const auto height = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
        const void* blob = sqlite3_column_blob(stmt, 1);
        const int blob_len = sqlite3_column_bytes(stmt, 1);
        if (blob == nullptr || blob_len != static_cast<int>(HASH_BYTES)) {
            ok = false;
            break;
        }
        if (!visit(height, static_cast<const uint8_t*>(blob))) {
            stopped_by_visitor = true;
            break;
        }
    }
    sqlite3_finalize(stmt);

    // A visitor that stops early also leaves the caller without a complete
    // view. Both current callers already fail in that case; saying so here
    // makes "true" mean exactly one thing: you saw the whole set.
    if (!ok || stopped_by_visitor) return false;
    return rc == SQLITE_DONE;
}

} // namespace dinero::consensus::shielded
