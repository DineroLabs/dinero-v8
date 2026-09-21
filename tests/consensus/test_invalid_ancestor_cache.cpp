/*
 * test_invalid_ancestor_cache — unit gate for the HasInvalidAncestor() cost fix.
 *
 * Field incident (SJ, 2026-09-21): a pool node that had accumulated ~3,300
 * same-height sibling tips ran GetBestCandidate() on every ActivateBestChain
 * pass, and GetBestCandidate() calls HasInvalidAncestor() for every candidate.
 * The walk went back to genesis for each one (no BLOCK_FAILED_VALID anywhere,
 * so nothing was ever cached), ~3,300 x 114,000 pprev steps ≈ 1.3 s of CPU per
 * pass, held under the activation + block-ingress locks. Peer threads that
 * process messages inline could not read their sockets while waiting for that
 * lock, kernel receive queues filled, peers were evicted, and the requested
 * bodies never got processed.
 *
 * The fix records "ancestry is clean" per block once walked, so the cost over
 * many siblings/descendants is O(new blocks). InvalidateAncestryCache() must be
 * called by every site that sets BLOCK_FAILED_VALID outside MarkBlockInvalid().
 *
 * Deterministic: counts pprev steps via g_invalid_ancestor_walk_steps rather
 * than wall-clock time.
 */
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"

using dinero::CBlockIndex;
using dinero::BlockHeader;

static int g_failures = 0;
static void check(bool cond, const std::string& name) {
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
    if (!cond) g_failures++;
}

// Standalone CBlockIndex with a unique hash derived from a counter. The graph
// predicate only reads hash / status / pprev, so no g_block_index insertion.
static CBlockIndex* mk(uint32_t height, CBlockIndex* parent, uint64_t serial) {
    BlockHeader hdr;
    auto* idx = new CBlockIndex(hdr, height);
    char hx[65];
    snprintf(hx, sizeof(hx), "%016llx%016llx%016llx%016llx",
             static_cast<unsigned long long>(serial), 0ULL, 0ULL,
             static_cast<unsigned long long>(height));
    idx->hash = dinero::uint256::FromHexUnsafe(hx);
    idx->height = height;
    idx->status = dinero::BLOCK_VALID_CHAIN | dinero::BLOCK_HAVE_DATA;
    idx->pprev = parent;
    return idx;
}

