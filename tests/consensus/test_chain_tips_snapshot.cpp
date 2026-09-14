/*
 * test_chain_tips_snapshot — unit gate for issue #741.
 *
 * getchaintips used to be fed by GetCandidateTipsSnapshot(), which only holds
 * tips that can still BECOME active: a tip is erased from g_candidates the
 * moment it is activated, and never re-inserted when a reorg abandons it. So
 * after any reorg the RPC listed exactly one tip and the abandoned branch
 * vanished (Bitcoin Core reports it as "valid-fork").
 *
 * GetChainTipsSnapshot() enumerates every known tip (a block-index entry with
 * no child) whose fork point with the active chain lies within a bounded
 * window, and classifies each with Core's status vocabulary. This test drives
 * it over a hand-built g_block_index (deterministic, no daemon):
 *
 *   genesis(0) — 1 — 2 — 3 — 4 — 5 — 6 — 7 — 8 — 9 — 10   (active, tip = 10)
 *                                     \       \
 *                                      8' — 9'  10''            side branches
 *                          \
 *                           6*                                  invalid
 *
 *   8'–9'  : fully validated bodies, abandoned by a reorg  -> valid-fork, branchlen 2
 *   10''   : header only (no body)                          -> headers-only, branchlen 1
 *   6*     : body present, marked BLOCK_FAILED_VALID        -> invalid, branchlen 1
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"

using dinero::CBlockIndex;
using dinero::ChainTipEntry;
using dinero::ChainTipStatus;
using dinero::GetChainTipsSnapshot;

namespace {

// Status a block carries after BlockAcceptor fully validated and stored it
// (block_acceptor.cpp sets exactly these on acceptance).
constexpr uint32_t kFullyValidated = dinero::BLOCK_VALID_HEADER |
                                     dinero::BLOCK_VALID_TREE |
                                     dinero::BLOCK_VALID_TRANSACTIONS |
                                     dinero::BLOCK_VALID_CHAIN |
                                     dinero::BLOCK_VALID_SCRIPTS |
                                     dinero::BLOCK_HAVE_DATA;
constexpr uint32_t kHeaderOnly = dinero::BLOCK_VALID_HEADER;
constexpr uint32_t kBodyNotValidated = dinero::BLOCK_VALID_HEADER |
                                       dinero::BLOCK_VALID_TREE |
                                       dinero::BLOCK_HAVE_DATA;

class ChainTipsSnapshotTest : public ::testing::Test {
protected:
    void SetUp() override { Reset(); }
    void TearDown() override { Reset(); }

    static void Reset() {
        dinero::g_block_index.clear();
        dinero::g_candidates.clear();
        dinero::g_orphan_pool.clear();
    }

    // Insert a block-index entry with a unique hash derived from `tag`, linked
    // under `parent`. Chainwork is height-monotonic so ByWorkThenHash ordering
    // is deterministic; the exact value is irrelevant to the query under test.
    static CBlockIndex* Add(const std::string& tag, uint32_t height, uint32_t status,
                            CBlockIndex* parent) {
        auto idx = std::make_unique<CBlockIndex>();
        std::string hx;
        for (char c : tag) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c));
            hx += buf;
        }
        while (hx.size() < 64) hx += "0";
        idx->hash = dinero::uint256::FromHexUnsafe(hx.substr(0, 64));
        idx->height = height;
        idx->status = status;
        idx->pprev = parent;
        if (parent) {
            idx->prev_hash = parent->hash;
            parent->children.push_back(idx.get());
        }
        char work[65];
        snprintf(work, sizeof(work), "%064x", static_cast<unsigned>(height + 1));
        idx->chainwork = work;
        CBlockIndex* raw = idx.get();
        dinero::g_block_index[raw->hash] = std::move(idx);
        return raw;
    }

    // Build the main chain genesis..`tip_height`, all fully validated.
    std::vector<CBlockIndex*> BuildMainChain(uint32_t tip_height) {
        std::vector<CBlockIndex*> chain;
        CBlockIndex* prev = nullptr;
        for (uint32_t h = 0; h <= tip_height; ++h) {
            prev = Add("main" + std::to_string(h), h, kFullyValidated, prev);
            chain.push_back(prev);
        }
        return chain;
    }

    static const ChainTipEntry* Find(const std::vector<ChainTipEntry>& tips,
                                     const CBlockIndex* idx) {
        auto it = std::find_if(tips.begin(), tips.end(),
                               [idx](const ChainTipEntry& e) { return e.tip == idx; });
        return it == tips.end() ? nullptr : &*it;
    }
};

}  // namespace

TEST_F(ChainTipsSnapshotTest, ReportsEveryTipWithCoreStatusesAndBranchLengths) {
    auto main = BuildMainChain(10);
    CBlockIndex* active = main[10];

    // Abandoned 2-block branch off height 7 (was active once; fully validated).
    CBlockIndex* s8 = Add("side8", 8, kFullyValidated, main[7]);
    CBlockIndex* s9 = Add("side9", 9, kFullyValidated, s8);
    // Header-only 1-block branch off height 9.
    CBlockIndex* h10 = Add("hdr10", 10, kHeaderOnly, main[9]);
    // Invalid 1-block branch off height 5.
    CBlockIndex* bad6 = Add("bad6", 6, kFullyValidated | dinero::BLOCK_FAILED_VALID, main[5]);

    // None of these is a candidate: mirrors the post-reorg reality that
    // motivated #741 (the candidate set is empty once the best tip is active).
    ASSERT_TRUE(dinero::g_candidates.empty());

    const auto tips = GetChainTipsSnapshot(active);
    ASSERT_EQ(tips.size(), 4u) << "every tip must be reported, not only candidates";

    const ChainTipEntry* e = Find(tips, active);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::Active);
    EXPECT_EQ(e->branchlen, 0u);

    e = Find(tips, s9);
    ASSERT_NE(e, nullptr) << "abandoned branch tip missing (the #741 defect)";
    EXPECT_EQ(e->status, ChainTipStatus::ValidFork);
    EXPECT_EQ(e->branchlen, 2u);

    e = Find(tips, h10);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::HeadersOnly);
    EXPECT_EQ(e->branchlen, 1u);

    e = Find(tips, bad6);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::Invalid);
    EXPECT_EQ(e->branchlen, 1u);

    // Non-tips (interior blocks) never appear.
    EXPECT_EQ(Find(tips, s8), nullptr);
    EXPECT_EQ(Find(tips, main[7]), nullptr);
}

TEST_F(ChainTipsSnapshotTest, BodyPresentButNotValidatedIsValidHeaders) {
    auto main = BuildMainChain(4);
    CBlockIndex* v = Add("unval", 4, kBodyNotValidated, main[3]);

    const auto tips = GetChainTipsSnapshot(main[4]);
    const ChainTipEntry* e = Find(tips, v);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::ValidHeaders);
    EXPECT_EQ(e->branchlen, 1u);
}

TEST_F(ChainTipsSnapshotTest, FailedChildFlagIsInvalid) {
    auto main = BuildMainChain(3);
    CBlockIndex* c = Add("badchild", 4, kFullyValidated | dinero::BLOCK_FAILED_CHILD, main[3]);

    const auto tips = GetChainTipsSnapshot(main[3]);
    const ChainTipEntry* e = Find(tips, c);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::Invalid);
    EXPECT_EQ(e->branchlen, 1u);
}

TEST_F(ChainTipsSnapshotTest, ActiveOnlyWhenNoForks) {
    auto main = BuildMainChain(6);
    const auto tips = GetChainTipsSnapshot(main[6]);
    ASSERT_EQ(tips.size(), 1u);
    EXPECT_EQ(tips[0].tip, main[6]);
    EXPECT_EQ(tips[0].status, ChainTipStatus::Active);
    EXPECT_EQ(tips[0].branchlen, 0u);
}

TEST_F(ChainTipsSnapshotTest, ActiveTipIsReportedEvenWithPendingHeaderChild) {
    // Header-first sync: the next header is indexed before its body arrives, so
    // the active tip has a child. Core still reports the active tip.
    auto main = BuildMainChain(5);
    CBlockIndex* pending = Add("pending6", 6, kHeaderOnly, main[5]);

    const auto tips = GetChainTipsSnapshot(main[5]);
    ASSERT_EQ(tips.size(), 2u);
    const ChainTipEntry* e = Find(tips, main[5]);
    ASSERT_NE(e, nullptr) << "active tip must always be reported";
    EXPECT_EQ(e->status, ChainTipStatus::Active);
    EXPECT_EQ(e->branchlen, 0u);
    e = Find(tips, pending);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::HeadersOnly);
    EXPECT_EQ(e->branchlen, 1u);
}

TEST_F(ChainTipsSnapshotTest, ForkPointDeeperThanWindowIsExcluded) {
    auto main = BuildMainChain(30);
    CBlockIndex* deep = Add("deep", 3, kFullyValidated, main[2]);      // fork @2, 28 below tip
    CBlockIndex* recent = Add("recent", 27, kFullyValidated, main[26]); // fork @26, 4 below tip

    const auto tips = GetChainTipsSnapshot(main[30], /*max_fork_depth=*/10);
    EXPECT_NE(Find(tips, recent), nullptr);
    EXPECT_EQ(Find(tips, deep), nullptr) << "fork point outside the window must be skipped";
    EXPECT_NE(Find(tips, main[30]), nullptr) << "active tip is always reported";

    // A wider window includes it.
    const auto all = GetChainTipsSnapshot(main[30], /*max_fork_depth=*/100);
    const ChainTipEntry* e = Find(all, deep);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, ChainTipStatus::ValidFork);
    EXPECT_EQ(e->branchlen, 1u);
}

