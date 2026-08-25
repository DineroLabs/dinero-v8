/*
 * test_reorg_candidate_eligibility — unit gate for the #309 connect-abort fix.
 *
 * The #309 connect-abort variant occurs when a reorg promotes a candidate whose
 * branch is missing a body, so the reorg ConnectTip walk hits the gap and aborts
 * (REORG ABORT: connect failed), wedging the node. The fix prevents this by
 * requiring WHOLE-branch data before a not-yet-validated side branch may be a
 * reorg candidate: BranchHasDataToConnectedBase() must hold. A partial branch
 * (a body gap above the connected base) is therefore never promoted, so the
 * connect-walk-into-a-gap can never happen.
 *
 * This test exercises that predicate directly over hand-built CBlockIndex graphs
 * (deterministic, no network/timing), plus the full reorg-candidacy composition.
 */
#include <iostream>
#include <vector>
#include <memory>
#include <string>

#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/activation_retry.h"

using dinero::CBlockIndex;
using dinero::BlockHeader;

static int g_failures = 0;
static void check(bool cond, const std::string& name) {
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
    if (!cond) g_failures++;
}

// Build a standalone CBlockIndex with a unique hash, given height/status/parent.
// (Heap-allocated and intentionally leaked; this is a short-lived test process.)
static CBlockIndex* mk(uint32_t height, uint32_t status, CBlockIndex* parent,
                       const std::string& tag) {
    BlockHeader hdr;  // contents irrelevant to the pure graph predicate
    auto* idx = new CBlockIndex(hdr, height);
    // Unique 32-byte hash derived from the tag so nodes are distinct.
    std::string hx;
    for (char c : tag) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c));
        hx += buf;
    }
    while (hx.size() < 64) hx += "0";
    hx = hx.substr(0, 64);
    idx->hash = dinero::uint256::FromHexUnsafe(hx);
    idx->height = height;
    idx->status = status;
    idx->pprev = parent;
    return idx;
}

// Compose the full reorg-candidacy decision the way ChainstateService does, so
// the test also covers the failed-flag / have-data gates around the predicate.
static bool reorg_eligible(CBlockIndex* b) {
    if (!b) return false;
    if (b->status & (dinero::BLOCK_FAILED_VALID | dinero::BLOCK_FAILED_CHILD)) return false;
    if (!(b->status & dinero::BLOCK_HAVE_DATA)) return false;
    if (b->status & dinero::BLOCK_VALID_CHAIN) return true;
    return dinero::BranchHasDataToConnectedBase(b);
}

