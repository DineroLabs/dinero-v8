/**
 * @file test_wallet_rescan_utxoset.cpp
 * @brief Regression test for WalletManager::rescanUtxoSet (AssumeUTXO/snapshot
 *        bootstrap visibility).
 *
 * BUG: A wallet that fast-syncs from a UTXO snapshot showed ZERO for coins
 * received before the snapshot height. The block-replay rescan can only match
 * outputs from block transaction bodies, which are absent for pre-snapshot
 * heights, so those coins never landed in the wallet's local `utxos` table.
 *
 * FIX: rescanUtxoSet() scans the loaded chainstate UTXO set directly, matching
 * each coin's scriptPubKey against watch_scripts and recording owned coins.
 *
 * This test seeds a watch_script, feeds a UTXO-set producer containing one
 * matching coin and one foreign coin, and asserts the matching coin lands in
 * the wallet (balance + utxo count), is idempotent on re-run, and that the
 * scan-height watermark is advanced to the snapshot height.
 *
 * Uses exit-nonzero checks (NOT bare assert(), which is a no-op under NDEBUG).
 */

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <unistd.h>

#include "wallet/wallet_manager.h"
#include "consensus/chainparams.h"

using dinero::WalletManager;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  ✓ " << what << std::endl;
    } else {
        std::cerr << "  ✗ FAIL: " << what << std::endl;
        ++g_failures;
    }
}

// Build a P2TR scriptPubKey: OP_1 (0x51) OP_PUSHBYTES_32 (0x20) <32-byte program>
static std::vector<uint8_t> p2tr(uint8_t fill) {
    std::vector<uint8_t> spk;
    spk.push_back(0x51);
    spk.push_back(0x20);
    for (int i = 0; i < 32; i++) spk.push_back(static_cast<uint8_t>(fill + i));
    return spk;
}

int main() {
    std::cout << "=== WalletManager::rescanUtxoSet regression test ===" << std::endl;

    dinero::SelectParams(dinero::Chain::MAINNET);

    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
        ("rescan_utxoset_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    const uint32_t kSnapshotHeight = 52287;
    const uint32_t kCoinHeight = 52000;
    const uint64_t kAmount = 5000000000ULL;  // 50 DIN in una

    // The wallet-owned script and a foreign script that must be ignored.
    std::vector<uint8_t> owned_spk = p2tr(0x10);
    std::vector<uint8_t> foreign_spk = p2tr(0x90);

    try {
        WalletManager wallet(dir);
        wallet.create("snapshot_wallet");
        wallet.open("snapshot_wallet");

        // Register only the owned script for watching.
        wallet.addWatchScript(owned_spk, "m/86'/0'/0'/0/0", false);

        // Baseline: nothing recorded yet.
        check(wallet.getBalance().utxo_count == 0, "wallet starts empty");

        // Producer yields one owned coin and one foreign coin.
        auto producer = [&](const std::function<void(const WalletManager::UtxoSetEntry&)>& sink) {
            WalletManager::UtxoSetEntry owned;
            owned.txid_hex = "1111111111111111111111111111111111111111111111111111111111111111";
            owned.vout = 0;
            owned.amount_una = kAmount;
            owned.script_pubkey = owned_spk;
            owned.height = kCoinHeight;
            owned.is_coinbase = false;
            sink(owned);

            WalletManager::UtxoSetEntry foreign;
            foreign.txid_hex = "2222222222222222222222222222222222222222222222222222222222222222";
            foreign.vout = 0;
            foreign.amount_una = kAmount;
            foreign.script_pubkey = foreign_spk;  // not in watch_scripts
            foreign.height = kCoinHeight;
            foreign.is_coinbase = false;
            sink(foreign);
        };

        int recorded = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(recorded == 1, "exactly one owned coin recorded (foreign coin ignored)");

        auto bal = wallet.getBalance();
        check(bal.utxo_count == 1, "wallet utxo_count == 1 after snapshot rescan");
        check(bal.total > 0.0, "wallet balance is non-zero after snapshot rescan");
        check(wallet.getCurrentBlockchainHeight() == kSnapshotHeight,
              "scan-height watermark advanced to snapshot height");

        // Idempotency: re-running records nothing new and does not double-count.
        int recorded2 = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(recorded2 == 0, "re-run records 0 new coins (INSERT OR IGNORE)");
        check(wallet.getBalance().utxo_count == 1, "utxo_count still 1 after re-run");

    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: exception: " << e.what() << std::endl;
        ++g_failures;
    }

    fs::remove_all(dir);

    if (g_failures == 0) {
        std::cout << "\n✓ All rescanUtxoSet checks passed." << std::endl;
        return 0;
    }
    std::cerr << "\n✗ " << g_failures << " check(s) failed." << std::endl;
    return 1;
}
