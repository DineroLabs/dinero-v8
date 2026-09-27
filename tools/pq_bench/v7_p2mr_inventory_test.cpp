#include "rpc/v7_pq_handlers.h"
#include "wallet/v7_p2mr_store.h"

#include <sqlite3.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace rpc = dinero::rpc::v7;
namespace wallet = dinero::wallet;
namespace {
void Require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
struct Temporary {
    std::string dir, path;
    Temporary() {
        char name[] = "/tmp/dinero_p2mr_inventory_XXXXXX";
        const auto* made = mkdtemp(name);
        Require(made, "temporary directory");
        dir = made;
        path = dir + "/store.sqlite";
    }
    ~Temporary() { std::filesystem::remove_all(dir); }
};
struct Raw {
    sqlite3* db = nullptr;
    explicit Raw(const std::string& path) {
        Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "test database open");
    }
    ~Raw() { sqlite3_close(db); }
    void Exec(const std::string& sql) {
        Require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                "test SQL setup");
    }
};
std::string Bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
rpc::GetNewP2MRAddressResult Add(wallet::V7P2MRStore& store, int64_t owner, uint32_t index) {
    rpc::GetNewP2MRAddressParams p{};
    p.wallet_id = owner;
    p.account = 7;
    p.address_index = index;
    p.hrp = "rdin";
    p.bip32_priv.fill(uint8_t(index + 1));
    p.bip32_chain.fill(42);
    p.master_key.fill(11);
    p.label = "preserve metadata";
    p.now_unix = 1700000000 + index;
    auto result = rpc::GetNewP2MRAddress(store, p);
    Require(result.status == rpc::HandlerStatus::Ok, "real P2MR handler insertion");
    return result;
}
void Refused(const wallet::V7P2MRStore& store) {
    bool threw = false;
    try { (void)store.ListByWallet(7); }
    catch (const std::runtime_error&) { threw = true; }
    Require(threw, "incomplete inventory must throw");
    const auto result = rpc::ListP2MRAddresses(store, {7});
    Require(result.status == rpc::HandlerStatus::StoreError && result.entries.empty(),
            "actual handler must return error without partial entries");
}
void ExistingReadOnly() {
    Temporary tmp;
    wallet::V7P2MRStore read;
    Require(read.OpenExistingReadOnly(tmp.path) == wallet::V7P2MRStore::OpenResult::IoError,
            "missing store must refuse");
    Require(!std::filesystem::exists(tmp.path), "read must not create missing file");
    Refused(read);
    {
        Raw raw(tmp.path);
        raw.Exec("CREATE TABLE unrelated(value TEXT); INSERT INTO unrelated VALUES('preserved')");
    }
    const auto original = Bytes(tmp.path);
    Require(read.OpenExistingReadOnly(tmp.path) == wallet::V7P2MRStore::OpenResult::SchemaError,
            "missing schema must refuse");
    Require(Bytes(tmp.path) == original, "read must not migrate existing database");
    wallet::V7P2MRStore write;
    Require(write.Open(tmp.path) == wallet::V7P2MRStore::OpenResult::Ok, "explicit store creation");
    const auto first = Add(write, 7, 0);
    Add(write, 7, 1);
    Add(write, 19, 2);
    write.Close();
    const auto populated = Bytes(tmp.path);
    Require(read.OpenExistingReadOnly(tmp.path) == wallet::V7P2MRStore::OpenResult::Ok,
            "existing readonly store");
    const auto rows = read.ListByWallet(7);
    Require(rows.size() == 2 && rows[0].address == first.address &&
            rows[0].label == "preserve metadata" && rows[0].wallet_id == 7,
            "ordered scoped inventory preserves exact metadata");
    Require(read.ListByWallet(19).size() == 1 && read.ListByWallet(29).empty(),
            "nonconsecutive owners and genuinely empty scope");
    rpc::GetNewP2MRAddressParams p{};
    p.wallet_id = 7; p.address_index = 99; p.bip32_priv.fill(9);
    p.bip32_chain.fill(8); p.master_key.fill(11); p.hrp = "rdin";
    Require(rpc::GetNewP2MRAddress(read, p).status == rpc::HandlerStatus::StoreError,
            "readonly connection refuses writes");
    read.Close();
    Require(Bytes(tmp.path) == populated, "readonly inventory preserves all durable bytes");
}
void MalformedLateRows() {
    Temporary tmp;
    wallet::V7P2MRStore store;
    Require(store.Open(tmp.path) == wallet::V7P2MRStore::OpenResult::Ok, "create fixture");
    Add(store, 7, 0); Add(store, 7, 1);
    Raw raw(tmp.path);
    // The second row follows a complete valid row. Every refusal must discard
    // that earlier result, including through the actual RPC component handler.
    for (const char* assignment : {
        "merkle_root=zeroblob(31)", "pubkey=zeroblob(1)",
        "seed_ciphertext=zeroblob(31)", "seed_nonce=zeroblob(11)", "seed_tag=zeroblob(15)",
        "merkle_root=CAST(zeroblob(32) AS TEXT)", "address=x'6162'",
        "derivation_path=''", "leaf_index=-1", "leaf_index=4294967296",
        "leaf_index=0.5", "created_at='invalid'"}) {
        raw.Exec("BEGIN");
        raw.Exec(std::string("UPDATE v7_p2mr_addresses SET ") + assignment + " WHERE id=2");
        raw.Exec("COMMIT");
        Refused(store);
        // Rebuild only the isolated fixture with the actual writer for the next case.
        raw.Exec("DELETE FROM v7_p2mr_addresses WHERE id=2");
        Add(store, 7, 1);
        raw.Exec("UPDATE v7_p2mr_addresses SET id=2 WHERE id>2");
    }
    raw.Exec("UPDATE v7_p2mr_addresses SET leaf_index=4294967295,label=NULL WHERE id=2");
    auto rows = store.ListByWallet(7);
    Require(rows.size() == 2 && rows[1].leaf_index == UINT32_MAX && rows[1].label.empty(),
            "full uint32 leaf and nullable label preserved without narrowing");
}
bool interrupt_inventory = false;
int Trace(unsigned event, void* db, void* statement, void*) {
    if (event == SQLITE_TRACE_ROW && interrupt_inventory) {
        const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (sql && std::string(sql).rfind("SELECT id, wallet_id", 0) == 0)
            sqlite3_interrupt(static_cast<sqlite3*>(db));
    }
    return 0;
}
int InstallTrace(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(db, SQLITE_TRACE_ROW, Trace, db);
}
void IncompleteRead() {
    Temporary tmp;
    wallet::V7P2MRStore write;
    Require(write.Open(tmp.path) == wallet::V7P2MRStore::OpenResult::Ok, "create interrupted fixture");
    Add(write, 7, 0); Add(write, 7, 1); write.Close();
    Require(sqlite3_auto_extension(reinterpret_cast<void(*)()>(InstallTrace)) == SQLITE_OK,
            "install isolated SQL interrupt fixture");
    wallet::V7P2MRStore read;
    const auto opened = read.OpenExistingReadOnly(tmp.path);
    sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(InstallTrace));
    Require(opened == wallet::V7P2MRStore::OpenResult::Ok, "open traced reader");
    interrupt_inventory = true;
    Refused(read);
    interrupt_inventory = false;
    Require(read.ListByWallet(7).size() == 2, "same readonly owner retries after interrupted query");
    read.Close(); Refused(read);
}
}
int main() {
    try {
        ExistingReadOnly(); std::puts("[PASS] P2MRInventory.ExistingReadOnly");
        MalformedLateRows(); std::puts("[PASS] P2MRInventory.MalformedLateRows");
        IncompleteRead(); std::puts("[PASS] P2MRInventory.IncompleteRead");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] P2MR inventory: %s\n", e.what()); return 1;
    }
}
