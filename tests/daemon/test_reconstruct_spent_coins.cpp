/**
 * #274 regression: stateless (CSN/utreexo) ConnectBlock cannot populate
 * BlockUndo.spent_coins (no UTXO set), so ConnectTip must reconstruct the
 * spent list from ChainDB coin rows before the unified batch stages the
 * deleteCoin calls. ChainDB rows are the only source carrying full fidelity
 * (height + coinbase flag, which utreexo proofs do not commit to).
 *
 * Pins ChainstateService::ReconstructSpentCoinsFromChainDb:
 *   1. ChainDB-backed reconstruction preserves every SpentCoin field.
 *   2. Same-block (intra-block) spends fall back to the block body.
 *   3. A miss from both sources is fatal: NotFound, empty out_spent,
 *      error names the offending outpoint.
 *
 * Checks are exit-nonzero (require/abort), NOT assert() — this test must
 * still gate under NDEBUG (Release/CI).
 */

#include "daemon/services/chainstate_service.h"
#include "primitives/amount.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dinero;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::abort();
    }
}

std::filesystem::path MakeTempRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("din_reconstruct_spent_coins_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

Transaction MakeCoinbaseTx(uint32_t n) {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, static_cast<uint8_t>(n & 0xff), 0x00, 0x00};
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, static_cast<uint8_t>(0x10 + n));
    tx.vout.push_back(out);

    tx.DetectWitnessVersion();
    return tx;
}

// Non-coinbase tx spending a single outpoint, producing a single output.
Transaction MakeSpendTx(const uint256& prev_txid, uint32_t prev_vout,
                        uint64_t out_value,
                        const std::vector<uint8_t>& out_spk) {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(prev_txid);
    in.prevout.vout = prev_vout;
    in.sequence = 0xfffffffe;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(out_value);
    out.scriptPubKey = out_spk;
    tx.vout.push_back(out);

    return tx;
}

Block MakeBlock(const std::vector<Transaction>& txs) {
    Block block;
    block.header.version = 3;
    block.vtx = txs;
    return block;
}

// ─────────────────────────────────────────────────────────────────
// #1 — ChainDB coin row reconstructs every SpentCoin field
// ─────────────────────────────────────────────────────────────────
void test01_ReconstructSpentCoinsReadsChainDbRows(const std::filesystem::path& root) {
    ChainDB db;
    require(db.init(root / "t01") == Status::Ok, "t01: ChainDB::init failed");
    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    const uint256 prev_txid = uint256::FromHexUnsafe(
        "11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa");
    const uint32_t prev_vout = 3;

    // Coin row with every field exercised, including the confidential pair.
    Coin coin;
    coin.amount = 12345;
    coin.script_pubkey = "deadbeefcafe";  // ChainDB stores spk as HEX STRING
    coin.height = 42;
    coin.coinbase = true;
    coin.is_confidential = true;
    coin.commitment = std::vector<uint8_t>(33, 0xab);
    require(db.putCoin(token, prev_txid, prev_vout, coin) == Status::Ok,
            "t01: putCoin failed");

    const Block block = MakeBlock({
        MakeCoinbaseTx(1),
        MakeSpendTx(prev_txid, prev_vout, 999, {0x51}),
    });

    ChainstateService svc;
    svc.setChainDB(&db);

    std::vector<dinero::SpentCoin> spent;
    std::string error;
    const Status st = svc.ReconstructSpentCoinsFromChainDb(block, 100, spent, error);
    require(st == Status::Ok, "t01: expected Ok, got " +
            std::string(StatusToString(st)) + " error=" + error);
    require(error.empty(), "t01: error must be empty on success: " + error);
    require(spent.size() == 1, "t01: expected exactly 1 SpentCoin, got " +
            std::to_string(spent.size()));

    const auto& sc = spent[0];
    require(sc.prev_txid == prev_txid, "t01: prev_txid mismatch");
    require(sc.prev_vout == prev_vout, "t01: prev_vout mismatch");
    require(sc.value == 12345, "t01: value mismatch: " + std::to_string(sc.value));
    const std::vector<uint8_t> expected_spk = {0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe};
    require(sc.scriptPubKey == expected_spk,
            "t01: scriptPubKey not byte-equal after hex decode");
    require(sc.is_coinbase == true, "t01: is_coinbase mismatch");
    require(sc.height == 42, "t01: height mismatch (must be coin row height, "
            "not block height): " + std::to_string(sc.height));
    require(sc.is_confidential == true, "t01: is_confidential mismatch");
    require(sc.commitment == std::vector<uint8_t>(33, 0xab),
            "t01: commitment mismatch");

    std::cout << "  [✓] ChainDB coin row reconstructed with full fidelity"
              << std::endl;
}

