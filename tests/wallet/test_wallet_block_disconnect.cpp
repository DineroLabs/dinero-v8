#include "wallet/wallet_manager.h"
#include "wallet/wallet_worker.h"
#include "wallet/utxo_index.h"
#include "consensus/chainparams.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <limits>

namespace dinero {
struct WalletWorkerTestAccess {
    static void Disconnect(WalletWorker& worker, uint32_t height, const Block& block) {
        worker.ProcessDisconnect(height, block);
    }
};
}
namespace {
class WalletBlockDisconnectTest : public ::testing::Test {
protected:
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);
        path = std::filesystem::temp_directory_path() / ("dinero-wallet-disconnect-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        wallet = std::make_unique<dinero::WalletManager>(path);
        wallet->create("rollback");
    }
    void TearDown() override { wallet.reset(); std::filesystem::remove_all(path); }
    static void Exec(sqlite3* db, const char* sql) {
        char* raw = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw);
        std::string error = raw ? raw : ""; sqlite3_free(raw);
        if (rc != SQLITE_OK) throw std::runtime_error(error);
    }
    static int Scalar(sqlite3* db, const char* sql) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error("state query prepare");
        const auto stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(raw, sqlite3_finalize);
        if (sqlite3_step(raw) != SQLITE_ROW) throw std::runtime_error("state query row");
        return sqlite3_column_int(raw, 0);
    }
    std::filesystem::path path;
    std::unique_ptr<dinero::WalletManager> wallet;
};

TEST_F(WalletBlockDisconnectTest, ActualWorkerRollbackFailureRetainsOrdinaryStateThenRetries) {
    sqlite3* db = wallet->getCurrentDatabase();
    const std::vector<uint8_t> script{0x51};
    dinero::uint256 seed_hash; seed_hash.begin()[0] = 1;
    const dinero::TxId seed(seed_hash);
    dinero::Transaction spending;
    spending.vin.resize(1); spending.vin[0].prevout = dinero::TxOutPoint(seed, 0);
    spending.vout.emplace_back(dinero::AmountUna::Una(600), script);
    dinero::Block block; block.vtx.push_back(spending);
    const auto seed_id = seed.AsUint256().GetHex();
    const auto created_id = spending.GetTxid().AsUint256().GetHex();
    for (const std::string stage : {"delete", "history", "restore", "automatic", "commit"}) {
        SCOPED_TRACE(stage);
        Exec(db, "DELETE FROM utxos; DELETE FROM transactions");
        ASSERT_TRUE(wallet->addUTXO(seed_id, 0, 1000, "owned", "51", 10, false));
        ASSERT_TRUE(wallet->spendUTXO(seed_id, 0));
        ASSERT_TRUE(wallet->addUTXO(created_id, 0, 600, "owned", "51", 20, false));
        ASSERT_TRUE(wallet->addTransaction(created_id, "owned", 0.000006, "receive", false, "", 1, 20));
        wallet->setBlockchainHeight(20);
        dinero::UTXOIndex index((path / (stage + "-index.sqlite")).string());
        ASSERT_TRUE(index.Initialize());
        ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(seed, 0, dinero::AmountUna::Una(1000), script, "m/84'/1448'/0'/0/0", 10)));
        ASSERT_TRUE(index.SpendUTXO(seed, 0, 20));
        ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(spending.GetTxid(), 0, dinero::AmountUna::Una(600), script, "m/84'/1448'/0'/0/0", 20)));
        if (stage == "delete")
            Exec(db, "CREATE TRIGGER reject_rollback BEFORE DELETE ON utxos BEGIN SELECT RAISE(ABORT, 'test delete'); END");
        else if (stage == "history")
            Exec(db, "CREATE TRIGGER reject_rollback BEFORE DELETE ON transactions BEGIN SELECT RAISE(ABORT, 'test history'); END");
        else if (stage == "restore")
            Exec(db, "CREATE TRIGGER reject_rollback BEFORE UPDATE OF is_spent ON utxos BEGIN SELECT RAISE(ABORT, 'test restore'); END");
        else if (stage == "automatic")
            Exec(db, "CREATE TRIGGER reject_rollback BEFORE UPDATE OF is_spent ON utxos BEGIN SELECT RAISE(ROLLBACK, 'test automatic rollback'); END");
        else
            Exec(db, "PRAGMA foreign_keys=ON; CREATE TABLE parent_check(id INTEGER PRIMARY KEY); CREATE TABLE child_check(id INTEGER REFERENCES parent_check(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER reject_rollback AFTER DELETE ON utxos BEGIN INSERT INTO child_check VALUES(1); END");
        dinero::WalletWorker worker(&index, wallet.get());
        std::string error;
        try { dinero::WalletWorkerTestAccess::Disconnect(worker, 20, block); }
        catch (const std::runtime_error& e) { error = e.what(); }
        if (stage == "commit") EXPECT_EQ(error, "Wallet disconnect commit failed: FOREIGN KEY constraint failed");
        else if (stage == "automatic") EXPECT_EQ(error, "Wallet disconnect restore failed: test automatic rollback");
        else EXPECT_EQ(error, "Wallet disconnect " + stage + " failed: test " + stage);
        EXPECT_EQ(sqlite3_get_autocommit(db), 1);
        EXPECT_EQ(Scalar(db, "SELECT count(*) FROM utxos"), 2);
        EXPECT_EQ(Scalar(db, "SELECT is_spent FROM utxos WHERE amount=1000"), 1);
        EXPECT_EQ(Scalar(db, "SELECT count(*) FROM transactions WHERE height=20"), 1);
        EXPECT_EQ(wallet->getBlockchainHeight(), 20);
        // The separate index already rolled back. Retrying must reconcile this
        // prefix, not treat an ordinary-wallet error as completed delivery.
        EXPECT_FALSE(index.GetUTXO(spending.GetTxid(), 0));
        ASSERT_TRUE(index.GetUTXO(seed, 0));
        EXPECT_FALSE(index.GetUTXO(seed, 0)->spend_height);
        Exec(db, "DROP TRIGGER reject_rollback");
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Disconnect(worker, 20, block));
        EXPECT_EQ(Scalar(db, "SELECT count(*) FROM utxos"), 1);
        EXPECT_EQ(Scalar(db, "SELECT is_spent FROM utxos WHERE amount=1000"), 0);
        EXPECT_EQ(Scalar(db, "SELECT count(*) FROM transactions WHERE height=20"), 0);
        EXPECT_EQ(wallet->getBlockchainHeight(), 19);
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Disconnect(worker, 20, block));
    }
    wallet.reset(); wallet = std::make_unique<dinero::WalletManager>(path); wallet->open("rollback");
    EXPECT_EQ(Scalar(wallet->getCurrentDatabase(), "SELECT count(*) FROM utxos"), 1);
    EXPECT_EQ(Scalar(wallet->getCurrentDatabase(), "SELECT is_spent FROM utxos WHERE amount=1000"), 0);
    EXPECT_EQ(wallet->getBlockchainHeight(), 19);
}

