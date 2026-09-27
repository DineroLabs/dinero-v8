/*
 * V7 P2MR store — SQLite-backed persistence.
 * See include/wallet/v7_p2mr_store.h.
 */

#include "wallet/v7_p2mr_store.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <sqlite3.h>

namespace dinero::wallet {

namespace {

// DDL — kept in one place so a schema migration is a single string edit.
constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS v7_p2mr_addresses ("
    "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  wallet_id        INTEGER NOT NULL,"
    "  address          TEXT    NOT NULL UNIQUE,"
    "  merkle_root      BLOB    NOT NULL,"
    "  pubkey           BLOB    NOT NULL,"
    "  seed_ciphertext  BLOB    NOT NULL,"
    "  seed_nonce       BLOB    NOT NULL,"
    "  seed_tag         BLOB    NOT NULL,"
    "  derivation_path  TEXT    NOT NULL,"
    "  leaf_index       INTEGER NOT NULL DEFAULT 0,"
    "  label            TEXT,"
    "  created_at       INTEGER NOT NULL,"
    "  UNIQUE(wallet_id, derivation_path, leaf_index)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_v7_p2mr_addresses_wallet "
    "  ON v7_p2mr_addresses(wallet_id);"
    "CREATE INDEX IF NOT EXISTS idx_v7_p2mr_addresses_merkle_root "
    "  ON v7_p2mr_addresses(merkle_root);";

class Stmt {
public:
    explicit Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) {
            if (s_) sqlite3_finalize(s_);
            s_ = nullptr;
        }
    }
    ~Stmt() {
        if (s_) sqlite3_finalize(s_);
    }
    sqlite3_stmt* raw() const noexcept { return s_; }
    bool ok() const noexcept { return s_ != nullptr; }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
private:
    sqlite3_stmt* s_ = nullptr;
};

bool BindBlob(sqlite3_stmt* s, int idx, const void* data, std::size_t len) {
    return sqlite3_bind_blob(s, idx, data, static_cast<int>(len), SQLITE_TRANSIENT) == SQLITE_OK;
}
bool BindText(sqlite3_stmt* s, int idx, const std::string& v) {
    return sqlite3_bind_text(s, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

// Read a blob column into a fixed-size array. Returns false on size mismatch.
template <std::size_t N>
bool ReadBlobExact(sqlite3_stmt* s, int col, std::array<uint8_t, N>& out) {
    const void* p = sqlite3_column_blob(s, col);
    const int len = sqlite3_column_bytes(s, col);
    if (p == nullptr || static_cast<std::size_t>(len) != N) return false;
    std::memcpy(out.data(), p, N);
    return true;
}

std::string ReadText(sqlite3_stmt* s, int col) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s, col));
    const int len = sqlite3_column_bytes(s, col);
    if (p == nullptr) return {};
    return std::string(p, static_cast<std::size_t>(len));
}

V7P2MRStore::KeyRecord ReadKeyRecord(sqlite3_stmt* statement, int64_t wallet_id) {
    const auto text = [&](int col, bool optional = false) {
        const auto type = sqlite3_column_type(statement, col);
        if (optional && type == SQLITE_NULL) return std::string{};
        if (type != SQLITE_TEXT) throw std::runtime_error("Invalid P2MR inventory text");
        auto value = ReadText(statement, col);
        if (!optional && (value.empty() || value.find('\0') != std::string::npos))
            throw std::runtime_error("Invalid P2MR inventory text");
        return value;
    };
    for (int col : {0, 1, 6, 8}) {
        if (sqlite3_column_type(statement, col) != SQLITE_INTEGER)
            throw std::runtime_error("Invalid P2MR inventory integer");
    }
    const auto leaf = sqlite3_column_int64(statement, 6);
    if (sqlite3_column_int64(statement, 0) <= 0 ||
        sqlite3_column_int64(statement, 1) != wallet_id || leaf < 0 ||
        leaf > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Invalid P2MR inventory identity");
    for (const auto [col, size] : {std::pair{3, 32}, {4, int(dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES)},
                                  {9, 32}, {10, 12}, {11, 16}}) {
        if (sqlite3_column_type(statement, col) != SQLITE_BLOB ||
            sqlite3_column_bytes(statement, col) != size)
            throw std::runtime_error("Invalid P2MR inventory blob");
    }
    V7P2MRStore::KeyRecord captured{};
    auto& row = captured.metadata;
    row.id        = sqlite3_column_int64(statement, 0);
    row.wallet_id = sqlite3_column_int64(statement, 1);
    row.address   = text(2);
    if (!ReadBlobExact(statement, 3, row.merkle_root) || !ReadBlobExact(statement, 4, row.pubkey))
        throw std::runtime_error("Unreadable P2MR inventory blob");
    row.derivation_path = text(5);
    row.leaf_index      = static_cast<uint32_t>(leaf);
    row.label           = text(7, true);
    row.created_at_unix = sqlite3_column_int64(statement, 8);
    auto& encrypted = captured.encrypted_seed;
    if (!ReadBlobExact(statement, 9, encrypted.ciphertext) ||
        !ReadBlobExact(statement, 10, encrypted.nonce) ||
        !ReadBlobExact(statement, 11, encrypted.tag))
        throw std::runtime_error("Unreadable P2MR ciphertext");
    return captured;
}

} // namespace

