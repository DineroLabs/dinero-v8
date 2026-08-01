// Behavior-matrix regression test for the canonical sync snapshot (#439).
//
// Proves the convergence rule documented in
// docs/architecture/sync-state-behavior-matrix.md. The mainnet before/after run
// is only a NON-REGRESSION smoke test — on a synced node headers == blocks under
// both the old fallback and the new implementation, so it cannot demonstrate
// that this change works. The matrix rows below are the actual proof.
//
// Two properties are load-bearing and each has a dedicated row:
//   * fail-closed  — any missing input yields Unknown, and Unknown is NEVER
//                    treated as synced (rows 1, 6a, 6b, 7)
//   * hash, not height — an equal-height reorg (same height, different hash) is
//                    NOT convergence (row 5)
//
// Exits non-zero on failure; does NOT rely on assert(), which is a no-op under
// NDEBUG and would not gate a release/CI build.

#include "consensus/header_convergence.h"

#include <cstdio>
#include <string>

using Convergence = dinero::consensus::HeaderConvergence;
using dinero::consensus::ComputeHeaderConvergence;
using SyncSnapshot = dinero::consensus::SyncSnapshot;
using dinero::uint256;

namespace {

int g_failures = 0;

uint256 HashFrom(uint8_t seed) {
    uint256 h;
    // Distinct, non-zero content per seed.
    reinterpret_cast<uint8_t*>(h.begin())[0] = seed;
    reinterpret_cast<uint8_t*>(h.begin())[31] = static_cast<uint8_t>(0xA0 ^ seed);
    return h;
}

const char* Name(Convergence c) {
    switch (c) {
        case Convergence::Unknown:      return "Unknown";
        case Convergence::Mismatch:     return "Mismatch";
        case Convergence::Converged:    return "Converged";
    }
    return "?";
}

void Row(const char* row,
         bool has_best, const uint256& best,
         bool has_tip, const uint256& tip,
         Convergence expected) {
    const Convergence got =
        ComputeHeaderConvergence(has_best, best, has_tip, tip);
    if (got == expected) {
        std::printf("  ok   %-46s -> %s\n", row, Name(got));
        return;
    }
    std::fprintf(stderr, "  FAIL %-46s -> got %s, expected %s\n",
                 row, Name(got), Name(expected));
    ++g_failures;
}

// A missing input must never read as synced, whatever else is true.
void MustNotBeSynced(const char* row,
                     bool has_best, const uint256& best,
                     bool has_tip, const uint256& tip) {
    // Exercises the PRODUCTION type + method, not a re-implementation of it.
    SyncSnapshot snap;
    snap.has_best_header = has_best;
    snap.best_header_hash = best;
    snap.has_active_tip = has_tip;
    snap.active_tip_hash = tip;
    snap.RecomputeConvergence();

    if (!snap.IsConverged()) {
        std::printf("  ok   %-46s -> IsConverged()==false\n", row);
        return;
    }
    std::fprintf(stderr, "  FAIL %-46s -> IsConverged()==true (must fail closed)\n", row);
    ++g_failures;
}

}  // namespace

int main() {
    const uint256 kEmpty;              // all-zero
    const uint256 kA = HashFrom(0x11);
    const uint256 kB = HashFrom(0x22);

    std::printf("Behavior matrix — docs/architecture/sync-state-behavior-matrix.md\n");

    // Row 1 — cold start: nothing loaded.
    Row("1  cold start (no best header, no tip)",
        false, kEmpty, false, kEmpty, Convergence::Unknown);

    // Row 2 — genesis-only regtest: best header == tip == genesis.
    Row("2  genesis-only regtest (best == tip)",
        true, kA, true, kA, Convergence::Converged);

    // Row 3 — headers ahead during normal sync.
    Row("3  headers ahead (best != tip)",
        true, kA, true, kB, Convergence::Mismatch);

    // Row 4 — convergence reached.
    Row("4  convergence reached (best == tip)",
        true, kB, true, kB, Convergence::Converged);

    // Row 5 — equal-height reorg. THE reason convergence compares hashes:
    // both sides are at the same height, but on different blocks.
    Row("5  equal-height reorg (same h, diff hash)",
        true, kA, true, kB, Convergence::Mismatch);

    // Row 6 — missing dependencies, each direction.
    Row("6a missing best header (selector unset/empty)",
        false, kEmpty, true, kA, Convergence::Unknown);
    Row("6b missing active tip",
        true, kA, false, kEmpty, Convergence::Unknown);

    // Row 7 — restart: tip loaded, headers not yet re-read.
    Row("7  restart (tip known, best header not yet)",
        false, kEmpty, true, kA, Convergence::Unknown);

    // Row 8 — AssumeUTXO at convergence. Convergence is reported independently
    // of AssumeUTXO/background-validation state, which this change does not
    // touch; the point of the row is that convergence does NOT depend on them.
    Row("8  assumeutxo converged (best == tip)",
        true, kA, true, kA, Convergence::Converged);

    std::printf("Fail-closed: Unknown must never satisfy a synced test\n");
    MustNotBeSynced("1  cold start", false, kEmpty, false, kEmpty);
    MustNotBeSynced("6a missing best header", false, kEmpty, true, kA);
    MustNotBeSynced("6b missing active tip", true, kA, false, kEmpty);
    MustNotBeSynced("7  restart before headers re-read", false, kEmpty, true, kA);

    // Degenerate guard: two all-zero hashes must not be called converged just
    // because they are equal — the has_* flags, not the hash values, decide
    // whether an input exists.
    MustNotBeSynced("   both inputs absent, hashes equal", false, kEmpty, false, kEmpty);

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d matrix row(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nPASS: sync-snapshot behavior matrix\n");
    return 0;
}