TEST_F(WalletBlockDisconnectTest, BorrowedTransactionAndInvalidHeightLeaveBothStoresUntouched) {
    sqlite3* db = wallet->getCurrentDatabase();
    dinero::UTXOIndex index((path / "bounds-index.sqlite").string());
    ASSERT_TRUE(index.Initialize());
    dinero::uint256 hash; hash.begin()[0] = 9;
    const dinero::TxId id(hash);
    ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(id, 0, dinero::AmountUna::Una(1), {0x51}, "m/84'/1448'/0'/0/0", 0)));
    dinero::WalletWorker worker(&index, wallet.get());
    dinero::Block block;
    for (uint32_t height : {0u, std::numeric_limits<uint32_t>::max()}) {
        EXPECT_THROW(wallet->onBlockDisconnected(block, height), std::runtime_error);
        EXPECT_THROW(dinero::WalletWorkerTestAccess::Disconnect(worker, height, block), std::runtime_error);
        EXPECT_TRUE(index.GetUTXO(id, 0));
    }
    Exec(db, "CREATE TABLE caller_probe(value INTEGER); BEGIN IMMEDIATE; INSERT INTO caller_probe VALUES(1)");
    EXPECT_THROW(wallet->onBlockDisconnected(block, 1), std::runtime_error);
    EXPECT_THROW(dinero::WalletWorkerTestAccess::Disconnect(worker, 1, block), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Scalar(db, "SELECT count(*) FROM caller_probe"), 1);
    EXPECT_TRUE(index.GetUTXO(id, 0));
    Exec(db, "ROLLBACK");
    EXPECT_EQ(Scalar(db, "SELECT count(*) FROM caller_probe"), 0);
    ASSERT_TRUE(index.SpendUTXO(id, 0, 1));
    auto outer = wallet->AcquireDatabaseLease();
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO caller_probe VALUES(2)");
    // Nested leases preserve the outer transaction; the worker must separately
    // reject it before committing any rollback in the other database.
    EXPECT_THROW(wallet->onBlockDisconnected(block, 1), std::runtime_error);
    EXPECT_THROW(dinero::WalletWorkerTestAccess::Disconnect(worker, 1, block), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Scalar(db, "SELECT count(*) FROM caller_probe"), 1);
    EXPECT_EQ(index.GetUTXO(id, 0)->spend_height, 1);
    Exec(db, "ROLLBACK");
}
} // namespace
