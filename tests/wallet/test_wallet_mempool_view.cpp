// Task 5.3: mempool observations through IWalletMempoolView
// (ctest WalletMempoolView). Exit-nonzero checks only — no assert().

#include "daemon/mempool_wallet_view.h"
#include "wallet/node_interfaces.h"
#include "wallet/rbf_cpfp_detector.h"
#include "wallet/wallet_manager.h"

#include "consensus/outpoint.h"
#include "primitives/uint256.h"

#include "tests/wallet/fake_node_views.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void TestListUnspentWithAndWithoutView() {
    using namespace dinero;
    const auto dir = fs::temp_directory_path() /
        ("mempool_view_listunspent_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        WalletManager wallet(dir);
        wallet.create("poolview");
        wallet.open("poolview");
        const std::string txid_a(64, 'a');
        const std::string txid_b(64, 'b');
        check(wallet.addUTXO(txid_a, 0, 5000, "addr_a", "5120", 1, false),
              "fixture UTXO A added");
        check(wallet.addUTXO(txid_b, 1, 7000, "addr_b", "5120", 1, false),
              "fixture UTXO B added");

        // Null view: no mempool filtering, both coins listed.
        const auto all = wallet.listUnspentUTXOs(0, 9999999, nullptr);
        check(all.size() == 2, "null mempool view returns both fixture UTXOs");

        // A view marking coin A spent filters it.
        wallet::testing::FakeMempoolView pool;
        pool.spent[OutPoint(TxId(uint256::FromHexUnsafe(txid_a)), 0)] = true;
        const auto filtered = wallet.listUnspentUTXOs(0, 9999999, &pool);
        check(filtered.size() == 1, "mempool-spent coin is filtered out");
        check(filtered.size() == 1 && filtered[0].txid == txid_b,
              "the remaining coin is the unspent one");
        check(pool.is_output_spent_calls >= 2, "the view was actually consulted");
    }
    fs::remove_all(dir);
}

void TestRbfDetectorOrchardEntryThrowsSameError() {
    using namespace dinero;
    wallet::testing::FakeMempoolView pool;
    uint256 txid;
    txid.data[0] = 0x33;
    wallet::WalletMempoolEntry orchard_shaped;  // no historical vin set
    orchard_shaped.fee = 1000;
    orchard_shaped.tx_size = 200;
    pool.entries[txid] = orchard_shaped;

    RbfCpfpDetector detector;
    bool threw = false;
    try {
        (void)detector.CheckRbfCapability(txid, &pool);
    } catch (const std::logic_error& e) {
        threw = true;
        check(std::string(e.what()) == "Orchard transaction has no historical body",
              "detector surfaces the exact Orchard-body message");
    } catch (...) {
        threw = true;
        check(false, "detector must throw std::logic_error for an Orchard entry");
    }
    check(threw, "detector throws for an Orchard-shaped entry without vin");
}

} // namespace

int main() {
    TestListUnspentWithAndWithoutView();
    TestRbfDetectorOrchardEntryThrowsSameError();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("WalletMempoolView: all checks passed");
    return 0;
}