int main() {
    std::cout << "=== HasInvalidAncestor ancestry-clean cache ===\n";

    constexpr uint32_t kChainHeight = 20000;
    constexpr uint32_t kSiblings = 2000;

    dinero::InvalidateAncestryCache();
    dinero::g_invalid_descendants.clear();

    std::vector<CBlockIndex*> chain;
    chain.reserve(kChainHeight + 1);
    uint64_t serial = 1;
    chain.push_back(mk(0, nullptr, serial++));
    for (uint32_t h = 1; h <= kChainHeight; ++h) {
        chain.push_back(mk(h, chain.back(), serial++));
    }
    CBlockIndex* tip = chain.back();

    std::vector<CBlockIndex*> siblings;
    siblings.reserve(kSiblings);
    for (uint32_t i = 0; i < kSiblings; ++i) {
        siblings.push_back(mk(kChainHeight + 1, tip, serial++));
    }

    // 1. Many siblings of one tip: the chain is walked once, siblings cost O(1).
    {
        dinero::g_invalid_ancestor_walk_steps = 0;
        bool any_invalid = false;
        for (CBlockIndex* s : siblings) {
            any_invalid = any_invalid || dinero::HasInvalidAncestor(s);
        }
        check(!any_invalid, "clean chain: no sibling reports an invalid ancestor");
        const uint64_t steps = dinero::g_invalid_ancestor_walk_steps;
        // Unfixed: kSiblings * kChainHeight = 40,000,000 steps.
        // Fixed: one full walk (kChainHeight) + one step per further sibling.
        const uint64_t bound = static_cast<uint64_t>(kChainHeight) + 2ULL * kSiblings;
        std::cout << "    walk steps over " << kSiblings << " siblings at height "
                  << kChainHeight << ": " << steps << " (bound " << bound << ")\n";
        check(steps <= bound, "sibling scan is amortized O(1) per candidate, not O(chain height)");
    }

    // 2. A second pass over the same siblings must not re-walk the chain at all.
    {
        dinero::g_invalid_ancestor_walk_steps = 0;
        for (CBlockIndex* s : siblings) {
            (void)dinero::HasInvalidAncestor(s);
        }
        check(dinero::g_invalid_ancestor_walk_steps <= kSiblings,
              "repeat pass costs at most one step per sibling");
    }

    // 3. Correctness: a failure flag set directly (bypassing MarkBlockInvalid) is
    //    seen once the cache is invalidated, for old and new descendants alike.
    {
        CBlockIndex* mid = chain[kChainHeight / 2];
        mid->status |= dinero::BLOCK_FAILED_VALID;
        dinero::InvalidateAncestryCache();
        check(dinero::HasInvalidAncestor(siblings[0]),
              "after invalidation, an existing sibling sees the failed ancestor");
        CBlockIndex* fresh = mk(kChainHeight + 1, tip, serial++);
        check(dinero::HasInvalidAncestor(fresh),
              "after invalidation, a new sibling sees the failed ancestor");
        check(!dinero::HasInvalidAncestor(chain[kChainHeight / 2 - 1]),
              "blocks below the failed one remain clean");
        // The positive result is cached in g_invalid_descendants as before.
        check(dinero::g_invalid_descendants.count(siblings[0]->hash) == 1,
              "positive result still recorded in g_invalid_descendants");
    }

    // 4. MarkBlockInvalid() itself invalidates the cache.
    {
        dinero::InvalidateAncestryCache();
        dinero::g_invalid_descendants.clear();
        chain[kChainHeight / 2]->status &= ~dinero::BLOCK_FAILED_VALID;
        CBlockIndex* base = mk(0, nullptr, serial++);
        CBlockIndex* a = mk(1, base, serial++);
        CBlockIndex* b = mk(2, a, serial++);
        check(!dinero::HasInvalidAncestor(b), "fresh branch is clean before marking");
        dinero::MarkBlockInvalid(a, dinero::BlockRejectReason::INVALID_POW, "test");
        check(dinero::HasInvalidAncestor(b), "MarkBlockInvalid is visible through the cache");
    }

    // 5. PR #800 review finding (orphan_cache_repro): a walk that ends at a
    //    MISSING parent (orphan-queued, pprev == nullptr, height > 0) proves
    //    nothing and must not be cached. When the parent later arrives through
    //    the real AddBlockIndex() relink and sits below an already-invalid
    //    block, HasInvalidAncestor() must report it. Uses the real block index.
    {
        dinero::InvalidateAncestryCache();
        dinero::g_invalid_descendants.clear();
        auto header = [](const dinero::uint256& parent, uint32_t serial) {
            BlockHeader h{};
            h.prev_block_hash = parent;
            h.difficulty = 0x207fffff;
            h.timestamp = 1'900'000'000u + serial;
            return h;
        };
        auto genesis_header = header(dinero::uint256{}, 1);
        CBlockIndex* genesis = dinero::AddBlockIndex(genesis_header, 0);
        genesis->status |= dinero::BLOCK_VALID_CHAIN | dinero::BLOCK_HAVE_DATA;
        auto failed_header = header(genesis->hash, 2);
        CBlockIndex* failed = dinero::AddBlockIndex(failed_header, 1);
        dinero::MarkBlockInvalid(failed, dinero::BlockRejectReason::INVALID_POW,
                                 "known invalid ancestor");
        auto missing_header = header(failed->hash, 3);           // not inserted yet
        auto orphan_header = header(missing_header.GetHash(), 4);
        CBlockIndex* orphan = dinero::AddBlockIndex(orphan_header, 3);
        auto child_header = header(orphan->hash, 5);
        CBlockIndex* child = dinero::AddBlockIndex(child_header, 4);
        check(orphan->pprev == nullptr, "orphan has no parent link while its parent is missing");
        check(!dinero::HasInvalidAncestor(child),
              "with the parent missing, no invalid ancestor is visible yet");
        CBlockIndex* arrived = dinero::AddBlockIndex(missing_header, 2);
        check(orphan->pprev == arrived && arrived->pprev == failed,
              "arriving parent relinks the orphan below the invalid block");
        check(dinero::HasInvalidAncestor(child),
              "after the relink the invalid ancestor is reported (walk to a missing parent must not be cached)");
        check(dinero::HasInvalidAncestor(orphan),
              "the relinked orphan itself reports the invalid ancestor");
        // A complete, clean chain is still cached: a sibling of `child` above a
        // clean genesis-rooted branch costs O(1) on repeat.
        auto clean_root_header = header(dinero::uint256{}, 6);
        CBlockIndex* clean_root = dinero::AddBlockIndex(clean_root_header, 0);
        auto clean_a_header = header(clean_root->hash, 7);
        CBlockIndex* clean_a = dinero::AddBlockIndex(clean_a_header, 1);
        auto clean_b_header = header(clean_a->hash, 8);
        CBlockIndex* clean_b = dinero::AddBlockIndex(clean_b_header, 2);
        check(!dinero::HasInvalidAncestor(clean_b), "genesis-rooted clean branch is clean");
        dinero::g_invalid_ancestor_walk_steps = 0;
        check(!dinero::HasInvalidAncestor(clean_b) && dinero::g_invalid_ancestor_walk_steps <= 1,
              "genesis-rooted clean branch is cached after one walk");
    }

    std::cout << (g_failures == 0
                  ? "\n✅ ALL INVALID-ANCESTOR CACHE TESTS PASSED\n"
                  : "\n❌ FAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
