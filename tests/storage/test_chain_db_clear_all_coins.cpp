// Regression test for ChainDB::clearAllCoins — the utxo-CF reset used by the
// incomplete-reorg clean re-bootstrap (ChainstateService Init).
//
// Coins are written to the RocksDB utxo CF by forward-connect (putCoin). When a
// crash mid-reorg leaves the forest checkpoint out of sync with the ChainDB
// tip, the recovery wipes the forest AND clears this CF — because orphan coin
// rows from the crashed divergent tip would otherwise persist as phantom
// balances (getCoin/forEachUTXO). The snapshot's base coins are NOT in this CF
// (BulkLoad is in-memory only), so clearing it whole is correct: it returns the
// CF to the empty fresh-bootstrap state that forward-connect then rebuilds.
//
// This covers assertion (a) of the pair: the orphan is removed, and the delete
// is neither a no-op nor selective. Assertion (b) — that recovery RECONVERGES
// (a legitimate coin is present + correct after re-arm + forward-connect, i.e.
// the fix rebuilt rather than merely emptied) — is covered end-to-end by the
// snapshot-node recovery integration test, which also asserts the "Cleared N
// utxo CF coin rows" log line so Init is proven to invoke this method.
//
// checks exit non-zero rather than using the C assert macro, which compiles to
// a no-op under NDEBUG and would not gate a release build.

#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Coin;
using dinero::Status;
using dinero::uint256;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " << msg << " (" << #cond << ") at line "    \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

std::filesystem::path MakeTempDir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("dinero_clearcoins_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

uint256 TxidFromByte(uint8_t b) {
    uint256 h;
    std::memset(h.data, 0, sizeof(h.data));
    h.data[0] = b;
    return h;
}

Coin MakeCoin(uint64_t amount, int height) {
    Coin c;
    c.amount = amount;
    c.script_pubkey = std::string(1, '\x51');  // OP_TRUE
    c.height = height;
    return c;
}

}  // namespace

int main() {
    auto dir = MakeTempDir();
    {
        ChainDB db;
        CHECK(db.init(dir) == Status::Ok, "ChainDB::init on temp dir");
        const ChainWriteToken token = ChainWriteToken::CreateForTesting();

        // A base-height coin, a mid coin, and a distinctive ORPHAN sentinel:
        // a real coin (height > snapshot base, real value/script) at an outpoint
        // a re-synced chain would NOT re-create — the phantom-balance shape.
        const uint256 base_txid = TxidFromByte(0x01);
        const uint256 mid_txid = TxidFromByte(0x02);
        const uint256 orphan_txid = TxidFromByte(0xAB);
        CHECK(db.putCoin(token, base_txid, 0, MakeCoin(100, 84131)) == Status::Ok, "putCoin base");
        CHECK(db.putCoin(token, mid_txid, 1, MakeCoin(200, 90000)) == Status::Ok, "putCoin mid");
        CHECK(db.putCoin(token, orphan_txid, 0, MakeCoin(999, 90390)) == Status::Ok, "putCoin orphan");

        // Precondition: coins are present, so the test is not vacuously passing
        // on an empty CF (an over-clear/no-rebuild bug must have something to
        // fail against).
        CHECK(db.getCoin(base_txid, 0).ok(), "base coin present pre-clear");
        CHECK(db.getCoin(mid_txid, 1).ok(), "mid coin present pre-clear");
        CHECK(db.getCoin(orphan_txid, 0).ok(), "orphan coin present pre-clear");

        auto cleared = db.clearAllCoins(token);
        CHECK(cleared.ok(), "clearAllCoins returned ok");
        CHECK(cleared.value() == 3, "clearAllCoins deleted exactly the 3 coins written");

        // Load-bearing: EVERY coin gone, orphan included — no phantom survives.
        CHECK(!db.getCoin(base_txid, 0).ok(), "base coin removed by clearAllCoins");
        CHECK(!db.getCoin(mid_txid, 1).ok(), "mid coin removed by clearAllCoins");
        CHECK(!db.getCoin(orphan_txid, 0).ok(), "orphan coin removed (no phantom balance)");

        // Idempotent: clearing an already-empty CF is a clean 0-delete no-op,
        // which is what makes a re-kill mid-recovery safe to re-attempt.
        auto again = db.clearAllCoins(token);
        CHECK(again.ok(), "clearAllCoins idempotent call returned ok");
        CHECK(again.value() == 0, "clearAllCoins on empty CF deletes nothing");
    }
    std::filesystem::remove_all(dir);
    std::cout << "test_chain_db_clear_all_coins: PASS" << std::endl;
    return 0;
}
