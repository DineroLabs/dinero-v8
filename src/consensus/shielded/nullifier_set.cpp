/**
 * Nullifier set — SQLite-backed, O(1) membership check.
 * See include/consensus/shielded/nullifier_set.h.
 */

#include "consensus/shielded/nullifier_set.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstring>

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
    char* err = nullptr;
    if (sqlite3_exec(db_, kCreateSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        Close();
        return OpenResult::SchemaError;
    }
    return OpenResult::Ok;
}

void NullifierSet::Close() noexcept {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool NullifierSet::Contains(const Hash& nullifier) const {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM nullifiers WHERE nullifier = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(stmt, 1, nullifier.data(), HASH_BYTES, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool NullifierSet::Insert(const Hash& nullifier, uint32_t block_height) {
    if (!db_) return false;
    if (Contains(nullifier)) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO nullifiers (nullifier, block_height) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(stmt, 1, nullifier.data(), HASH_BYTES, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, static_cast<int>(block_height));
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

void NullifierSet::RollbackAbove(uint32_t height) {
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM nullifiers WHERE block_height > ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, static_cast<int>(height));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void NullifierSet::Clear() {
    if (!db_) return;
    char* err = nullptr;
    sqlite3_exec(db_, "DELETE FROM nullifiers", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

uint64_t NullifierSet::Size() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM nullifiers";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

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

} // namespace dinero::consensus::shielded
