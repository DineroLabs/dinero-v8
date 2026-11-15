#include "wallet/wallet_index_v3_coin_view.h"
#include "wallet/wallet_index_v3_schema.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace dinero {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite3_finalize(raw);
        throw std::runtime_error("Wallet index v3 coin query unavailable");
    }
    return Statement(raw, sqlite3_finalize);
}

int64_t ReadIntegerPragma(sqlite3* db, const char* query) {
    auto q = Prepare(db, query);
    if (sqlite3_step(q.get()) != SQLITE_ROW ||
        sqlite3_column_type(q.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(q.get(), 0) < 0)
        throw std::runtime_error("Wallet index v3 schema query failed");
    const auto result = sqlite3_column_int64(q.get(), 0);
    if (sqlite3_step(q.get()) != SQLITE_DONE)
        throw std::runtime_error("Wallet index v3 schema query incomplete");
    return result;
}
} // namespace

WalletIndexV3CoinView::WalletIndexV3CoinView(const std::filesystem::path& index_file) {
    // A read-only source must already have the verified v3 schema and its
    // immutable v2 rollback backup. Never upgrade here.
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(index_file)))
        throw std::runtime_error("Wallet index v3 coin source link refused");
    const auto parent = index_file.parent_path().empty()
        ? std::filesystem::path(".") : index_file.parent_path();
    const auto utf8 = (std::filesystem::canonical(parent) / index_file.filename()).u8string();
    const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (sqlite3_open_v2(filename.c_str(), &db_,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Wallet index v3 coin source open failed");
    }
    sqlite3_busy_timeout(db_, 10000);
    try {
        // Verify the same connection used for every later coin query.
        RequireFullArchiveWalletIndexV3(db_, index_file);
        if (ReadIntegerPragma(db_, "PRAGMA user_version") != 3)
            throw std::runtime_error("Wallet index v3 coin source changed");
        schema_version_ = ReadIntegerPragma(db_, "PRAGMA schema_version");
    }
    catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}

WalletIndexV3CoinView::~WalletIndexV3CoinView() {
    if (db_) sqlite3_close(db_);
}

std::optional<uint64_t> WalletIndexV3CoinView::ReadRawCoinValue(
        const WalletIndexV3OwnerToken& owner,
        const WalletManager::DatabaseLease& lease,
        const std::string& txid_hex, uint32_t vout) const {
    std::lock_guard lock(mutex_);
    owner.RequireCurrent(lease);
    if (txid_hex.size() != 64 ||
        !std::all_of(txid_hex.begin(), txid_hex.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw std::runtime_error("Wallet index v3 transaction id malformed");
    if (ReadIntegerPragma(db_, "PRAGMA user_version") != 3 ||
        ReadIntegerPragma(db_, "PRAGMA schema_version") != schema_version_)
        throw std::runtime_error("Wallet index v3 coin schema changed");
    auto q = Prepare(db_, "SELECT value FROM wallet_utxos "
        "WHERE wallet_identity=? AND txid=? AND vout=?");
    owner.BindCurrentIdentity(q.get(), 1, lease);
    if (sqlite3_bind_text(q.get(), 2, txid_hex.data(), int(txid_hex.size()),
            SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(q.get(), 3, vout) != SQLITE_OK)
        throw std::runtime_error("Wallet index v3 coin key binding failed");
    const int result = sqlite3_step(q.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW || sqlite3_column_type(q.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(q.get(), 0) < 0)
        throw std::runtime_error("Wallet index v3 coin value invalid");
    const auto value = uint64_t(sqlite3_column_int64(q.get(), 0));
    if (sqlite3_step(q.get()) != SQLITE_DONE)
        throw std::runtime_error("Wallet index v3 coin lookup incomplete");
    return value;
}

uint64_t WalletIndexV3CoinView::CountRawUnspent(
        const WalletIndexV3OwnerToken& owner,
        const WalletManager::DatabaseLease& lease) const {
    std::lock_guard lock(mutex_);
    owner.RequireCurrent(lease);
    if (ReadIntegerPragma(db_, "PRAGMA user_version") != 3 ||
        ReadIntegerPragma(db_, "PRAGMA schema_version") != schema_version_)
        throw std::runtime_error("Wallet index v3 coin schema changed");
    auto q = Prepare(db_, "SELECT count(*) FROM wallet_utxos "
        "WHERE wallet_identity=? AND spend_height IS NULL");
    owner.BindCurrentIdentity(q.get(), 1, lease);
    if (sqlite3_step(q.get()) != SQLITE_ROW ||
        sqlite3_column_type(q.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(q.get(), 0) < 0)
        throw std::runtime_error("Wallet index v3 coin count invalid");
    const auto count = uint64_t(sqlite3_column_int64(q.get(), 0));
    if (sqlite3_step(q.get()) != SQLITE_DONE)
        throw std::runtime_error("Wallet index v3 coin count incomplete");
    return count;
}

} // namespace dinero
