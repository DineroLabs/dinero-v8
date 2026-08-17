// Regression test for the Utreexo-forest read-during-free UAF (audit HIGH finding).
//
// Root cause: the forest is replaced/mutated on the block-connect thread while
// RPC/mining/FFI threads read it with no lock, so a replacement can free the
// storage a reader is walking. GuardedForest<T> fixes this by holding a reader's
// SHARED lock across the whole read, so a replacement (EXCLUSIVE) cannot run
// until every reader has finished.
//
// This test asserts that mutual-exclusion property deterministically, without a
// sanitizer (ASan/TSan are unusable on some dev machines): while a reader holds
// the shared lock inside WithShared(), a concurrent Replace() MUST NOT complete
// until the reader releases. The test times a reader that deliberately holds the
// lock and checks the writer's Replace() finished AFTER the reader's read, not
// during it.
//
// Gate is the process exit code (0 = pass, 1 = fail) — never assert(), which is
// a no-op under NDEBUG. Fails against a guard whose WithShared/Replace do not
// actually lock (proven by neutering the lock); passes against the real guard.

#include "consensus/guarded_forest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using dinero::consensus::GuardedForest;

namespace {
struct HeapBuf {
    std::vector<uint8_t> data;
    HeapBuf() = default;
    explicit HeapBuf(std::size_t n) : data(n, 0xAB) {}
};

// Monotonic sequence stamp so we can order the reader's "read finished" against
// the writer's "replace finished" without wall-clock comparisons.
std::atomic<int> g_seq{0};
int stamp() { return g_seq.fetch_add(1, std::memory_order_seq_cst); }
}  // namespace

int main() {
    GuardedForest<HeapBuf> guard{HeapBuf(1u << 16)};
    std::atomic<bool> reader_inside{false};
    std::atomic<int> read_finished_seq{-1};
    std::atomic<int> replace_finished_seq{-1};

    std::thread reader([&] {
        guard.WithShared([&](const HeapBuf&) {
            reader_inside.store(true, std::memory_order_release);
            // Hold the shared lock long enough that a writer NOT excluded by the
            // lock would finish its Replace() well before we release.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            read_finished_seq.store(stamp(), std::memory_order_seq_cst);
            return 0;
        });
    });

    // Wait until the reader is provably inside the shared-locked region.
    while (!reader_inside.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Give the reader a moment to be mid-hold, then replace. With a correct lock
    // this call blocks until the reader releases the shared lock.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    guard.Replace(HeapBuf(1u << 16));
    replace_finished_seq.store(stamp(), std::memory_order_seq_cst);

    reader.join();

    const int rf = read_finished_seq.load();
    const int pf = replace_finished_seq.load();
    std::printf("read_finished_seq=%d replace_finished_seq=%d\n", rf, pf);

    // Correct guard: the read (holding the shared lock) finishes BEFORE the
    // replacement can complete -> rf < pf. A guard that does not lock lets the
    // replacement finish first -> rf > pf.
    if (!(rf >= 0 && pf >= 0 && rf < pf)) {
        std::printf("FAIL: Replace() was not excluded by an in-progress read "
                    "(read-during-free UAF is possible)\n");
        return 1;
    }
    std::printf("PASS: Replace() waited for the in-progress read\n");
    return 0;
}
