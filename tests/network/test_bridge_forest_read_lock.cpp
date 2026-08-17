// Regression test for issue #578: BridgeNode's live-forest reads must go
// through the owner's SHARED forest lock, so proof serving on P2P threads
// cannot race guarded forest writes on the activation side (the read-during-
// free UAF class fixed repo-wide in #570; TSan caught this remaining site).
//
// What it proves, deterministically:
//   1. EXCLUSION: while a writer holds the exclusive lock inside
//      MutateForestGuarded(), BridgeNode::GetCurrentForestCommitment() must
//      BLOCK until the mutation completes — the read returns only after the
//      writer's in_mutation flag has been cleared, and it observes the
//      post-mutation forest.
//   2. OWNERLESS COMPAT: a BridgeNode constructed without an owner (tests,
//      standalone forests) still reads directly, unchanged.
//
// Neuter check: constructing the bridge with owner=nullptr in part 1 makes the
// read return mid-mutation (in_mutation still true) and the test FAILS — the
// exclusion assertion has teeth.
//
// Gate is the process exit code — never assert() (a no-op under NDEBUG).
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "network/bridge_node.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace dinero;
using namespace dinero::consensus;

namespace {
UtreexoHash mkhash(uint64_t seed) {
    UtreexoHash h(32);
    for (int i = 0; i < 32; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        h[i] = static_cast<uint8_t>(seed >> 33);
    }
    return h;
}
UtreexoForest makeForest(std::size_t leaves, uint64_t salt) {
    UtreexoForest f;
    for (std::size_t i = 0; i < leaves; ++i) f.add(mkhash(salt * 1000003ULL + i));
    return f;
}

int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK FAILED: %s\n  %s\n  at %s:%d\n",       \
                         #cond, msg, __FILE__, __LINE__);                      \
        }                                                                      \
    } while (0)
}  // namespace

int main() {
    // ── Part 1: exclusion through the owner wiring ──────────────────────────
    ConsensusUTXOSet set;
    set.ReplaceForestGuarded(makeForest(64, 1));

    // ConsensusUTXOSet IS-A IUTXOProvider; non-owning aliasing shared_ptr,
    // same construction daemon_app uses.
    auto provider = std::shared_ptr<IUTXOProvider>(
        std::shared_ptr<void>{}, static_cast<IUTXOProvider*>(&set));
    network::BridgeNode bridge(provider, &set.GetForest(),
                               /*proof_cache=*/nullptr, /*chain_db=*/nullptr,
                               /*block_storage=*/nullptr, /*owner=*/&set);

    const UtreexoHash replacement_commitment = makeForest(96, 2).getCommitment();

    std::atomic<bool> in_mutation{false};
    std::atomic<bool> reader_started{false};
    std::thread writer([&] {
        set.MutateForestGuarded([&](UtreexoForest& f) {
            in_mutation.store(true);
            // Give the reader ample time to start and hit the lock. 300ms is
            // enormous next to a lock acquisition; if the reader does NOT
            // block, it returns well inside this window with in_mutation=true.
            while (!reader_started.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            f = makeForest(96, 2);
            in_mutation.store(false);
        });
    });

    while (!in_mutation.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader_started.store(true);
    const UtreexoHash seen = bridge.GetCurrentForestCommitment();
    const bool mutation_running_at_return = in_mutation.load();
    writer.join();

    CHECK(!mutation_running_at_return,
          "read returned while the exclusive mutation was still running — "
          "BridgeNode is not honoring the owner's shared lock");
    CHECK(seen == replacement_commitment,
          "read did not observe the post-mutation forest");

    // ── Part 2: ownerless bridge still reads directly ───────────────────────
    UtreexoForest standalone = makeForest(32, 3);
    auto provider2 = std::shared_ptr<IUTXOProvider>(
        std::shared_ptr<void>{}, static_cast<IUTXOProvider*>(&set));
    network::BridgeNode lone(provider2, &standalone);
    CHECK(lone.GetCurrentForestCommitment() == standalone.getCommitment(),
          "ownerless bridge must read its standalone forest unchanged");

    if (g_failures == 0) {
        std::printf("PASS: BridgeNode forest reads honor the owner's shared lock "
                    "(blocked across a 300ms exclusive mutation, observed the "
                    "post-mutation forest); ownerless path unchanged\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
