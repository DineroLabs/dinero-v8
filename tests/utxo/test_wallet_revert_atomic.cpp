#include "wallet/utxo_index.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace dinero;
static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void Sql(sqlite3* db, const char* sql) {
    Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "test SQL failed");
}
static TxId Id(unsigned n) {
    uint256 h; h.begin()[0] = n; return TxId(h);
}
static void CheckOriginal(UTXOIndex& index) {
    const auto old = index.GetUTXO(Id(1), 0), created = index.GetUTXO(Id(2), 0);
    Check(old && old->spend_height == 20, "old spend must remain after failed rollback");
    Check(created && created->height == 20, "created output must remain after failed rollback");
    // GetUTXO reuses a prepared SELECT; exhaust it before external schema writes
    // so the next transaction does not attempt to upgrade an old WAL snapshot.
    Check(!index.GetUTXO(Id(99), 0), "unexpected test output");
}
static void RejectRevert(UTXOIndex& index, const char* expected = "Wallet UTXO rollback statement failed") {
    bool refused = false;
    try { index.RevertBlock(20); } catch (const std::exception& e) {
        Check(std::string(e.what()) == expected, "rollback failed at the wrong stage"); refused = true;
    }
    Check(refused, "failed SQLite rollback must report failure");
    CheckOriginal(index);
}
int main() {
    try {
        auto pattern = (std::filesystem::temp_directory_path() / "wallet-revert-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
        Check(mkdtemp(name.data()) != nullptr, "temporary directory failed");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{name.data()};
        const auto path = (cleanup.path / "index.sqlite").string();
        {
            UTXOIndex index(path); Check(index.Initialize(), "index initialization failed");
            Check(index.AddUTXO(WalletUTXO(Id(1), 0, AmountUna::Una(700), {0x51}, "m/84'/1448'/0'/0/0", 10, false)), "seed old output failed");
            Check(index.SpendUTXO(Id(1), 0, 20), "seed spend failed");
            Check(index.AddUTXO(WalletUTXO(Id(2), 0, AmountUna::Una(600), {0x51}, "m/84'/1448'/0'/0/1", 20, false)), "seed new output failed");
            // A second real connection installs bounded local fault injection.
            sqlite3* raw = nullptr; Check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "observer open failed");
            struct Close { sqlite3* db; ~Close() { sqlite3_close(db); } } close{raw};
            Sql(raw, "CREATE TRIGGER fail_unspend BEFORE UPDATE OF spend_height ON wallet_utxos BEGIN SELECT RAISE(ABORT, 'test update failure'); END;");
            RejectRevert(index); // Delete succeeded first; it must be rolled back.
            Sql(raw, "DROP TRIGGER fail_unspend;");
            Sql(raw, "CREATE TRIGGER fail_delete BEFORE DELETE ON wallet_utxos BEGIN SELECT RAISE(ABORT, 'test delete failure'); END;");
            RejectRevert(index); // A failed delete must not permit an un-spend.
            Sql(raw, "DROP TRIGGER fail_delete;");
            // Both statements succeed, but the deferred constraint rejects COMMIT.
            Sql(raw, "CREATE TABLE parent(id INTEGER PRIMARY KEY); CREATE TABLE child(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER fail_commit AFTER DELETE ON wallet_utxos BEGIN INSERT INTO child VALUES(99); END;");
            RejectRevert(index, "Wallet UTXO rollback transaction failed");
            Sql(raw, "DROP TRIGGER fail_commit;");
            // Refusing nested ownership must neither commit nor roll back the caller.
            Check(index.BeginTransaction(), "outer transaction failed");
            Check(index.SetMetadata("test-outer", "uncommitted"), "outer write failed");
            RejectRevert(index, "Wallet UTXO rollback transaction failed");
            Check(index.GetMetadata("test-outer") == std::optional<std::string>("uncommitted"), "caller transaction changed");
            Check(index.RollbackTransaction(), "caller rollback failed");
            Check(!index.GetMetadata("test-outer"), "nested revert committed caller state");
            index.RevertBlock(20);
            Check(!index.GetUTXO(Id(2), 0), "successful revert must delete created output");
            Check(index.GetUTXO(Id(1), 0) && !index.GetUTXO(Id(1), 0)->spend_height, "successful revert must restore spend");
            index.RevertBlock(20); // Idempotent replay.
        }
        {
            UTXOIndex reopened(path); Check(reopened.Initialize(), "reopen failed");
            Check(!reopened.GetUTXO(Id(2), 0) && reopened.GetUTXO(Id(1), 0) && !reopened.GetUTXO(Id(1), 0)->spend_height, "reopened rollback state differs");
        }
        std::cout << "Wallet UTXO rollback: SQL failure atomicity, caller transaction isolation, replay and reopen passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
