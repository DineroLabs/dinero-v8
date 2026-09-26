#include "wallet/wallet_manager.h"
#include "wallet/shielded_wallet_ops.h"
#include "wallet/wallet_worker.h"
#include "wallet/utxo_index.h"
#include "consensus/chainparams.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

namespace dinero {
struct WalletWorkerTestAccess {
    static void Connect(WalletWorker& worker, uint32_t height,
                        const std::vector<Transaction>& transactions) {
        worker.ProcessConnect(height, std::string(64, '1'), transactions);
    }
};
}
namespace {
using namespace std::chrono_literals;

class WalletDatabaseLeaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
            ("dinero_wallet_lease_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        wallet = std::make_unique<dinero::WalletManager>(path);
        wallet->create("owner");
    }
    void TearDown() override {
        wallet.reset();
        std::filesystem::remove_all(path);
    }
    static void Exec(sqlite3* db, const char* sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        if (rc != SQLITE_OK) throw std::runtime_error(message);
    }
    static int Count(sqlite3* db) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM lease_probe", -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare probe");
        const int rc = sqlite3_step(stmt);
        const int count = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return count;
    }
    std::filesystem::path path;
    std::unique_ptr<dinero::WalletManager> wallet;
};

TEST_F(WalletDatabaseLeaseTest, SerializesConnectionAndPinsWalletThroughSwitch) {
    wallet->create("other");
    wallet->open("owner");
    auto lease = wallet->AcquireDatabaseLease();
    ASSERT_EQ(lease->WalletName(), "owner");
    ASSERT_EQ(lease->Database(), wallet->getCurrentDatabase());
    sqlite3* db = lease->Database();
    // A deterministic probe of SQLite's real connection mutex, not a mocked
    // writer or a timing-only observation that another thread hasn't run.
    auto attempt = std::async(std::launch::async, [db] {
        auto* mutex = sqlite3_db_mutex(db);
        const int rc = sqlite3_mutex_try(mutex);
        if (rc == SQLITE_OK) sqlite3_mutex_leave(mutex);
        return rc;
    });
    EXPECT_EQ(attempt.get(), SQLITE_BUSY);
    EXPECT_THROW(wallet->open("other"), std::logic_error);
    EXPECT_THROW(wallet->create("reentrant"), std::logic_error);
    EXPECT_FALSE(wallet->exists("reentrant"));
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto change = std::async(std::launch::async, [&] {
        entered.set_value();
        wallet->open("other");
    });
    ready.wait();
    EXPECT_EQ(change.wait_for(100ms), std::future_status::timeout);
    EXPECT_EQ(lease->WalletName(), "owner");
    lease.reset();
    EXPECT_EQ(change.wait_for(5s), std::future_status::ready);
    EXPECT_NO_THROW(change.get());
    EXPECT_EQ(wallet->AcquireDatabaseLease()->WalletName(), "other");
}

TEST_F(WalletDatabaseLeaseTest, RejectsBorrowedTransactionAndRollsBackOnlyOwnedWork) {
    sqlite3* db = wallet->getCurrentDatabase();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    EXPECT_THROW(wallet->AcquireDatabaseLease(), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Count(db), 1);
    Exec(db, "ROLLBACK");
    {
        auto lease = wallet->AcquireDatabaseLease();
        Exec(lease->Database(), "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(2)");
        EXPECT_EQ(Count(lease->Database()), 1);
    }
    EXPECT_EQ(sqlite3_get_autocommit(db), 1);
    EXPECT_EQ(Count(db), 0);
    {
        auto lease = wallet->AcquireDatabaseLease();
        Exec(lease->Database(), "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(3); COMMIT");
    }
    wallet.reset();
    wallet = std::make_unique<dinero::WalletManager>(path);
    wallet->open("owner");
    EXPECT_EQ(Count(wallet->getCurrentDatabase()), 1);
}

TEST_F(WalletDatabaseLeaseTest, ConcurrentRawWriteCannotJoinLeasedTransaction) {
    auto lease = wallet->AcquireDatabaseLease();
    sqlite3* db = lease->Database();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto write = std::async(std::launch::async, [&] {
        entered.set_value();
        Exec(db, "INSERT INTO lease_probe VALUES(2)");
    });
    ready.wait();
    EXPECT_EQ(write.wait_for(100ms), std::future_status::timeout);
    lease.reset(); // rolls back 1 before the competing statement can execute
    EXPECT_NO_THROW(write.get());
    EXPECT_EQ(Count(db), 1);
}

