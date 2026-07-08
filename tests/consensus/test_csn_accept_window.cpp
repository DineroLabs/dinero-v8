/**
 * #384: the CSN forward-validate cursor must track the active chain tip so a
 * node that restarts mid-backfill cannot deadlock rejecting every requested
 * block as "too far ahead of cursor 1" (DineroDPI phone, 2026-07-07).
 *
 * ReconcileCsnCursorToTip is the pure decision run before every cursor-
 * dependent guard in OnUtxoBlock. Exit non-zero on failure (no assert()).
 */

#include "consensus/csn_accept_window.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using dinero::consensus::ReconcileCsnCursorToTip;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAIL: " << (msg) << " (" << #cond << ") line "    \
                      << __LINE__ << std::endl;                             \
            std::exit(1);                                                   \
        }                                                                   \
    } while (0)

int main() {
    constexpr uint32_t kWindow = 256;  // MAX_PENDING_WINDOW in OnUtxoBlock

    // THE phone deadlock: cursor stuck at 1, active tip 53848, a requested
    // forward block at 53850 arrives. Pre-fix it is rejected (53850 > 1+256);
    // the reconcile must advance the cursor to 53849 so 53850 is in-window.
    {
        const uint32_t reconciled = ReconcileCsnCursorToTip(1, 53848);
        CHECK(reconciled == 53849, "cursor must advance to active_tip+1");
        CHECK(static_cast<uint64_t>(53850) <= static_cast<uint64_t>(reconciled) + kWindow,
              "forward block must fall within the window after reconcile "
              "(the deadlock is broken)");
    }

    // Backfill routing: after reconcile a pre-base body (1578) is far BELOW
    // the cursor, so OnUtxoBlock's below-cursor branch routes it to the
    // store-only backfill lane instead of the too-far-ahead reject.
    {
        const uint32_t reconciled = ReconcileCsnCursorToTip(1, 53848);
        CHECK(1578 < reconciled,
              "pre-base backfill body must be below the reconciled cursor");
    }

    // No active tip yet (genuinely fresh node): leave the cursor untouched so
    // the low-block bootstrap path is unchanged.
    CHECK(ReconcileCsnCursorToTip(1, -1) == 1,
          "no active tip must leave the cursor as-is");

    // The window guard still protects: a block absurdly far beyond even the
    // reconciled cursor stays out-of-window (caller rejects it).
    {
        const uint32_t reconciled = ReconcileCsnCursorToTip(1, 100);
        CHECK(reconciled == 101, "reconcile to small tip");
        CHECK(static_cast<uint64_t>(5000) > static_cast<uint64_t>(reconciled) + kWindow,
              "absurdly-far block stays out-of-window after reconcile");
    }

    // Never regress: an already-ahead cursor is not pulled back to a smaller
    // tip+1 (e.g. during a transient GetActiveTip lag).
    CHECK(ReconcileCsnCursorToTip(60000, 100) == 60000,
          "reconcile must never move the cursor backward");

    // Equal case: tip+1 == cursor → unchanged.
    CHECK(ReconcileCsnCursorToTip(53849, 53848) == 53849,
          "cursor already at tip+1 stays put");

    std::cout << "All #384 CSN cursor-reconcile tests passed" << std::endl;
    return 0;
}
