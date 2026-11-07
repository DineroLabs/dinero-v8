// Task 5.2: rescan through IWalletChainView (ctest WalletChainView).
// Exit-nonzero checks only — no assert().

#include "storage/chain_db_wallet_view.h"
#include "wallet/node_interfaces.h"
#include "wallet/wallet_manager.h"

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/chain_db.h"
#include "din_json.h"
#include "storage/chain_write_token.h"

#include "tests/wallet/fake_node_views.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::vector<uint8_t> p2tr(uint8_t seed) {
    std::vector<uint8_t> spk;
    spk.push_back(0x51);
    spk.push_back(0x20);
    for (int i = 0; i < 32; ++i) spk.push_back(static_cast<uint8_t>(seed + i));
    return spk;
}

void TestNoBlockStorageReadsChainDbOnly() {
    using namespace dinero;
    const auto dir = fs::temp_directory_path() /
        ("chain_view_db_only_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    ChainDB chain;
    check(chain.init(dir / "chain") == Status::Ok, "chain view fixture opens chain store");
    const auto token = ChainWriteToken::CreateForTesting();

    Transaction receive;
    receive.version = 2;
    TxInput coinbase;
    coinbase.prevout.vout = UINT32_MAX;
    coinbase.scriptSig = {1, 1};
    receive.vin.push_back(coinbase);
    TxOutput owned;
    owned.value = AmountUna::Una(5000);
    owned.scriptPubKey = p2tr(0x61);
    receive.vout.push_back(owned);
    Block first;
    first.header.timestamp = 1;
    first.header.difficulty = 0x1d31ffce;  // DIFFICULTY_1 reference bits
    first.vtx.push_back(receive);
    first.header.merkle_root = receive.GetTxid().AsUint256();

    check(chain.putBlock(token, first.GetHash(), first) == Status::Ok,
          "chain view fixture stores block body");
    check(chain.putHeightIndex(token, 1, first.GetHash()) == Status::Ok,
          "chain view fixture stores height index");
    check(chain.setTip(token, first.GetHash(), 1, arith_uint256(1)) == Status::Ok,
          "chain view fixture sets tip");

    // Review fix (stage-5 I2): the earlier "counting double" could never
    // fire, because it was never passed anywhere; the honest statement of
    // the invariant is the construction itself — a null BlockStorage means
    // ReadBlock can only be satisfied by ChainDB's stored body, through
    // ReadArchivalBlock's AllowLegacyFallback default (exercised here: the
    // body exists ONLY via chain.putBlock, no flatfile storage exists).
    storage::ChainDbWalletView view(chain, /*block_storage=*/nullptr);

    const auto tip = view.GetTip();
    check(tip.has_value() && tip->height == 1 && tip->hash == first.GetHash(),
          "view tip comes from ChainDB");
    const auto hash = view.GetBlockHashAtHeight(1);
    check(hash.has_value() && *hash == first.GetHash(), "view hash-at-height comes from ChainDB");
    const auto block = view.ReadBlock(first.GetHash());
    check(block.has_value() && block->GetHash() == first.GetHash(),
          "ReadBlock with null BlockStorage reads the ChainDB-stored body");
    check(!view.ReadBlock(uint256{}).has_value(), "unknown hash is nullopt");

    // ChainHeightProvider half (stage-5 review I3: first live provider needs
    // coverage). Add a second block with DIFFERENT compact bits and move the
    // tip there: GetBlockHeader must report each block's OWN difficulty.
    Block second;
    second.header.timestamp = 2;
    second.header.prev_block_hash = first.GetHash();
    second.header.difficulty = 0x1c31ffce;  // smaller target: measurably harder
    Transaction filler = receive;
    filler.vin[0].scriptSig = {9, 9};
    second.vtx.push_back(filler);
    second.header.merkle_root = filler.GetTxid().AsUint256();
    check(chain.putBlock(token, second.GetHash(), second) == Status::Ok &&
          chain.putHeightIndex(token, 2, second.GetHash()) == Status::Ok &&
          chain.putHeader(token, first.GetHash(), first.header, 1, arith_uint256(1)) == Status::Ok &&
          chain.putHeader(token, second.GetHash(), second.header, 2, arith_uint256(2)) == Status::Ok &&
          chain.setTip(token, second.GetHash(), 2, arith_uint256(2)) == Status::Ok,
          "second fixture block + both headers stored, tip moved");
    check(view.GetBestHeight() == 2, "provider best height tracks the tip");
    check(view.GetBestHash() == second.GetHash().GetHex(), "provider best hash tracks the tip");
    check(view.IsAvailable(), "provider is available over a live ChainDB");
    const auto hdr1 = view.GetBlockHeader(first.GetHash().GetHex());
    const auto hdr2 = view.GetBlockHeader(second.GetHash().GetHex());
    check(hdr1["height"].asInt() == 1 && hdr2["height"].asInt() == 2,
          "provider headers carry their own heights");
    check(hdr1["bits"].asUInt() != hdr2["bits"].asUInt(),
          "fixture blocks carry different compact bits");
    check(hdr1["difficulty"].asDouble() != hdr2["difficulty"].asDouble(),
          "each header reports its OWN difficulty (tip-difficulty bug stays dead)");
    check(view.GetBlockHeader(std::string(64, 'f')).isNull() ||
          view.GetBlockHeader(std::string(64, 'f')).empty(),
          "unknown header hash yields an empty object");

    fs::remove_all(dir);
}

void TestRescanNullViewGuard() {
    using namespace dinero;
    const auto dir = fs::temp_directory_path() /
        ("chain_view_null_guard_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        WalletManager wallet(dir);
        wallet.create("nullguard");
        wallet.open("nullguard");
        // Null view and no prepared history: the guard must refuse, not crash.
        check(!wallet.rescanBlockchain(0, 20, nullptr),
              "rescan with a null view and no prepared history returns false");
        // The prepared-history positive path is driven by ChainstateService
        // (RescanBlockchainImpl friend call) and covered by the snapshot
        // recovery suites; it is not constructible from here.
    }
    fs::remove_all(dir);
}

void TestRescanWithFakeViewFindsFundedAddress() {
    using namespace dinero;
    const auto dir = fs::temp_directory_path() /
        ("chain_view_fake_rescan_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        WalletManager wallet(dir);
        wallet.create("fakeview");
        wallet.open("fakeview");
        const auto script = p2tr(0x61);
        wallet.addWatchScript(script, "m/86'/0'/0'/0/0", false);

        // Three blocks; the first funds the watched script.
        Transaction receive;
        receive.version = 2;
        TxInput coinbase;
        coinbase.prevout.vout = UINT32_MAX;
        coinbase.scriptSig = {1, 1};
        receive.vin.push_back(coinbase);
        TxOutput owned;
        owned.value = AmountUna::Una(5000);
        owned.scriptPubKey = script;
        receive.vout.push_back(owned);
        Block b1;
        b1.header.timestamp = 1;
        b1.vtx.push_back(receive);
        b1.header.merkle_root = receive.GetTxid().AsUint256();

        Transaction filler1 = receive;
        filler1.vin[0].scriptSig = {2, 2};
        filler1.vout[0].scriptPubKey = p2tr(0x71);
        Block b2;
        b2.header.timestamp = 2;
        b2.header.prev_block_hash = b1.GetHash();
        b2.vtx.push_back(filler1);
        b2.header.merkle_root = filler1.GetTxid().AsUint256();

        Transaction filler2 = receive;
        filler2.vin[0].scriptSig = {3, 3};
        filler2.vout[0].scriptPubKey = p2tr(0x81);
        Block b3;
        b3.header.timestamp = 3;
        b3.header.prev_block_hash = b2.GetHash();
        b3.vtx.push_back(filler2);
        b3.header.merkle_root = filler2.GetTxid().AsUint256();

        wallet::testing::FakeChainView fake;
        int32_t h = 1;
        for (const auto* b : {&b1, &b2, &b3}) {
            fake.blocks_by_height[h] = b->GetHash();
            fake.blocks_by_hash[b->GetHash()] = *b;
            ++h;
        }

        check(wallet.rescanBlockchain(1, 0, &fake),
              "rescan through a FakeChainView completes");
        check(fake.read_block_calls >= 3, "rescan read every fixture block through the view");

        sqlite3_stmt* raw = nullptr;
        check(sqlite3_prepare_v2(wallet.getCurrentDatabase(),
                                 "SELECT COUNT(*) FROM utxos WHERE is_spent=0", -1,
                                 &raw, nullptr) == SQLITE_OK,
              "utxo count query prepares");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(raw, sqlite3_finalize);
        check(sqlite3_step(raw) == SQLITE_ROW && sqlite3_column_int64(raw, 0) == 1,
              "rescan registered exactly the funded UTXO");
    }
    fs::remove_all(dir);
}

} // namespace

int main() {
    TestNoBlockStorageReadsChainDbOnly();
    TestRescanNullViewGuard();
    TestRescanWithFakeViewFindsFundedAddress();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("WalletChainView: all checks passed");
    return 0;
}
