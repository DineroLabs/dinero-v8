// Concurrency stress harness for the Utreexo forest read/write lock (PR: forest UAF).
//
// Drives the REAL production accessors on a real ConsensusUTXOSet:
//   readers  -> SnapshotForestRoots/Commitment/LeafCount (shared) and the
//               "clone under LockForestShared()" structural pattern used by
//               block_assembler / ExportSnapshot / methods_utreexo_batch.
//   writers  -> ReplaceForestGuarded() (frees the old forest's buffers) and
//               RemoveLastNLeavesGuarded() (in-place shrink).
// Many reader and writer threads run concurrently for a fixed duration.
//
// What it proves (and what it does NOT):
//   * DEADLOCK-FREEDOM of the forest lock under heavy contention — a watchdog
//     thread hard-exits nonzero if global progress stalls (this is the primary
//     risk of the lock change).
//   * A broken lock would let a reader clone/read a forest a writer is freeing;
//     under load that typically faults (and, built with -fsanitize=thread or
//     address on a host where those work, reports the race deterministically).
//   * It does NOT exercise forest_mutex_ vs activation_mutex_ / ChainDB lock
//     ORDERING — that requires the live-node harness (see
//     tools/forest-lock-stress/run_regtest_stress.sh).
//
// Gate is the process exit code (0 pass, 2 deadlock) — never assert() (a no-op
// under NDEBUG). Args: [readers] [writers] [seconds]  (defaults 8 2 3).
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace dinero::consensus;

namespace {
UtreexoHash mkhash(uint64_t seed) {
    UtreexoHash h(32);
    for (int i = 0; i < 32; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;  // LCG, deterministic
        h[i] = static_cast<uint8_t>(seed >> 33);
    }
    return h;
}
UtreexoForest makeForest(std::size_t leaves, uint64_t salt) {
    UtreexoForest f;
    for (std::size_t i = 0; i < leaves; ++i) f.add(mkhash(salt * 1000003ULL + i));
    return f;
}
}  // namespace

int main(int argc, char** argv) {
    const int n_readers = argc > 1 ? std::atoi(argv[1]) : 8;
    const int n_writers = argc > 2 ? std::atoi(argv[2]) : 2;
    const int seconds   = argc > 3 ? std::atoi(argv[3]) : 3;

    ConsensusUTXOSet set;
    set.ReplaceForestGuarded(makeForest(256, 1));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> progress{0};
    std::vector<std::thread> threads;

    for (int r = 0; r < n_readers; ++r) {
        threads.emplace_back([&, r] {
            std::uint64_t local = 0, acc = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                acc += set.SnapshotForestRoots().size();
                acc += set.SnapshotForestCommitment().size();
                acc += set.SnapshotForestLeafCount();
                {
                    // Structural clone-under-shared-lock (block_assembler pattern).
                    auto forest_lock = set.LockForestShared();
                    UtreexoForest c = set.GetForest().clone();
                    acc += c.getRoots().size();      // force a read of the clone's buffers
                    acc += c.getNumLeaves();
                }
                if (((++local) & 0x3F) == 0)
                    progress.fetch_add(1, std::memory_order_relaxed);
            }
            if (acc == 0xDEADBEEF) std::fprintf(stderr, "%d", r);  // defeat DCE
        });
    }

    for (int w = 0; w < n_writers; ++w) {
        threads.emplace_back([&, w] {
            std::uint64_t k = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const std::size_t leaves = 64 + (k % 512);
                set.ReplaceForestGuarded(makeForest(leaves, static_cast<uint64_t>(w) * 7919 + k));
                if ((k & 3) == 0) set.RemoveLastNLeavesGuarded(1 + (k % 8));
                // Also exercise MutateForestGuarded — the exclusive-lock primitive
                // StatelessNode's CSN-mode forest writes route through.
                if ((k & 1) == 0) {
                    set.MutateForestGuarded([&](UtreexoForest& f) {
                        f = makeForest(96 + (k % 256), static_cast<uint64_t>(w) * 104729 + k);
                    });
                }
                ++k;
                progress.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Watchdog: hard-fail if no thread makes progress for 5s (deadlock).
    std::thread watchdog([&] {
        std::uint64_t last = progress.load();
        int stalls = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const std::uint64_t cur = progress.load();
            if (cur == last) {
                if (++stalls >= 10) {
                    std::fprintf(stderr,
                                 "FAIL: forest-lock DEADLOCK — no progress for 5s "
                                 "(%d readers x %d writers)\n",
                                 n_readers, n_writers);
                    std::fflush(stderr);
                    std::_Exit(2);
                }
            } else {
                stalls = 0;
                last = cur;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    watchdog.join();

    std::printf("PASS: forest-lock stress — %d readers x %d writers x %ds, "
                "progress=%llu, no deadlock, no crash\n",
                n_readers, n_writers, seconds,
                static_cast<unsigned long long>(progress.load()));
    return 0;
}
