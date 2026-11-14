/**
 * HeaderIndexEntry skip pointers.
 *
 * GetAncestor used to walk parent links one at a time. Phone history
 * verification calls it (through GetAncestorHashByHash and the ASERT ancestry
 * lookup) for every appended block, from about height 150k down to mainnet
 * checkpoints and block 1, which made verification O(blocks * height).
 *
 * Skip pointers (the Bitcoin Core construction) make each lookup logarithmic
 * with identical results. These checks require:
 *   - every built entry at height >= 2 links to its exact skip height;
 *   - GetAncestor agrees with a reference parent walk for random heights on a
 *     long chain, across forks, after side-branch eviction and after reload
 *     from storage;
 *   - copies never carry a skip link (they fall back to the parent walk);
 *   - 100k deep lookups on a 300k chain finish far inside a bound a linear
 *     walk cannot meet.
 * Failures exit non-zero (no assert(), which NDEBUG would disable).
 */
#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

uint32_t g_time = 1'000'000;
BlockHeader MakeHeader(const uint256& prev) {
    BlockHeader h{};
    h.version = 1;
    h.prev_block_hash = prev;
    h.timestamp = ++g_time;
    h.difficulty = 0x1d00ffff;
    h.nonce = 1;
    return h;
}

// Reference: the old linear parent walk.
const HeaderIndexEntry* LinearAncestor(const HeaderIndexEntry* e, uint32_t height) {
    while (e && e->height > height) e = e->parent;
    return (e && e->height == height) ? e : nullptr;
}

