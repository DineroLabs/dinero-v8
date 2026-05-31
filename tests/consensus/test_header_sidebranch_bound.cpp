/**
 * Bounded side-branch header storage — work-aware eviction (4d-2 / issue #181)
 *
 * HeaderChainSelector bounds storage of losing/side-fork headers so a low-work
 * header flood cannot exhaust memory/disk, WITHOUT ever impairing the node's
 * ability to follow the most-work chain — including a reorg that arrives AFTER
 * an attacker has filled the side-branch budget with junk.
 *
 * Regtest is used so ValidateHeader skips PoW/ASERT and we can mint synthetic
 * headers. Under regtest every header carries the same per-header work, so
 * cumulative work is a function of height: a header at height h has work
 * proportional to (h+1). That gives the SAME asymmetry the real network has via
 * difficulty (junk forked near genesis = low cumulative work; a real reorg
 * forked near the tip = high cumulative work), which is exactly the signal the
 * work-aware eviction ranks on.
 *
 * Tests:
 *   #1 reorg-after-flood (ACCEPTANCE GATE) — fill the budget with low-work junk,
 *      then feed a competing chain whose tip exceeds best; assert the node
 *      REORGS to it. (This is the test the rejected count-only cap FAILS.)
 *   #2 static bound — a pure low-work flood is bounded; active chain + best tip
 *      intact.
 *   #3 no-orphan / integrity — after an eviction storm, no surviving header is
 *      orphaned and the best chain is fully present and linked.
 *   #4 restart equivalence — persist a flooded store, reload, and confirm the
 *      bound + best chain + no-orphan invariants hold post-LoadFromStorage, and
 *      the bound is still enforced on the live path afterwards.
 *
 * Built with NDEBUG, so force asserts on (mirrors test_header_restart_safety).
 */

#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace dinero;
using namespace dinero::consensus;

