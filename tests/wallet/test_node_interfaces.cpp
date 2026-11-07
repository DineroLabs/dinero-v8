// Task 5.1: wallet node-facing interfaces (ctest WalletNodeInterfaces).
// Exit-nonzero checks only — no assert() (NDEBUG builds must still gate).

#include "wallet/node_interfaces.h"

#include "tests/wallet/fake_node_views.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using dinero::wallet::WalletMempoolEntry;
using dinero::wallet::testing::FakeChainView;
using dinero::wallet::testing::FakeMempoolView;

// Step 1 of the plan: WalletMempoolEntryOrchardVinThrowsSameError.
void TestOrchardVinThrowsSameError() {
    WalletMempoolEntry entry;
    entry.fee = 1000;
    entry.tx_size = 250;
    // No historical vin set: an Orchard-shaped entry.
    bool threw = false;
    try {
        (void)entry.vin();
    } catch (const std::logic_error& e) {
        threw = true;
        Check(std::string(e.what()) == "Orchard transaction has no historical body",
              "vin() logic_error message must match the wallet_manager text exactly");
    } catch (...) {
        Check(false, "vin() must throw std::logic_error, not another type");
        threw = true;
    }
    Check(threw, "vin() on an entry without historical body must throw");

    // A historical entry returns its inputs.
    WalletMempoolEntry historical;
    dinero::TxInput in;
    in.sequence = 7;
    historical.SetHistoricalVin({in});
    try {
        const auto& vin = historical.vin();
        Check(vin.size() == 1 && vin[0].sequence == 7,
              "historical vin() must return the stored inputs");
    } catch (...) {
        Check(false, "historical vin() must not throw");
    }
}

void TestFakeViewsCountAndBehave() {
    FakeChainView chain;
    Check(!chain.GetTip().has_value(), "empty FakeChainView has no tip");
    dinero::uint256 h;
    h.data[0] = 0x11;
    chain.blocks_by_height[3] = h;
    auto tip = chain.GetTip();
    Check(tip.has_value() && tip->height == 3 && tip->hash == h,
          "FakeChainView tip reflects the highest fixture block");
    Check(chain.GetBlockHashAtHeight(3) == h, "hash-at-height lookup");
    Check(!chain.GetBlockHashAtHeight(9).has_value(), "missing height is nullopt");
    Check(!chain.ReadBlock(h).has_value(), "no block body stored -> nullopt");
    Check(chain.get_tip_calls == 2 && chain.get_hash_calls == 2 &&
          chain.read_block_calls == 1, "FakeChainView counts calls per method");
    Check(chain.reads_block_storage == false, "FakeChainView never reads block storage");

    FakeMempoolView pool;
    dinero::OutPoint op;
    op.vout = 1;
    Check(!pool.IsOutputSpent(op), "unmarked output is unspent");
    pool.spent[op] = true;
    Check(pool.IsOutputSpent(op), "marked output is spent");
    dinero::uint256 txid;
    txid.data[0] = 0x22;
    Check(!pool.GetEntry(txid).has_value(), "unknown txid is nullopt");
    WalletMempoolEntry e;
    e.fee = 5;
    e.tx_size = 100;
    pool.entries[txid] = e;
    auto got = pool.GetEntry(txid);
    Check(got.has_value() && got->fee == 5 && got->tx_size == 100,
          "GetEntry returns the fixture entry");
    pool.stats.tx_count = 4;
    Check(pool.GetStats().tx_count == 4, "GetStats returns fixture stats");
    Check(pool.is_output_spent_calls == 2 && pool.get_entry_calls == 2 &&
          pool.get_stats_calls == 1, "FakeMempoolView counts calls per method");
}

} // namespace

int main() {
    TestOrchardVinThrowsSameError();
    TestFakeViewsCountAndBehave();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("WalletNodeInterfaces: all checks passed");
    return 0;
}
