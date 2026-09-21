/*
 * test_fork_acceptance_policy — unit gate for dinero-v8 #803.
 *
 * SJ mainnet, 2026-09-21: a peer fed blocks from an abandoned April-era branch
 * (heights 1988, 2086, 2234; mainnet's last checkpoint is 13000). Each was
 * accepted as a side-chain block and the acceptor's fork-aware utreexo overlay
 * then walked main-chain undo data from tip 114173 down to the fork parent
 * under the block-ingress lock: ~40 minutes per block, node otherwise idle.
 *
 * Two pure rules close this:
 *   1. ForksPriorToLastCheckpoint — a new block at or below the last checkpoint
 *      height is rejected outright (Bitcoin Core's bad-fork-prior-to-checkpoint),
 *      except a re-delivered main-chain block, which stays a duplicate.
 *   2. ForkAwareOverlayWithinDepth — the eager utreexo root pre-check is skipped
 *      for forks deeper than kMaxForkAwareOverlayDepth.
 */
#include <iostream>
#include <map>
#include <string>

#include "consensus/fork_acceptance_policy.h"

using dinero::consensus::ForkAwareOverlayWithinDepth;
using dinero::consensus::ForksPriorToLastCheckpoint;
using dinero::consensus::LastCheckpointHeight;
using dinero::consensus::kMaxForkAwareOverlayDepth;

static int g_failures = 0;
static void check(bool cond, const std::string& name) {
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
    if (!cond) g_failures++;
}

int main() {
    std::cout << "=== fork acceptance policy (#803) ===\n";

    // Mainnet shape: genesis + one checkpoint at 13000.
    const std::map<uint32_t, std::string> mainnet{
        {0, "genesis"},
        {13000, "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3"},
    };
    const std::map<uint32_t, std::string> none{};

    check(LastCheckpointHeight(mainnet) == 13000, "last checkpoint height is the highest key");
    check(LastCheckpointHeight(none) == 0, "no checkpoints -> 0");

    // 1. The SJ specimens: new blocks at 1988 / 2086 / 2234 on a foreign branch.
    for (uint32_t h : {1988u, 2086u, 2234u}) {
        check(ForksPriorToLastCheckpoint(h, mainnet, /*canonical=*/false),
              "new block at height " + std::to_string(h) + " below checkpoint 13000 is rejected");
    }
    check(ForksPriorToLastCheckpoint(13000, mainnet, false),
          "a different block AT the checkpoint height is rejected");
    check(!ForksPriorToLastCheckpoint(13001, mainnet, false),
          "a fork just above the last checkpoint is not rejected by this rule");
    check(!ForksPriorToLastCheckpoint(2234, mainnet, /*canonical=*/true),
          "a re-delivered main-chain block below the checkpoint is not rejected (duplicate path)");
    check(!ForksPriorToLastCheckpoint(2234, none, false),
          "without checkpoints nothing is rejected");

    // 2. Overlay depth cap.
    check(!ForkAwareOverlayWithinDepth(114173, 2233),
          "SJ specimen: fork depth 111940 skips the eager overlay");
    check(ForkAwareOverlayWithinDepth(114173, 114171),
          "a sibling two blocks back keeps the overlay pre-check");
    check(ForkAwareOverlayWithinDepth(114173, 114173 - kMaxForkAwareOverlayDepth),
          "exactly kMaxForkAwareOverlayDepth deep is still checked");
    check(!ForkAwareOverlayWithinDepth(114173, 114173 - kMaxForkAwareOverlayDepth - 1),
          "one deeper than kMaxForkAwareOverlayDepth is skipped");
    check(ForkAwareOverlayWithinDepth(10, 20),
          "parent above tip (racing tip) is treated as within depth");

    std::cout << (g_failures == 0
                  ? "\n✅ ALL FORK ACCEPTANCE POLICY TESTS PASSED\n"
                  : "\n❌ FAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