TEST_F(WalletDatabaseLeaseTest, EmptySelectionCanBeLeasedWithoutFabricatedDatabase) {
    wallet.reset();
    wallet = std::make_unique<dinero::WalletManager>(path);
    auto lease = wallet->AcquireDatabaseLease();
    EXPECT_EQ(lease->Database(), nullptr);
    EXPECT_TRUE(lease->WalletName().empty());
    EXPECT_THROW(wallet->open("owner"), std::logic_error);
    lease.reset();
    EXPECT_NO_THROW(wallet->open("owner"));
}

TEST_F(WalletDatabaseLeaseTest, NestedOwnerPreservesOuterTransaction) {
    auto outer = wallet->AcquireDatabaseLease();
    sqlite3* db = outer->Database();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    {
        auto inner = wallet->AcquireDatabaseLease();
        EXPECT_EQ(inner->Database(), db);
        std::string error;
        EXPECT_TRUE(dinero::wallet::shielded_ops::EnsureWalletRuntime(*wallet, &error)) << error;
    }
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Count(db), 1);
    outer.reset();
    EXPECT_EQ(sqlite3_get_autocommit(db), 1);
    EXPECT_EQ(Count(db), 0);
}

TEST_F(WalletDatabaseLeaseTest, RuntimePinsWalletBeforeTakingSharedRuntimeLock) {
    dinero::WalletManager other(path / "other-manager");
    other.create("other");
    auto lease = wallet->AcquireDatabaseLease();
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto first = std::async(std::launch::async, [&] {
        entered.set_value();
        return dinero::wallet::shielded_ops::EnsureWalletRuntime(*wallet, nullptr);
    });
    ready.wait();
    // This wallet's runtime must respect the already-held lifetime lease.
    EXPECT_EQ(first.wait_for(200ms), std::future_status::timeout);
    auto second = std::async(std::launch::async, [&] {
        return dinero::wallet::shielded_ops::EnsureWalletRuntime(other, nullptr);
    });
    // Waiting for one wallet must not hold the process-wide runtime mutex and
    // block a different wallet. Always release before joining, even on failure.
    EXPECT_EQ(second.wait_for(2s), std::future_status::ready);
    lease.reset();
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
}
TEST_F(WalletDatabaseLeaseTest, RealWorkerCommitsIndexBlockTogether) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    dinero::UTXOIndex index((path / "worker-index.sqlite").string());
    ASSERT_TRUE(index.Initialize());
    const std::vector<uint8_t> script{0x51};
    index.RegisterAddress(script, "m/84'/1448'/0'/0/0");
    dinero::uint256 hash; hash.begin()[0] = 1;
    dinero::TxId seed(hash);
    ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(seed, 0, dinero::AmountUna::Una(1000),
        script, "m/84'/1448'/0'/0/0", 10, false)));
    dinero::Transaction first;
    first.vin.resize(1); first.vin[0].prevout = dinero::TxOutPoint(seed, 0);
    first.vout.emplace_back(dinero::AmountUna::Una(800), script);
    dinero::Transaction second;
    second.vin.resize(1); second.vin[0].prevout = dinero::TxOutPoint(first.GetTxid(), 0);
    second.vout.emplace_back(dinero::AmountUna::Una(600), script);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open((path / "worker-index.sqlite").string().c_str(), &raw), SQLITE_OK);
    const auto close = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(raw, sqlite3_close);
    Exec(raw, "CREATE TRIGGER fail_second BEFORE INSERT ON wallet_utxos WHEN NEW.value=600 BEGIN SELECT RAISE(ABORT, 'test worker insert'); END");
    dinero::WalletWorker worker(&index, nullptr); // Real worker/index, no other store claim.
    std::string error;
    try { dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}); }
    catch (const std::runtime_error& e) { error = e.what(); }
    EXPECT_EQ(error, "Wallet UTXO block insert failed");
    ASSERT_TRUE(index.GetUTXO(seed, 0));
    EXPECT_FALSE(index.GetUTXO(seed, 0)->spend_height);
    EXPECT_FALSE(index.GetUTXO(first.GetTxid(), 0));
    EXPECT_FALSE(index.GetUTXO(second.GetTxid(), 0));
    Exec(raw, "DROP TRIGGER fail_second");
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
    ASSERT_TRUE(index.GetUTXO(first.GetTxid(), 0));
    EXPECT_EQ(index.GetUTXO(seed, 0)->spend_height, 20);
    EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
    EXPECT_FALSE(index.GetUTXO(second.GetTxid(), 0)->spend_height);
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
    EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
}

