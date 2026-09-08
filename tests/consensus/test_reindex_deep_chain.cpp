// Copyright (c) 2026 Dinero Labs.
//
// Regression test for issue #708: reindex must not overflow the stack on a
// chain deeper than the process stack can hold.
//
// The bug: SelectCanonicalChain resolved parent links by RECURSING once per
// block, so the first descent from tip to genesis was one unbroken recursion
// as deep as the chain. On mainnet (~107,750 blocks) that segfaulted against
// the default 8 MB stack in reindex step 3, before any work was done —
// `--reindex-chainstate` was simply unusable, on a documented recovery path.
//
// WHY THIS TEST IS SHAPED THIS WAY
//
// A test that reindexes a short chain passes with or without the fix and gates
// nothing. Two things make this one discriminate:
//
//   1. It runs the walk on a thread with a DELIBERATELY SMALL stack
//      (kThreadStackBytes), so the depth needed to overflow is reachable in a
//      unit test instead of requiring a 107k-block datadir.
//   2. It asserts NORMAL COMPLETION — the returned chain's length AND its
//      genesis-to-tip ordering — not merely that the process survived. A
//      process-survival check would pass on a function that returned an error
//      or a truncated chain.
//
// With the recursive version this dies by SIGSEGV inside the walk. With the
// iterative version the thread's stack usage is O(1) in chain height and the
// chain comes back intact.
//
// Margin: kChainDepth frames must not fit in kThreadStackBytes even if a frame
// were an implausible 16 bytes (20,000 x 16 = 320 KB > 256 KB). Real frames
// here carry a std::string and an arith_uint256, so the true overrun is an
// order of magnitude larger. The iterative version is unaffected by either
// number, so the margin is one-sided and cannot flake into a false PASS.
//
// RECORD ORDER IS LOAD-BEARING -- the first version of this test got it wrong
// and PASSED against the recursive code, gating nothing.
//
// The outer driver is `for (i = 0..n) resolve(i)`. With records in ascending
// height order, resolve(0) is the genesis child and terminates at depth 1; by
// the time record i is reached its parent is already memoised, so the walk
// never nests more than two frames deep no matter how long the chain is. A
// 20,000-block ascending chain therefore completes happily on a 256 KB stack
// WITH THE BUG STILL PRESENT.
//
// Deep recursion needs a record resolved BEFORE its ancestors, which is what a
// real datadir looks like: blk*.dat is in ARRIVAL order, and a node that
// bootstrapped from an AssumeUTXO snapshot writes post-base blocks first and
// backfills the pre-base history afterwards -- tip long before genesis. So the
// records here are built tip-first, which makes resolve(0) descend the entire
// chain in one unbroken recursion, exactly as it did on the datadir that
// crashed.

#include <pthread.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "consensus/chainparams.h"
#include "consensus/reindexer_detail.h"
#include "primitives/uint256.h"

using dinero::uint256;
using dinero::consensus::reindex_detail::DiskBlockRecord;
using dinero::consensus::reindex_detail::SelectCanonicalChain;

namespace {

constexpr size_t kChainDepth = 20000;
constexpr size_t kThreadStackBytes = 256 * 1024;

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "[PASS] " << what << "\n";
    } else {
        std::cout << "[FAIL] " << what << "\n";
        ++g_failures;
    }
}

// Distinct, collision-free synthetic hashes that cannot equal the genesis hash
// (byte 0 is a tag the genesis hash does not carry at that position).
uint256 SyntheticHash(uint32_t i) {
    uint256 h;
    std::memset(h.data, 0, sizeof(h.data));
    h.data[0] = 0xA1;
    std::memcpy(h.data + 1, &i, sizeof(i));
    return h;
}

struct WalkOutcome {
    bool ran = false;
    bool ok = false;
    size_t length = 0;
    bool ordered_genesis_to_tip = false;
};

struct WalkArgs {
    const std::vector<DiskBlockRecord>* records;
    WalkOutcome out;
};

void* RunWalk(void* raw) {
    auto* args = static_cast<WalkArgs*>(raw);
    args->out.ran = true;
    auto result = SelectCanonicalChain(*args->records, std::string());
    if (!result.ok()) {
        return nullptr;
    }
    args->out.ok = true;
    const auto& chain = result.value();
    args->out.length = chain.size();
    // Genesis-to-tip order over tip-first records means the returned indices
    // must run backwards: position i holds record (kChainDepth - 1 - i).
    args->out.ordered_genesis_to_tip = (chain.size() == kChainDepth);
    for (size_t i = 0; i < chain.size() && args->out.ordered_genesis_to_tip; ++i) {
        if (chain[i] != kChainDepth - 1 - i) {
            args->out.ordered_genesis_to_tip = false;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    dinero::SelectParams(dinero::Chain::MAINNET);
    const auto& params = dinero::Params();

    // The chain must actually anchor on genesis, or every record resolves as
    // unconnected and the walk returns NotFound without ever going deep --
    // which would make this test vacuous.
    const uint256 genesis = uint256::FromHexUnsafe(params.genesis_hash);
    check(genesis.GetHex() == params.genesis_hash,
          "genesis hash round-trips through uint256 (test would be vacuous otherwise)");

    // Record r holds the block at height (kChainDepth - r): index 0 is the TIP
    // and the last index is genesis's child. See the note above on why order
    // decides whether this test can fail at all.
    std::vector<DiskBlockRecord> records;
    records.reserve(kChainDepth);
    for (size_t r = 0; r < kChainDepth; ++r) {
        const uint32_t height = static_cast<uint32_t>(kChainDepth - r);
        DiskBlockRecord rec;
        rec.hash = SyntheticHash(height);
        rec.prev_hash = (height == 1) ? genesis : SyntheticHash(height - 1);
        rec.block.header.difficulty = params.genesis.nBits;
        records.push_back(std::move(rec));
    }
    std::cout << "[INFO] built a " << kChainDepth
              << "-block chain, TIP FIRST; walking it on a "
              << (kThreadStackBytes / 1024) << " KB stack\n";

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0 ||
        pthread_attr_setstacksize(&attr, kThreadStackBytes) != 0) {
        std::cout << "[FAIL] could not constrain the thread stack\n";
        return 1;
    }

    WalkArgs args{&records, {}};
    pthread_t tid;
    if (pthread_create(&tid, &attr, RunWalk, &args) != 0) {
        std::cout << "[FAIL] could not start the constrained-stack thread\n";
        return 1;
    }
    pthread_join(tid, nullptr);
    pthread_attr_destroy(&attr);

    check(args.out.ran, "the walk was entered");
    check(args.out.ok, "SelectCanonicalChain returned a chain (no stack overflow, no error)");
    check(args.out.length == kChainDepth,
          "the whole chain came back: got " + std::to_string(args.out.length) +
              " of " + std::to_string(kChainDepth));
    check(args.out.ordered_genesis_to_tip,
          "traversal order preserved: chain is genesis-to-tip, record i at position i");

    if (g_failures == 0) {
        std::cout << "\n✅ deep-chain reindex walk completes on a constrained stack\n";
        return 0;
    }
    std::cout << "\n❌ " << g_failures << " check(s) failed\n";
    return 1;
}
