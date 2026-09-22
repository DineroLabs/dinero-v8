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
 *   1. ForkPriorToEstablishedCheckpoint — conflicting blocks at or below an
 *      established active-chain checkpoint are rejected,
 *      except a re-delivered main-chain block, which stays a duplicate.
 *   2. ForkAwareOverlayWithinDepth — the eager utreexo root pre-check is skipped
 *      for forks deeper than kMaxForkAwareOverlayDepth.
 */
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cstring>

#include "consensus/fork_acceptance_policy.h"

using dinero::consensus::ForkAwareOverlayWithinDepth;
using dinero::consensus::ForkPriorToEstablishedCheckpoint;
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

    std::vector<dinero::CBlockIndex> active(13002);
    for (uint32_t h = 0; h < active.size(); ++h) {
        active[h].height = h;
        std::memcpy(active[h].hash.data, &h, sizeof(h));
        active[h].pprev = h ? &active[h - 1] : nullptr;
    }
    active[13000].hash = dinero::uint256::FromHexUnsafe(mainnet.at(13000));
    const auto foreign = dinero::uint256::FromHexUnsafe(std::string(64, 'f'));
    auto rejects = [&](uint32_t height) {
        return ForkPriorToEstablishedCheckpoint(height, foreign, mainnet, &active.back()).has_value();
    };
    for (uint32_t h : {1988u, 2086u, 2234u})
        check(rejects(h), "SJ foreign block below established checkpoint is rejected");
    check(rejects(13000), "foreign block at established checkpoint is rejected");
    check(!rejects(13001), "block above checkpoint is not rejected by this rule");
    check(!ForkPriorToEstablishedCheckpoint(2234, active[2234].hash, mainnet, &active.back()),
          "canonical duplicate below established checkpoint remains eligible");
    check(!ForkPriorToEstablishedCheckpoint(2234, foreign, none, &active.back()),
          "no checkpoint means no checkpoint-fork rejection");
    check(!ForkPriorToEstablishedCheckpoint(999, foreign, mainnet, &active[1000]),
          "initial sync below checkpoint permits competing branch bodies");
    check(!ForkPriorToEstablishedCheckpoint(999, foreign, mainnet, nullptr),
          "missing active ancestry does not prove a fork conflict");
    const auto saved = active[13000].hash;
    active[13000].hash = foreign;
    check(!ForkPriorToEstablishedCheckpoint(999, foreign, mainnet, &active.back()),
          "height alone does not establish the configured checkpoint hash");
    active[13000].hash = saved;
    auto future = mainnet;
    future.emplace(20000, std::string(64, 'a'));
    check(ForkPriorToEstablishedCheckpoint(999, foreign, future, &active.back()) == 13000,
          "an unknown future checkpoint does not hide an established earlier checkpoint");
    auto* parent = active[1000].pprev;
    active[1000].pprev = nullptr;
    check(!ForkPriorToEstablishedCheckpoint(999, foreign, mainnet, &active.back()),
          "missing historical ancestry is not guessed to be a conflict");
    active[1000].pprev = parent;

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