V7P2MRStore::OpenResult V7P2MRStore::Open(const std::string& path) {
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

V7P2MRStore::OpenResult V7P2MRStore::OpenExistingReadOnly(const std::string& path) {
    Close();
    if (path.empty() || sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        Close();
        return OpenResult::IoError;
    }
    bool present = false;
    {
        Stmt schema(db_, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='v7_p2mr_addresses'");
        present = schema.ok() && sqlite3_step(schema.raw()) == SQLITE_ROW &&
                  sqlite3_step(schema.raw()) == SQLITE_DONE;
    }
    if (!present) {
        Close();
        return OpenResult::SchemaError;
    }
    return OpenResult::Ok;
}

void V7P2MRStore::Close() noexcept {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

V7P2MRStore::AddResult V7P2MRStore::AddAddress(
    int64_t                 wallet_id,
    const std::string&      address,
    const std::array<uint8_t, 32>& merkle_root,
    const std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES>& pubkey,
    const AeadCiphertext&   seed_ciphertext,
    const AeadNonce&        seed_nonce,
    const AeadTag&          seed_tag,
    const std::string&      derivation_path,
    uint32_t                leaf_index,
    const std::string&      label,
    int64_t                 now_unix) {
    if (!db_) return AddResult::DbError;

    Stmt s(db_,
        "INSERT INTO v7_p2mr_addresses "
        "(wallet_id, address, merkle_root, pubkey, "
        " seed_ciphertext, seed_nonce, seed_tag, "
        " derivation_path, leaf_index, label, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!s.ok()) return AddResult::DbError;

    int idx = 1;
    sqlite3_bind_int64(s.raw(), idx++, wallet_id);
    BindText(s.raw(), idx++, address);
    BindBlob(s.raw(), idx++, merkle_root.data(), merkle_root.size());
    BindBlob(s.raw(), idx++, pubkey.data(), pubkey.size());
    BindBlob(s.raw(), idx++, seed_ciphertext.data(), seed_ciphertext.size());
    BindBlob(s.raw(), idx++, seed_nonce.data(), seed_nonce.size());
    BindBlob(s.raw(), idx++, seed_tag.data(), seed_tag.size());
    BindText(s.raw(), idx++, derivation_path);
    sqlite3_bind_int(s.raw(), idx++, static_cast<int>(leaf_index));
    BindText(s.raw(), idx++, label);
    sqlite3_bind_int64(s.raw(), idx++, now_unix);

    const int rc = sqlite3_step(s.raw());
    if (rc == SQLITE_CONSTRAINT) return AddResult::UniqueConflict;
    if (rc != SQLITE_DONE)        return AddResult::DbError;
    return AddResult::Ok;
}

std::optional<P2MRStoredAddress>
V7P2MRStore::GetByAddress(int64_t wallet_id, const std::string& address) const {
    if (!db_) return std::nullopt;

    Stmt s(db_,
        "SELECT id, wallet_id, address, merkle_root, pubkey, "
        "       derivation_path, leaf_index, label, created_at "
        "FROM v7_p2mr_addresses "
        "WHERE wallet_id = ? AND address = ?;");
    if (!s.ok()) return std::nullopt;
    sqlite3_bind_int64(s.raw(), 1, wallet_id);
    BindText(s.raw(), 2, address);

    if (sqlite3_step(s.raw()) != SQLITE_ROW) return std::nullopt;

    P2MRStoredAddress out{};
    out.id        = sqlite3_column_int64(s.raw(), 0);
    out.wallet_id = sqlite3_column_int64(s.raw(), 1);
    out.address   = ReadText(s.raw(), 2);
    if (!ReadBlobExact(s.raw(), 3, out.merkle_root)) return std::nullopt;
    if (!ReadBlobExact(s.raw(), 4, out.pubkey))      return std::nullopt;
    out.derivation_path = ReadText(s.raw(), 5);
    out.leaf_index      = static_cast<uint32_t>(sqlite3_column_int(s.raw(), 6));
    out.label           = ReadText(s.raw(), 7);
    out.created_at_unix = sqlite3_column_int64(s.raw(), 8);
    return out;
}

std::optional<P2MRStoredAddress>
V7P2MRStore::GetByMerkleRoot(int64_t wallet_id,
                             const std::array<uint8_t, 32>& merkle_root) const {
    if (!db_) return std::nullopt;

    Stmt s(db_,
        "SELECT id, wallet_id, address, merkle_root, pubkey, "
        "       derivation_path, leaf_index, label, created_at "
        "FROM v7_p2mr_addresses "
        "WHERE wallet_id = ? AND merkle_root = ?;");
    if (!s.ok()) return std::nullopt;
    sqlite3_bind_int64(s.raw(), 1, wallet_id);
    BindBlob(s.raw(), 2, merkle_root.data(), merkle_root.size());

    if (sqlite3_step(s.raw()) != SQLITE_ROW) return std::nullopt;

    P2MRStoredAddress out{};
    out.id        = sqlite3_column_int64(s.raw(), 0);
    out.wallet_id = sqlite3_column_int64(s.raw(), 1);
    out.address   = ReadText(s.raw(), 2);
    if (!ReadBlobExact(s.raw(), 3, out.merkle_root)) return std::nullopt;
    if (!ReadBlobExact(s.raw(), 4, out.pubkey))      return std::nullopt;
    out.derivation_path = ReadText(s.raw(), 5);
    out.leaf_index      = static_cast<uint32_t>(sqlite3_column_int(s.raw(), 6));
    out.label           = ReadText(s.raw(), 7);
    out.created_at_unix = sqlite3_column_int64(s.raw(), 8);
    return out;
}

std::vector<P2MRStoredAddress>
V7P2MRStore::ListByWallet(int64_t wallet_id) const {
    std::vector<P2MRStoredAddress> out;
    if (!db_ || wallet_id <= 0)
        throw std::runtime_error("P2MR inventory owner unavailable");

    Stmt s(db_,
        "SELECT id, wallet_id, address, merkle_root, pubkey, "
        "       derivation_path, leaf_index, label, created_at, "
        "       seed_ciphertext, seed_nonce, seed_tag "
        "FROM v7_p2mr_addresses "
        "WHERE wallet_id = ? "
        "ORDER BY created_at ASC, id ASC;");
    if (!s.ok() || sqlite3_bind_int64(s.raw(), 1, wallet_id) != SQLITE_OK)
        throw std::runtime_error("P2MR inventory query unavailable");

    int rc;
    while ((rc = sqlite3_step(s.raw())) == SQLITE_ROW) {
        out.push_back(ReadKeyRecord(s.raw(), wallet_id).metadata);
    }
    if (rc != SQLITE_DONE) throw std::runtime_error("Incomplete P2MR inventory read");
    return out;
}

std::optional<V7P2MRStore::KeyRecord>
V7P2MRStore::CaptureKeyByAddress(int64_t wallet_id, const std::string& address) const {
    if (!db_ || wallet_id <= 0 || address.empty() ||
        address.find('\0') != std::string::npos || !sqlite3_get_autocommit(db_))
        throw std::runtime_error("P2MR key capture owner unavailable");
    Stmt s(db_,
        "SELECT id, wallet_id, address, merkle_root, pubkey, "
        "       derivation_path, leaf_index, label, created_at, "
        "       seed_ciphertext, seed_nonce, seed_tag "
        "FROM v7_p2mr_addresses WHERE wallet_id = ? AND address = ?;");
    if (!s.ok() || sqlite3_bind_int64(s.raw(), 1, wallet_id) != SQLITE_OK ||
        !BindText(s.raw(), 2, address))
        throw std::runtime_error("P2MR key capture query unavailable");
    const int rc = sqlite3_step(s.raw());
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) throw std::runtime_error("P2MR key capture read failed");
    auto captured = ReadKeyRecord(s.raw(), wallet_id);
    if (captured.metadata.address != address || sqlite3_step(s.raw()) != SQLITE_DONE)
        throw std::runtime_error("P2MR key capture incomplete or ambiguous");
    return captured;
}

std::optional<V7P2MRStore::EncryptedSeed>
V7P2MRStore::LoadEncryptedSeed(int64_t wallet_id, const std::string& address) const {
    if (!db_) return std::nullopt;

    Stmt s(db_,
        "SELECT seed_ciphertext, seed_nonce, seed_tag "
        "FROM v7_p2mr_addresses "
        "WHERE wallet_id = ? AND address = ?;");
    if (!s.ok()) return std::nullopt;
    sqlite3_bind_int64(s.raw(), 1, wallet_id);
    BindText(s.raw(), 2, address);
    if (sqlite3_step(s.raw()) != SQLITE_ROW) return std::nullopt;

    EncryptedSeed out{};
    if (!ReadBlobExact(s.raw(), 0, out.ciphertext)) return std::nullopt;
    if (!ReadBlobExact(s.raw(), 1, out.nonce))      return std::nullopt;
    if (!ReadBlobExact(s.raw(), 2, out.tag))        return std::nullopt;
    return out;
}

} // namespace dinero::wallet