namespace {

// Monotonic timestamp source: a strictly-increasing timestamp guarantees a
// header's time exceeds any ancestor's median-time-past (so ValidateHeader's
// timestamp rule always passes) AND makes every header hash distinct even when
// it shares a parent.
uint32_t g_time = 1'000'000;
uint32_t NextTime() { return ++g_time; }

BlockHeader MakeHeader(const uint256& prev) {
    BlockHeader h{};  // value-init: zeroes reserved[12] (consensus requires it zero)
    h.version = 1;
    h.prev_block_hash = prev;
    h.merkle_root = uint256();
    h.utreexo_root = uint256();
    h.timestamp = NextTime();
    h.difficulty = 0x1d00ffff;
    h.nonce = 1;
    return h;
}

// Extend `tip_hash` by `n` headers via the selector; returns the new tip hash.
// If `record` is non-null, appends each (hash, prev) pair.
uint256 ExtendChain(HeaderChainSelector& sel, uint256 tip_hash, int n,
                    std::vector<std::pair<uint256, uint256>>* record = nullptr) {
    for (int i = 0; i < n; ++i) {
        BlockHeader h = MakeHeader(tip_hash);
        bool ok = sel.AddHeader(h);
        assert(ok && "extending the best chain must always be accepted");
        uint256 hh = h.GetHash();
        if (record) record->emplace_back(hh, tip_hash);
        tip_hash = hh;
    }
    return tip_hash;
}

// The number of stored headers minus the best chain length = side branches.
size_t SideCount(const HeaderChainSelector& sel) {
    const auto* best = sel.GetBestHeader();
    const size_t active_len = best ? static_cast<size_t>(best->height) + 1 : 0;
    const size_t total = sel.GetHeaderCount();
    return total > active_len ? total - active_len : 0;
}

// Assert the best chain is fully present and correctly linked genesis..tip.
void AssertBestChainIntact(const HeaderChainSelector& sel) {
    const auto* best = sel.GetBestHeader();
    assert(best != nullptr);
    const uint32_t h = best->height;
    const HeaderIndexEntry* prev = nullptr;
    for (uint32_t i = 0; i <= h; ++i) {
        const auto* e = sel.GetHeaderAtHeight(i);
        assert(e != nullptr && "every best-chain height must be present");
        assert(e->height == i);
        if (prev != nullptr) {
            assert(e->prev_hash == prev->hash && "best chain must be linked");
        }
        prev = e;
    }
    assert(prev != nullptr && prev->hash == best->hash);
}

constexpr size_t kCap = 10000;  // mirrors MAX_SIDE_BRANCH_HEADERS

// ---------------------------------------------------------------------------
// #1 reorg-after-flood — the acceptance gate.
// ---------------------------------------------------------------------------
void TestReorgAfterFlood() {
    std::cout << "\n#1 reorg-after-flood (acceptance gate)..." << std::endl;
    HeaderChainSelector sel;

    // Main chain: genesis (h0) .. h50. best work ∝ 51.
    BlockHeader genesis = MakeHeader(uint256());
    assert(sel.AddHeader(genesis));
    const uint256 g = genesis.GetHash();
    uint256 main_tip = ExtendChain(sel, g, 50);
    const uint256 old_main_tip = main_tip;
    const auto* fork40 = sel.GetHeaderAtHeight(40);
    assert(fork40 != nullptr);
    const uint256 fork40_hash = fork40->hash;
    assert(sel.GetBestHeader()->height == 50);

    // Flood the budget with low-work height-1 junk forked off genesis (work ∝ 2).
    size_t accepted = 0;
    for (size_t i = 0; i < kCap + 5000; ++i) {
        BlockHeader junk = MakeHeader(g);
        if (sel.AddHeader(junk)) ++accepted;
    }
    std::cout << "   junk accepted=" << accepted << " sideCount=" << SideCount(sel)
              << std::endl;
    assert(SideCount(sel) == kCap && "budget must be exactly full of junk");
    assert(sel.GetBestHeader()->hash == old_main_tip && "flood must not move best");

    // Now a legitimate competing chain forks at h40 (near the tip = high work) and
    // climbs to h55 (work ∝ 56 > 51). Through a FULL budget, each near-tip fork
    // header must evict a lower-work junk tip and be admitted, until it overtakes.
    uint256 reorg_tip = ExtendChain(sel, fork40_hash, 15);

    const auto* best = sel.GetBestHeader();
    assert(best != nullptr);
    std::cout << "   after reorg: best height=" << best->height << std::endl;
    assert(best->height == 55 && "reorg must win even through a full budget");
    assert(best->hash == reorg_tip && "best tip must be the reorg tip");
    // The new best chain at h50 is the reorg header, NOT the old main tip.
    assert(sel.GetHeaderAtHeight(50) != nullptr);
    assert(sel.GetHeaderAtHeight(50)->hash != old_main_tip);
    // The old main tip survives as a (now losing) side branch.
    assert(sel.GetHeader(old_main_tip) != nullptr);
    AssertBestChainIntact(sel);
    // A reorg demotes the old best-chain headers above the fork into side
    // branches WITHOUT routing them through the admission cap, so side_count may
    // transiently exceed the cap by up to the reorg depth. It stays tightly
    // bounded (and self-heals: the next over-budget side-branch admit evicts the
    // lowest-work tip), and must be nowhere near the flood size.
    std::cout << "   sideCount after reorg=" << SideCount(sel)
              << " (cap " << kCap << " + reorg slack)" << std::endl;
    assert(SideCount(sel) <= kCap + 100 && "side branches bounded (cap + reorg slack)");
    std::cout << "   ✅ reorg succeeded through a full junk budget" << std::endl;
}

// ---------------------------------------------------------------------------
// #2 static bound — pure low-work flood is bounded; best chain intact.
// ---------------------------------------------------------------------------
void TestStaticBound() {
    std::cout << "\n#2 static side-branch bound..." << std::endl;
    HeaderChainSelector sel;
    BlockHeader genesis = MakeHeader(uint256());
    assert(sel.AddHeader(genesis));
    const uint256 g = genesis.GetHash();
    const uint256 tip = ExtendChain(sel, g, 2);  // best h2
    const uint256 best_hash = sel.GetBestHeader()->hash;

    const size_t flood = kCap + 10000;
    size_t accepted = 0, refused = 0;
    for (size_t i = 0; i < flood; ++i) {
        BlockHeader junk = MakeHeader(g);
        if (sel.AddHeader(junk)) ++accepted; else ++refused;
    }
    std::cout << "   accepted=" << accepted << " refused=" << refused << std::endl;
    assert(accepted == kCap && "equal-work junk is admitted only up to the cap");
    assert(refused > 0 && "the cap must be enforced");
    assert(SideCount(sel) == kCap);
    assert(sel.GetBestHeader()->hash == best_hash && "best tip unchanged");
    assert(sel.GetHeader(tip) != nullptr);
    AssertBestChainIntact(sel);
    std::cout << "   ✅ flood bounded; active chain intact" << std::endl;
}

// ---------------------------------------------------------------------------
// #3 no-orphan / integrity after an eviction storm.
// ---------------------------------------------------------------------------
void TestNoOrphan() {
    std::cout << "\n#3 no-orphan / integrity after eviction storm..." << std::endl;
    HeaderChainSelector sel;
    std::vector<std::pair<uint256, uint256>> rec;  // (hash, prev)

    BlockHeader genesis = MakeHeader(uint256());
    assert(sel.AddHeader(genesis));
    const uint256 g = genesis.GetHash();
    rec.emplace_back(g, uint256());
    uint256 main_tip = ExtendChain(sel, g, 30, &rec);

    // A multi-level shared-prefix side branch off h10: h10 -> s1 -> s2 -> s3.
    const uint256 fork10 = sel.GetHeaderAtHeight(10)->hash;
    uint256 s_tip = ExtendChain(sel, fork10, 3, &rec);  // 3 stacked side headers

    // Storm: alternate low-work junk (off genesis) and deeper higher-work forks
    // (off h20) to drive admission + eviction churn well past the cap.
    const uint256 fork20 = sel.GetHeaderAtHeight(20)->hash;
    for (size_t i = 0; i < kCap + 3000; ++i) {
        BlockHeader low = MakeHeader(g);            // work ∝ 2
        if (sel.AddHeader(low)) rec.emplace_back(low.GetHash(), g);
        BlockHeader high = MakeHeader(fork20);      // work ∝ 22
        if (sel.AddHeader(high)) rec.emplace_back(high.GetHash(), fork20);
    }

    // Invariant: no SURVIVING header is orphaned — its parent is still stored.
    size_t survivors = 0;
    for (const auto& [h, prev] : rec) {
        if (sel.GetHeader(h) == nullptr) continue;  // evicted — fine
        ++survivors;
        if (prev.IsNull()) continue;  // genesis
        assert(sel.GetHeader(prev) != nullptr &&
               "a surviving header must never have an evicted parent (no orphans)");
    }
    AssertBestChainIntact(sel);
    assert(SideCount(sel) <= kCap && "bounded after storm");
    assert(sel.GetHeader(main_tip) != nullptr && "best tip retained");
    std::cout << "   survivors=" << survivors << " sideCount=" << SideCount(sel)
              << " — no orphans" << std::endl;
    std::cout << "   ✅ pruning never orphaned a descendant" << std::endl;
    (void)s_tip;
}

// ---------------------------------------------------------------------------
// #4 restart equivalence — bound + invariants survive LoadFromStorage.
// ---------------------------------------------------------------------------
void TestRestartEquivalence() {
    std::cout << "\n#4 restart equivalence..." << std::endl;
    const std::string db =
        (std::filesystem::temp_directory_path() / "test_sidebranch_bound_db").string();
    std::error_code ec;
    std::filesystem::remove_all(db, ec);

    uint256 g, main_tip;
    std::vector<std::pair<uint256, uint256>> rec;

    // Build + flood with persistence.
    {
        HeaderStore store(db);
        assert(store.Open());
        HeaderChainSelector sel(&store);
        BlockHeader genesis = MakeHeader(uint256());
        assert(sel.AddHeader(genesis));
        g = genesis.GetHash();
        rec.emplace_back(g, uint256());
        main_tip = ExtendChain(sel, g, 20, &rec);
        for (size_t i = 0; i < kCap + 4000; ++i) {
            BlockHeader junk = MakeHeader(g);
            if (sel.AddHeader(junk)) rec.emplace_back(junk.GetHash(), g);
        }
        assert(SideCount(sel) <= kCap);
        std::cout << "   pre-restart: count=" << sel.GetHeaderCount()
                  << " sideCount=" << SideCount(sel) << std::endl;
    }

    // Reload from the same store; LoadFromStorage must rebuild + bound.
    {
        HeaderStore store(db);
        assert(store.Open());
        HeaderChainSelector sel(&store);
        std::cout << "   post-restart: count=" << sel.GetHeaderCount()
                  << " sideCount=" << SideCount(sel) << std::endl;
        assert(sel.GetBestHeader() != nullptr);
        assert(sel.GetBestHeader()->hash == main_tip && "best tip survives restart");
        assert(SideCount(sel) <= kCap && "bound holds after reload");
        AssertBestChainIntact(sel);
        // No orphan among survivors.
        for (const auto& [h, prev] : rec) {
            if (sel.GetHeader(h) == nullptr) continue;
            if (prev.IsNull()) continue;
            assert(sel.GetHeader(prev) != nullptr && "no orphan after reload");
        }
        // The bound is still enforced on the LIVE path after reload: a new
        // equal-work junk header is refused when the budget is full.
        if (SideCount(sel) == kCap) {
            BlockHeader junk = MakeHeader(g);
            assert(!sel.AddHeader(junk) &&
                   "equal-work junk must be refused when full, post-restart");
        }
        // And a strictly higher-work fork is still admittable (work-aware).
        const uint256 fork10 = sel.GetHeaderAtHeight(10)->hash;
        BlockHeader high = MakeHeader(fork10);  // height 11 > height-1 junk
        assert(sel.AddHeader(high) &&
               "higher-work fork must still be admittable post-restart");
        AssertBestChainIntact(sel);
    }

    std::filesystem::remove_all(db, ec);
    std::cout << "   ✅ bound + invariants survive restart" << std::endl;
}

// ---------------------------------------------------------------------------
// #5 extend-the-min-tip — eviction must never free the new header's own parent.
//
// When a header E EXTENDS a side-branch tip P, and P is the lowest-work tip
// (begin() of the eviction set) under a full budget, a naive eviction would pick
// P as min_tip and free E's own parent (use-after-free), since E.chainwork is
// always > P.chainwork. The parent-slot reservation must prevent this.
// ---------------------------------------------------------------------------
void TestExtendMinTipNoUAF() {
    std::cout << "\n#5 extend the lowest-work tip (no use-after-free)..." << std::endl;
    HeaderChainSelector sel;
    BlockHeader genesis = MakeHeader(uint256());
    assert(sel.AddHeader(genesis));
    const uint256 g = genesis.GetHash();
    ExtendChain(sel, g, 30);  // best h30 (work ∝ 31)

    // P: a single low-work tip (height-1 fork off genesis, work ∝ 2) — the unique
    // lowest-work side-branch tip.
    BlockHeader pH = MakeHeader(g);
    assert(sel.AddHeader(pH));
    const uint256 p = pH.GetHash();

    // Fill the rest of the budget with strictly HIGHER-work forks (height-21 off
    // the h20 best-chain header, work ∝ 22) so begin() is unambiguously P.
    const uint256 fork20 = sel.GetHeaderAtHeight(20)->hash;
    size_t added = 1;  // P already counts as one side branch
    while (added < kCap) {
        BlockHeader hi = MakeHeader(fork20);
        if (sel.AddHeader(hi)) ++added;
    }
    assert(SideCount(sel) == kCap && "budget full");
    assert(sel.GetHeader(p) != nullptr);

    // Extend the lowest-work tip P. Naive eviction would EvictBranch(P) here and
    // free E's parent. Must not crash, must not orphan, must keep P.
    BlockHeader e = MakeHeader(p);  // child of P
    sel.AddHeader(e);               // admitted or refused — both acceptable
    assert(sel.GetHeader(p) != nullptr &&
           "extending the min-work tip must never free it (use-after-free)");
    if (sel.GetHeader(e.GetHash()) != nullptr) {
        assert(sel.GetHeader(e.prev_block_hash) != nullptr &&
               "if admitted, the new header must not be orphaned");
    }
    AssertBestChainIntact(sel);
    std::cout << "   ✅ extending the min tip never freed its parent" << std::endl;
}

}  // namespace

int main() {
    SelectParams(Chain::REGTEST);
    std::cout << "=== Bounded side-branch header storage (4d-2 / #181) ===" << std::endl;
    TestReorgAfterFlood();
    TestStaticBound();
    TestNoOrphan();
    TestExtendMinTipNoUAF();
    TestRestartEquivalence();
    std::cout << "\n✅ ALL side-branch eviction tests passed" << std::endl;
    return 0;
}
