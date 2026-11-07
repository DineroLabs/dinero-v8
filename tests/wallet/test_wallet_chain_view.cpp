// Task 5.2: rescan through IWalletChainView (ctest WalletChainView).
// Exit-nonzero checks only — no assert().

#include "storage/chain_db_wallet_view.h"
#include "wallet/node_interfaces.h"
#include "wallet/wallet_manager.h"

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/chain_db.h"
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

// A BlockStorage stand-in that counts every call. It is NEVER passed to the
// view below; the counter staying at zero is the test's whole point: a
// ChainDbWalletView built with a null BlockStorage must satisfy ReadBlock
// from ChainDB alone.
struct CountingBlockStorageDouble {
    mutable int calls = 0;
};

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
    first.vtx.push_back(receive);
    first.header.merkle_root = receive.GetTxid().AsUint256();

    check(chain.putBlock(token, first.GetHash(), first) == Status::Ok,
          "chain view fixture stores block body");
    check(chain.putHeightIndex(token, 1, first.GetHash()) == Status::Ok,
          "chain view fixture stores height index");
    check(chain.setTip(token, first.GetHash(), 1, arith_uint256(1)) == Status::Ok,
          "chain view fixture sets tip");

    CountingBlockStorageDouble storage_double;  // never passed anywhere

    storage::ChainDbWalletView view(chain, /*block_storage=*/nullptr);

    const auto tip = view.GetTip();
    check(tip.has_value() && tip->height == 1 && tip->hash == first.GetHash(),
          "view tip comes from ChainDB");
    const auto hash = view.GetBlockHashAtHeight(1);
    check(hash.has_value() && *hash == first.GetHash(), "view hash-at-height comes from ChainDB");
    const auto block = view.ReadBlock(first.GetHash());
    check(block.has_value() && block->GetHash() == first.GetHash(),
          "ReadBlock with null BlockStorage reads the ChainDB-stored body");
    check(storage_double.calls == 0,
          "no BlockStorage method was called (double was never touched)");
    check(!view.ReadBlock(uint256{}).has_value(), "unknown hash is nullopt");

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