void TestDirectChain() {
    std::cout << "direct 300k chain..." << std::endl;
    constexpr uint32_t kLength = 300'000;
    std::vector<std::unique_ptr<HeaderIndexEntry>> chain;
    chain.reserve(kLength);
    const HeaderIndexEntry* parent = nullptr;
    for (uint32_t i = 0; i < kLength; ++i) {
        chain.push_back(std::make_unique<HeaderIndexEntry>(
            MakeHeader(parent ? parent->hash : uint256()), parent));
        parent = chain.back().get();
    }
    for (uint32_t h = 0; h < kLength; ++h) {
        const auto* e = chain[h].get();
        if (h < 2) {
            Check(e->Skip() == nullptr || e->Skip()->height == HeaderIndexEntry::SkipHeight(h),
                  "low height skip " + std::to_string(h));
            continue;
        }
        Check(e->Skip() != nullptr && e->Skip()->height == HeaderIndexEntry::SkipHeight(h) &&
              e->Skip() == chain[e->Skip()->height].get(),
              "skip link at height " + std::to_string(h));
    }
    std::mt19937 rng(7);
    for (int i = 0; i < 20'000; ++i) {
        const uint32_t from = std::uniform_int_distribution<uint32_t>(0, kLength - 1)(rng);
        const uint32_t to = std::uniform_int_distribution<uint32_t>(0, from)(rng);
        const auto* got = chain[from]->GetAncestor(to);
        Check(got == chain[to].get(), "ancestor " + std::to_string(from) + "->" + std::to_string(to));
        if (i < 200) Check(got == LinearAncestor(chain[from].get(), to), "linear parity");
    }
    Check(chain.back()->GetAncestor(kLength) == nullptr, "ancestor above height");
    Check(chain.back()->GetAncestor(kLength - 1) == chain.back().get(), "ancestor at own height");
    Check(chain.back()->GetAncestor(0) == chain.front().get(), "ancestor at genesis");

    // Copies never carry a skip link and still answer through the parent walk.
    HeaderIndexEntry copy = *chain[kLength / 2];
    Check(copy.Skip() == nullptr, "copy has no skip link");
    Check(copy.GetAncestor(17) == chain[17].get(), "copy falls back to parent walk");
    HeaderIndexEntry assigned;
    assigned = *chain[kLength / 3];
    Check(assigned.Skip() == nullptr, "assigned copy has no skip link");

    // 100k deep lookups. A linear walk needs ~1.5e10 parent hops here.
    const auto start = std::chrono::steady_clock::now();
    size_t found = 0;
    for (int i = 0; i < 100'000; ++i) {
        const uint32_t to = std::uniform_int_distribution<uint32_t>(0, 13'000)(rng);
        found += chain.back()->GetAncestor(to) == chain[to].get();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(found == 100'000, "deep lookups resolve");
    Check(elapsed < std::chrono::seconds(5), "100k deep lookups took " +
          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms");
}

// Reference ancestor through the selector's own value API (prev_hash links).
std::optional<uint256> ReferenceAncestor(const HeaderChainSelector& sel, uint256 hash, uint32_t height) {
    for (;;) {
        const auto e = sel.GetHeaderValue(hash);
        if (!e || e->height < height) return std::nullopt;
        if (e->height == height) return e->hash;
        hash = e->prev_hash;
    }
}

void CheckSelectorParity(const HeaderChainSelector& sel, const std::vector<uint256>& tips,
                         const std::string& label) {
    std::mt19937 rng(11);
    for (const auto& tip : tips) {
        const auto value = sel.GetHeaderValue(tip);
        if (!value) continue;
        for (int i = 0; i < 300; ++i) {
            const uint32_t to = std::uniform_int_distribution<uint32_t>(0, value->height)(rng);
            uint256 got;uint32_t anchor = 0;
            const bool ok = sel.GetAncestorHashByHash(tip, to, got, anchor);
            const auto want = ReferenceAncestor(sel, tip, to);
            Check(ok && want && got == *want && anchor == value->height,
                  label + " parity at height " + std::to_string(to));
        }
    }
}

uint256 Extend(HeaderChainSelector& sel, uint256 tip, int n) {
    for (int i = 0; i < n; ++i) {
        const auto h = MakeHeader(tip);
        Check(sel.AddHeader(h), "extend chain");
        tip = h.GetHash();
    }
    return tip;
}

void TestSelectorForksEvictionAndReload() {
    std::cout << "selector forks, eviction, reload..." << std::endl;
    const std::string db = (std::filesystem::temp_directory_path() / "test_header_skip_pointers_db").string();
    std::error_code ec;
    std::filesystem::remove_all(db, ec);
    std::vector<uint256> tips;
    {
        HeaderStore store(db);
        Check(store.Open(), "open store");
        HeaderChainSelector sel(&store);
        const auto genesis = MakeHeader(uint256());
        Check(sel.AddHeader(genesis), "genesis");
        const uint256 main_tip = Extend(sel, genesis.GetHash(), 3000);
        tips.push_back(main_tip);
        // Forks at several depths, then a long competing branch that wins.
        for (uint32_t fork_height : {5u, 700u, 2048u, 2999u}) {
            const auto base = sel.GetHeaderAtHeightValue(fork_height);
            Check(base.has_value(), "fork base");
            if (base) tips.push_back(Extend(sel, base->hash, 3));
        }
        const auto base = sel.GetHeaderAtHeightValue(1500);
        if (base) tips.push_back(Extend(sel, base->hash, 1600));
        CheckSelectorParity(sel, tips, "live");
        // Fill the side-branch budget so EvictBranch prunes low-work forks.
        const auto g = sel.GetHeaderAtHeightValue(0);
        for (int i = 0; i < 10'500 && g; ++i) {
            const auto junk = MakeHeader(g->hash);
            (void)sel.AddHeader(junk);
        }
        CheckSelectorParity(sel, tips, "after eviction");
        uint256 fork;
        Check(sel.FindForkPointHash(tips.front(), tips.back(), fork) &&
              fork == sel.GetHeaderAtHeightValue(1500)->hash, "fork point");
    }
    {
        HeaderStore store(db);
        Check(store.Open(), "reopen store");
        HeaderChainSelector sel(&store);
        CheckSelectorParity(sel, tips, "after reload");
        uint256 fork;
        Check(sel.FindForkPointHash(tips.front(), tips.back(), fork) &&
              fork == sel.GetHeaderAtHeightValue(1500)->hash, "fork point after reload");
    }
    std::filesystem::remove_all(db, ec);
}

}  // namespace

int main() {
    SelectParams(Chain::REGTEST);
    TestDirectChain();
    TestSelectorForksEvictionAndReload();
    if (g_failures) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS header skip pointers" << std::endl;
    return 0;
}
