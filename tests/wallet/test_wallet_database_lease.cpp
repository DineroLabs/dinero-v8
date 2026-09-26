#include "wallet/wallet_manager.h"
#include "wallet/shielded_wallet_ops.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

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
} // namespace
