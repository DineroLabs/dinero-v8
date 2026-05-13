/*
 * V7 P2MR store — SQLite-backed persistence.
 * See include/wallet/v7_p2mr_store.h.
 */

#include "wallet/v7_p2mr_store.h"

#include <cstring>
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
        sqlite3_prepare_v2(db, sql, -1, &s_, nullptr);
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
    if (!db_) return out;

    Stmt s(db_,
        "SELECT id, wallet_id, address, merkle_root, pubkey, "
        "       derivation_path, leaf_index, label, created_at "
        "FROM v7_p2mr_addresses "
        "WHERE wallet_id = ? "
        "ORDER BY created_at ASC, id ASC;");
    if (!s.ok()) return out;
    sqlite3_bind_int64(s.raw(), 1, wallet_id);

    while (sqlite3_step(s.raw()) == SQLITE_ROW) {
        P2MRStoredAddress row{};
        row.id        = sqlite3_column_int64(s.raw(), 0);
        row.wallet_id = sqlite3_column_int64(s.raw(), 1);
        row.address   = ReadText(s.raw(), 2);
        if (!ReadBlobExact(s.raw(), 3, row.merkle_root)) continue;
        if (!ReadBlobExact(s.raw(), 4, row.pubkey))      continue;
        row.derivation_path = ReadText(s.raw(), 5);
        row.leaf_index      = static_cast<uint32_t>(sqlite3_column_int(s.raw(), 6));
        row.label           = ReadText(s.raw(), 7);
        row.created_at_unix = sqlite3_column_int64(s.raw(), 8);
        out.push_back(std::move(row));
    }
    return out;
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