// ─────────────────────────────────────────────────────────────────
// #2 — same-block spend falls back to the block body
// ─────────────────────────────────────────────────────────────────
void test02_ReconstructSpentCoinsIntraBlockFallback(const std::filesystem::path& root) {
    ChainDB db;
    require(db.init(root / "t02") == Status::Ok, "t02: ChainDB::init failed");
    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    // txA's own input references a coin that IS in ChainDB so the whole
    // call succeeds.
    const uint256 funding_txid = uint256::FromHexUnsafe(
        "22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb22bb");
    Coin funding;
    funding.amount = 5000;
    funding.script_pubkey = "0014";
    funding.height = 7;
    funding.coinbase = false;
    require(db.putCoin(token, funding_txid, 0, funding) == Status::Ok,
            "t02: putCoin failed");

    // txA spends the funding coin and creates an output NOT in ChainDB.
    const std::vector<uint8_t> txa_out_spk = {0x00, 0x14, 0x77, 0x77, 0x77};
    Transaction txA = MakeSpendTx(funding_txid, 0, 777, txa_out_spk);

    // txB spends txA:0 — created in this very block, never written to ChainDB.
    Transaction txB = MakeSpendTx(txA.GetTxid().AsUint256(), 0, 111, {0x51});

    const uint32_t block_height = 55;
    const Block block = MakeBlock({MakeCoinbaseTx(2), txA, txB});

    ChainstateService svc;
    svc.setChainDB(&db);

    std::vector<dinero::SpentCoin> spent;
    std::string error;
    const Status st =
        svc.ReconstructSpentCoinsFromChainDb(block, block_height, spent, error);
    require(st == Status::Ok, "t02: expected Ok, got " +
            std::string(StatusToString(st)) + " error=" + error);
    require(spent.size() == 2, "t02: expected exactly 2 SpentCoins, got " +
            std::to_string(spent.size()));

    // Entry 0: txA's input — from the ChainDB row.
    require(spent[0].prev_txid == funding_txid, "t02: entry0 prev_txid mismatch");
    require(spent[0].prev_vout == 0, "t02: entry0 prev_vout mismatch");
    require(spent[0].value == 5000, "t02: entry0 value mismatch");
    require(spent[0].height == 7, "t02: entry0 height mismatch");
    require(spent[0].is_coinbase == false, "t02: entry0 is_coinbase mismatch");

    // Entry 1: txB's input — reconstructed from txA's output in this block.
    require(spent[1].prev_txid == txA.GetTxid().AsUint256(),
            "t02: entry1 prev_txid mismatch");
    require(spent[1].prev_vout == 0, "t02: entry1 prev_vout mismatch");
    require(spent[1].value == 777, "t02: entry1 value must come from txA's output");
    require(spent[1].scriptPubKey == txa_out_spk,
            "t02: entry1 scriptPubKey must come from txA's output");
    require(spent[1].is_coinbase == false,
            "t02: entry1 is_coinbase must be false (txA is not the coinbase)");
    require(spent[1].height == block_height,
            "t02: entry1 height must be the block height: " +
            std::to_string(spent[1].height));
    require(spent[1].is_confidential == false,
            "t02: entry1 is_confidential mismatch");
    require(spent[1].commitment.empty(), "t02: entry1 commitment mismatch");

    std::cout << "  [✓] intra-block spend reconstructed from block body"
              << std::endl;
}

// ─────────────────────────────────────────────────────────────────
// #3 — miss from both sources is fatal: NotFound, empty, named outpoint
// ─────────────────────────────────────────────────────────────────
void test03_ReconstructSpentCoinsFailsLoudOnMissingCoin(const std::filesystem::path& root) {
    ChainDB db;
    require(db.init(root / "t03") == Status::Ok, "t03: ChainDB::init failed");
    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    // A known coin spent FIRST, so we also prove out_spent is cleared on
    // failure (a partial spent list would be worse than an empty one).
    const uint256 known_txid = uint256::FromHexUnsafe(
        "33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc33cc");
    Coin known;
    known.amount = 100;
    known.script_pubkey = "51";
    known.height = 9;
    known.coinbase = false;
    require(db.putCoin(token, known_txid, 0, known) == Status::Ok,
            "t03: putCoin failed");

    const uint256 missing_txid = uint256::FromHexUnsafe(
        "44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd44dd");
    const uint32_t missing_vout = 5;

    const Block block = MakeBlock({
        MakeCoinbaseTx(3),
        MakeSpendTx(known_txid, 0, 90, {0x51}),
        MakeSpendTx(missing_txid, missing_vout, 1, {0x51}),
    });

    ChainstateService svc;
    svc.setChainDB(&db);

    std::vector<dinero::SpentCoin> spent;
    std::string error;
    const Status st = svc.ReconstructSpentCoinsFromChainDb(block, 60, spent, error);
    require(st == Status::NotFound, "t03: expected NotFound, got " +
            std::string(StatusToString(st)));
    require(spent.empty(), "t03: out_spent must be cleared on failure, has " +
            std::to_string(spent.size()) + " entries");
    require(!error.empty(), "t03: error must name the outpoint");
    require(error.find(missing_txid.GetHex()) != std::string::npos,
            "t03: error must contain the missing txid hex: " + error);
    require(error.find(std::to_string(missing_vout)) != std::string::npos,
            "t03: error must contain the missing vout: " + error);

    std::cout << "  [✓] missing coin fails loud with named outpoint" << std::endl;
}

}  // namespace

int main() {
    const auto root = MakeTempRoot();

    test01_ReconstructSpentCoinsReadsChainDbRows(root);
    test02_ReconstructSpentCoinsIntraBlockFallback(root);
    test03_ReconstructSpentCoinsFailsLoudOnMissingCoin(root);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::cout << "[PASS] ReconstructSpentCoinsFromChainDb: 3/3 cases" << std::endl;
    return 0;
}
