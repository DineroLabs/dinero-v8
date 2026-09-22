/*
 * test_forward_sync_body_gap — regression for #806 (forward-sync REORG ABORT ladder).
 *
 * Reproduced on a 2-vCPU regtest peer sync: block 62 arrived before 54..61 and became
 * the best candidate. Header-only entries can carry BLOCK_VALID_CHAIN, so candidate
 * eligibility accepted 62 (its own body present, ancestors "connected" by flag), and the
 * body-gap guard in ActivateBestChain only runs when a branch is being disconnected.
 * The forward connect walk then tried to read block 54, failed, and entered the
 * REORG ABORT backoff ladder 23 times.
 *
 * The fix: a forward extension connects only the contiguous prefix of bodies present
 * (ContiguousBodyPrefix over the connect path, BLOCK_HAVE_DATA only) and defers the
 * rest. This test hand-builds the exact graph and shows (a) both existing predicates
 * are fooled by the flag, so they cannot be the guard, and (b) the prefix rule yields
 * the right plan in every arrangement of missing bodies.
 */
#include <algorithm>
#include <iostream>
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

static CBlockIndex* mk(uint32_t height, uint32_t status, CBlockIndex* parent, const std::string& tag) {
    BlockHeader hdr;
    auto* idx = new CBlockIndex(hdr, height);
    std::string hx;
    for (char c : tag) { char buf[3]; snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c)); hx += buf; }
    while (hx.size() < 64) hx += "0";
    idx->hash = dinero::uint256::FromHexUnsafe(hx.substr(0, 64));
    idx->height = height; idx->status = status; idx->pprev = parent;
    return idx;
}

// Mirror of ChainstateService::IsReorgCandidateEligible (the flag shortcut included).
static bool eligible(const CBlockIndex* b) {
    if (!b) return false;
    if (b->status & (dinero::BLOCK_FAILED_VALID | dinero::BLOCK_FAILED_CHILD)) return false;
    if (!(b->status & dinero::BLOCK_HAVE_DATA)) return false;
    if (b->status & dinero::BLOCK_VALID_CHAIN) return true;
    return dinero::BranchHasDataToConnectedBase(b);
}

// Mirror of the connect-path construction in ActivateBestChain (fork+1 .. candidate).
static std::vector<CBlockIndex*> connect_path(CBlockIndex* candidate, CBlockIndex* fork) {
    std::vector<CBlockIndex*> p;
    for (CBlockIndex* w = candidate; w && w != fork; w = w->pprev) p.push_back(w);
    std::reverse(p.begin(), p.end());
    return p;
}

int main() {
    using dinero::BLOCK_HAVE_DATA; using dinero::BLOCK_VALID_CHAIN; using dinero::BLOCK_VALID_SCRIPTS;
    const uint32_t CONNECTED = BLOCK_VALID_CHAIN | BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
    const uint32_t HEADER_ONLY_FLAGGED = BLOCK_VALID_CHAIN;            // the #806 state: flag, no body
    const uint32_t BODY_PRESENT_FLAGGED = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

    // Active chain: genesis .. 53 connected.
    CBlockIndex* g = mk(0, CONNECTED, nullptr, "g");
    CBlockIndex* prev = g;
    for (uint32_t h = 1; h <= 53; ++h) prev = mk(h, CONNECTED, prev, "a" + std::to_string(h));
    CBlockIndex* tip53 = prev;

    // 54..61 header-only but stamped BLOCK_VALID_CHAIN; 62 has its body.
    std::vector<CBlockIndex*> chain;
    prev = tip53;
    for (uint32_t h = 54; h <= 61; ++h) { prev = mk(h, HEADER_ONLY_FLAGGED, prev, "h" + std::to_string(h)); chain.push_back(prev); }
    CBlockIndex* b62 = mk(62, BODY_PRESENT_FLAGGED, prev, "h62"); chain.push_back(b62);

    std::cout << "Existing predicates on the #806 graph (documenting the trap):\n";
    check(eligible(b62), "candidate eligibility ACCEPTS 62 (own body + BLOCK_VALID_CHAIN shortcut)");
    check(dinero::BranchHasDataToConnectedBase(b62), "BranchHasDataToConnectedBase(62) is TRUE (stops at 61's BLOCK_VALID_CHAIN, never sees the gap)");
    check(!dinero::BranchHasDataToFork(b62, tip53), "BranchHasDataToFork(62, tip53) is FALSE (HAVE_DATA-based) - but only consulted when disconnecting");

    std::cout << "Forward-extension prefix rule (the fix):\n";
    auto path = connect_path(b62, tip53);
    check(path.size() == 9 && path.front()->height == 54 && path.back()->height == 62, "connect path is 54..62");
    check(dinero::ContiguousBodyPrefix(path) == 0, "first body missing at 54 -> prefix 0: defer, connect nothing, no abort");

    // Bodies for 54..57 arrive: connect exactly those four, defer 58..62.
    for (CBlockIndex* b : chain) if (b->height <= 57) b->status |= BLOCK_HAVE_DATA;
    check(dinero::ContiguousBodyPrefix(path) == 4, "bodies 54..57 present -> prefix 4 (connect to 57, defer 58..62)");
    {
        auto trimmed = path; trimmed.resize(dinero::ContiguousBodyPrefix(path));
        bool all_have_data = true; for (auto* b : trimmed) all_have_data = all_have_data && (b->status & BLOCK_HAVE_DATA);
        check(all_have_data && trimmed.back()->height == 57, "trimmed plan contains only blocks with bodies and ends at 57");
    }

    // A later body (60) arriving before 58/59 must not extend the prefix past the gap.
    chain[6]->status |= BLOCK_HAVE_DATA;   // height 60
    check(dinero::ContiguousBodyPrefix(path) == 4, "body 60 present but 58 missing -> prefix still 4");

    // All bodies present -> whole path connectable.
    for (CBlockIndex* b : chain) b->status |= BLOCK_HAVE_DATA;
    check(dinero::ContiguousBodyPrefix(path) == 9, "all bodies present -> prefix 9 (full connect)");

    // Empty path and null entries are handled.
    check(dinero::ContiguousBodyPrefix({}) == 0, "empty path -> 0");
    std::vector<CBlockIndex*> with_null{path[0], nullptr, path[2]};
    check(dinero::ContiguousBodyPrefix(with_null) == 1, "null entry terminates the prefix");

    std::cout << (g_failures ? "FAILED" : "ALL PASSED") << " (" << g_failures << " failure(s))\n";
    return g_failures ? 1 : 0;
}