TEST_F(ChainTipsSnapshotTest, IsReadOnlyAndLeavesCandidateSetUntouched) {
    auto main = BuildMainChain(5);
    CBlockIndex* side = Add("cand", 5, kFullyValidated, main[4]);
    dinero::g_candidates.insert(side);

    const auto before = dinero::GetCandidateTipsSnapshot();
    (void)GetChainTipsSnapshot(main[5]);
    const auto after = dinero::GetCandidateTipsSnapshot();
    EXPECT_EQ(before, after) << "getchaintips must not alter ActivateBestChain's candidate set";
    EXPECT_EQ(main[4]->children.size(), 2u);
}

TEST_F(ChainTipsSnapshotTest, StatusNamesMatchBitcoinCore) {
    EXPECT_STREQ(dinero::ChainTipStatusName(ChainTipStatus::Active), "active");
    EXPECT_STREQ(dinero::ChainTipStatusName(ChainTipStatus::ValidFork), "valid-fork");
    EXPECT_STREQ(dinero::ChainTipStatusName(ChainTipStatus::ValidHeaders), "valid-headers");
    EXPECT_STREQ(dinero::ChainTipStatusName(ChainTipStatus::HeadersOnly), "headers-only");
    EXPECT_STREQ(dinero::ChainTipStatusName(ChainTipStatus::Invalid), "invalid");
}