TEST_F(WalletDatabaseLeaseTest, RealWorkerChecksOrdinaryWalletWritesAndCommitBeforeHeight) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    const std::vector<uint8_t> script{0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    auto scalar = [](sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare state query");
        const int rc = sqlite3_step(stmt);
        const int result = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return result;
    };
    for (const std::string stage : {"insert", "spend", "history", "confirmation", "commit"}) {
        SCOPED_TRACE(stage);
        wallet->create("block-" + stage);
        sqlite3* db = wallet->getCurrentDatabase();
        dinero::UTXOIndex index((path / (stage + "-index.sqlite")).string());
        ASSERT_TRUE(index.Initialize());
        index.RegisterAddress(script, "m/84'/1448'/0'/0/0");
        dinero::uint256 hash; hash.begin()[0] = 2;
        dinero::Transaction first;
        first.vin.resize(1); first.vin[0].prevout = dinero::TxOutPoint(dinero::TxId(hash), 0);
        first.vout.emplace_back(dinero::AmountUna::Una(800), script);
        dinero::Transaction second;
        second.vin.resize(1); second.vin[0].prevout = dinero::TxOutPoint(first.GetTxid(), 0);
        second.vout.emplace_back(dinero::AmountUna::Una(600), script);
        const auto first_id = first.GetTxid().AsUint256().GetHex();
        if (stage == "confirmation") {
            ASSERT_TRUE(wallet->addTransaction(first_id, "pending", 0.000008, "receive", false));
            Exec(db, "CREATE TRIGGER fail_block BEFORE UPDATE ON transactions BEGIN SELECT RAISE(ABORT, 'test confirmation'); END");
        } else if (stage == "insert") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE INSERT ON utxos WHEN NEW.amount=600 BEGIN SELECT RAISE(ABORT, 'test ordinary insert'); END");
        } else if (stage == "spend") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE UPDATE OF is_spent ON utxos BEGIN SELECT RAISE(ABORT, 'test ordinary spend'); END");
        } else if (stage == "history") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT, 'test ordinary history'); END");
        } else {
            Exec(db, "PRAGMA foreign_keys=ON; CREATE TABLE commit_parent(id INTEGER PRIMARY KEY); CREATE TABLE commit_child(id INTEGER REFERENCES commit_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER fail_block AFTER INSERT ON utxos BEGIN INSERT INTO commit_child VALUES(1); END");
        }
        const int initial_history = scalar(db, "SELECT count(*) FROM transactions");
        const auto initial_height = wallet->getBlockchainHeight();
        dinero::WalletWorker worker(&index, wallet.get());
        std::string error;
        try { dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}); }
        catch (const std::runtime_error& e) { error = e.what(); }
        if (stage == "confirmation") EXPECT_EQ(error, "Wallet block confirmation failed: test confirmation");
        else if (stage == "commit") EXPECT_EQ(error, "Wallet block commit failed: FOREIGN KEY constraint failed");
        else if (stage == "history") EXPECT_EQ(error, "Wallet block history insert failed");
        else EXPECT_EQ(error, "Wallet block ordinary " + stage + " failed");
        EXPECT_EQ(sqlite3_get_autocommit(db), 1);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM utxos"), 0);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM transactions"), initial_history);
        if (stage == "confirmation") EXPECT_EQ(scalar(db, "SELECT height FROM transactions LIMIT 1"), 0);
        EXPECT_EQ(wallet->getBlockchainHeight(), initial_height);
        // Index commits before ordinary-wallet COMMIT. This is a recoverable
        // prefix to replay, not a claim of atomicity between the two databases.
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0).has_value(), stage == "commit");
        Exec(db, "DROP TRIGGER fail_block");
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
        EXPECT_EQ(wallet->getBlockchainHeight(), 20);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM utxos"), 2);
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=800"), 1);
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=600"), 0);
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
        // A creation-only replay may not erase a recorded spend in either store.
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first}));
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=800"), 1);
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
        wallet->open("owner");
        wallet->open("block-" + stage);
        EXPECT_EQ(scalar(wallet->getCurrentDatabase(), "SELECT is_spent FROM utxos WHERE amount=800"), 1);
    }
}
} // namespace