int main() {
    using dinero::BLOCK_VALID_CHAIN;
    using dinero::BLOCK_HAVE_DATA;
    using dinero::BLOCK_FAILED_VALID;

    const uint32_t DATA = BLOCK_HAVE_DATA;
    const uint32_t CONNECTED = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

    std::cout << "=== BranchHasDataToConnectedBase ===\n";

    // 1. Complete branch off a connected base → eligible.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base1");
        CBlockIndex* b1   = mk(11, DATA, base, "c1a");
        CBlockIndex* b2   = mk(12, DATA, b1,   "c1b");
        CBlockIndex* tip  = mk(13, DATA, b2,   "c1tip");
        check(dinero::BranchHasDataToConnectedBase(tip), "complete branch (all bodies) → true");
        check(reorg_eligible(tip), "complete branch → reorg-eligible");
    }

    // 2. THE CONNECT-ABORT CASE: a body gap in the middle → NOT eligible.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base2");
        CBlockIndex* b1   = mk(11, DATA, base, "c2a");
        CBlockIndex* gap  = mk(12, 0 /* no HAVE_DATA */, b1, "c2gap");
        CBlockIndex* tip  = mk(13, DATA, gap, "c2tip");
        check(!dinero::BranchHasDataToConnectedBase(tip),
              "mid-branch body gap → false (prevents connect-abort)");
        check(!reorg_eligible(tip), "partial branch → NOT reorg-eligible");
    }

    // 3. Gap immediately above the base → NOT eligible.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base3");
        CBlockIndex* g1   = mk(11, 0, base, "c3gap");
        CBlockIndex* tip  = mk(12, DATA, g1, "c3tip");
        check(!dinero::BranchHasDataToConnectedBase(tip), "gap above base → false");
    }

    // 4. Tip already connected (VALID_CHAIN) → trivially true.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base4");
        CBlockIndex* tip  = mk(11, CONNECTED, base, "c4tip");
        check(dinero::BranchHasDataToConnectedBase(tip), "already-connected tip → true");
    }

    // 5. Branch never roots in a connected base (runs to a no-data root) → false.
    {
        CBlockIndex* root = mk(10, DATA, nullptr, "c5root");   // has data but NOT connected
        CBlockIndex* tip  = mk(11, DATA, root, "c5tip");
        check(!dinero::BranchHasDataToConnectedBase(tip), "no connected base → false");
    }

    // 6. Genesis-with-data acts as a base.
    {
        CBlockIndex* genesis = mk(0, DATA, nullptr, "c6gen");   // height 0, has data
        CBlockIndex* b1      = mk(1, DATA, genesis, "c6a");
        CBlockIndex* tip     = mk(2, DATA, b1, "c6tip");
        check(dinero::BranchHasDataToConnectedBase(tip), "genesis-with-data base → true");
    }

    // 7. nullptr → false.
    check(!dinero::BranchHasDataToConnectedBase(nullptr), "nullptr → false");

    std::cout << "=== BranchHasDataToFork ===\n";

    // The #650 shape: a side-branch VALID_CHAIN flag above a header-only gap
    // must not be mistaken for the active-chain boundary.
    {
        CBlockIndex* fork = mk(2, CONNECTED, nullptr, "fork10");
        CBlockIndex* active = mk(3, CONNECTED, fork, "active10");
        CBlockIndex* gap = mk(3, BLOCK_VALID_CHAIN, fork, "gap10");
        CBlockIndex* misleading = mk(4, CONNECTED, gap, "valid10");
        CBlockIndex* tip = mk(5, DATA, misleading, "tip10");
        check(dinero::BranchHasDataToConnectedBase(tip),
              "connected-base shortcut is fooled by side-branch VALID_CHAIN");
        check(!dinero::BranchHasDataToFork(tip, active),
              "actual-fork preflight rejects the hidden body gap");
    }

    // A complete replacement branch is ready regardless of validation level.
    {
        CBlockIndex* fork = mk(2, CONNECTED, nullptr, "fork11");
        CBlockIndex* active = mk(3, CONNECTED, fork, "active11");
        CBlockIndex* side = mk(3, DATA, fork, "side11");
        CBlockIndex* tip = mk(4, DATA, side, "tip11");
        check(dinero::BranchHasDataToFork(tip, active),
              "complete branch to the actual fork is ready");
    }

    // Unrelated graphs are never ready for activation.
    {
        CBlockIndex* active = mk(3, CONNECTED, nullptr, "active12");
        CBlockIndex* candidate = mk(4, DATA, nullptr, "candidate12");
        check(!dinero::BranchHasDataToFork(candidate, active),
              "branches without a common ancestor are rejected");
    }

    std::cout << "=== reorg-candidacy composition (failed / no-data gates) ===\n";

    // 8. A failed block (even with whole-branch data) is never eligible.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base8");
        CBlockIndex* tip  = mk(11, BLOCK_HAVE_DATA | BLOCK_FAILED_VALID, base, "c8tip");
        check(!reorg_eligible(tip), "failed tip → not eligible");
    }

    // 9. A tip without its own body is never eligible.
    {
        CBlockIndex* base = mk(10, CONNECTED, nullptr, "base9");
        CBlockIndex* tip  = mk(11, 0, base, "c9tip");
        check(!reorg_eligible(tip), "tip without body → not eligible");
    }

    std::cout << "=== operational activation retry preservation ===\n";
    {
        using namespace std::chrono_literals;
        dinero::consensus::ActivationRetryTracker retries(10ms, 40ms);
        const auto candidate = mk(12, DATA, nullptr, "retry")->hash;
        const auto start = dinero::consensus::ActivationRetryTracker::TimePoint{};
        check(retries.IsReady(candidate, start), "new candidate is immediately eligible");
        check(retries.RecordFailure(candidate, start) == 10ms, "first retry uses millisecond cooldown");
        check(!retries.IsReady(candidate, start + 9ms), "candidate stays preserved but cooled down");
        check(retries.IsReady(candidate, start + 10ms), "candidate automatically becomes retryable");
        check(retries.RecordFailure(candidate, start + 10ms) == 20ms, "retry delay backs off");
        check(retries.RecordFailure(candidate, start + 30ms) == 40ms, "retry delay reaches cap");
        check(retries.RecordFailure(candidate, start + 70ms) == 40ms, "retry delay remains capped");
        retries.Clear(candidate);
        check(retries.IsReady(candidate, start), "success clears retry state");
    }

    std::cout << (g_failures == 0
                  ? "\n✅ ALL REORG-CANDIDATE ELIGIBILITY TESTS PASSED\n"
                  : "\n❌ FAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
