// Regression guard (Jun 2026): the 1024-block startup undo audit
// (ChainstateService::VerifyActiveChainUndoCoverage ->
// CheckBlockDisconnectMaterialDurable) used to read back coinbase output 0
// from chaindb for EVERY block in the window. For a block whose coinbase
// has matured and been spent, that output is legitimately gone
// (gettxout == null) — yet the audit treated the missing read-back as an
// atomicity failure and wrote a false chainstate_recovery.marker. This was
// observed fleet-wide (LA / CN / SJ) at "coinbase output 0 not in chaindb
// at height=35600" while the tip was ~36410 (depth 810, coinbase long
// mature and spent, block fully present with 800+ confirmations).
//
// The fix gates the read-back on coinbase immaturity via
// dinero::daemon::CoinbaseReadbackApplies. This test pins that gate's
// boundary. Neuter check: make CoinbaseReadbackApplies always return true
// (the pre-fix behavior) and the maturity/production cases below fail.

#include "daemon/coinbase_readback_gate.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using dinero::daemon::CoinbaseReadbackApplies;

namespace {

int g_failures = 0;

// Throwing/exit-nonzero check — NOT assert(), which is a no-op under
// NDEBUG (Release/CI), where this test must still gate.
void check(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

constexpr uint32_t kMainnetMaturity = 100;  // Params().coinbase_maturity (mainnet)
constexpr uint32_t kRegtestMaturity = 10;   // Params().coinbase_maturity (regtest)

}  // namespace

int main() {
    // --- Post-commit ConnectTip path: reference tip == the block being
    //     connected (depth 0). Coinbase always immature -> read-back MUST
    //     run so the atomicity invariant stays fully enforced. ---
    check(CoinbaseReadbackApplies(36410, 36410, kMainnetMaturity),
          "depth 0 (post-commit ConnectTip) must still run the read-back");

    // --- Immature, just below the maturity boundary (depth 99 < 100):
    //     coinbase cannot have been spent yet -> read-back applies. ---
    check(CoinbaseReadbackApplies(35699, 35600, kMainnetMaturity),
          "depth 99 (immature) must run the read-back");

    // --- Maturity boundary (depth == maturity == 100): coinbase is now
    //     spendable, so output 0 may be legitimately spent -> SKIP. ---
    check(!CoinbaseReadbackApplies(35700, 35600, kMainnetMaturity),
          "depth 100 (just matured) must skip the read-back");

    // --- THE production false-positive: height=35600, tip=36410, depth
    //     810 >> 100. Pre-fix this wrote a false chainstate_recovery.marker.
    //     Post-fix: mature -> skip. ---
    check(!CoinbaseReadbackApplies(36410, 35600, kMainnetMaturity),
          "production case height=35600 tip=36410 (depth 810) must skip");

    // --- Regtest maturity is 10, not 100: the gate must use the network
    //     value, so the boundary moves. depth 9 -> run; depth 10 -> skip. ---
    check(CoinbaseReadbackApplies(109, 100, kRegtestMaturity),
          "regtest depth 9 (immature) must run the read-back");
    check(!CoinbaseReadbackApplies(110, 100, kRegtestMaturity),
          "regtest depth 10 (matured) must skip the read-back");

    // --- Conservative fallback: tip behind the block (should not happen
    //     in practice) -> keep checking rather than silently skip. ---
    check(CoinbaseReadbackApplies(50, 100, kMainnetMaturity),
          "tip behind block must conservatively run the read-back");

    if (g_failures != 0) {
        std::cerr << "\n=== test_undo_audit_coinbase_gate: "
                  << g_failures << " FAILURE(S) ===" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "[PASS] coinbase read-back maturity gate boundary correct"
              << std::endl;
    return EXIT_SUCCESS;
}
