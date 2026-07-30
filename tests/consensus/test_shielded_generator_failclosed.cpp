// Regression test: consensus shielded validation must FAIL CLOSED when the
// Pedersen generators are unavailable, and a node that cannot derive them must
// refuse to start.
//
// Vulnerability shape. ValidateShieldedBundle ran the mandatory range-proof and
// binding-signature checks only `if (PedersenGeneratorsReady())`. A node whose
// generator derivation failed therefore SKIPPED both value-integrity checks and
// fell through to ZK proof verification, so block validity silently depended on
// a node's runtime initialization state rather than on the block.
//
// Discriminator (height >= the input-binding activation height, a NON-EMPTY
// aggregated_range_proof so the empty-proof rejection is not what fires, and the
// generators forced unavailable through the test seam):
//   - WITHOUT the fix: both checks are skipped; validation falls through to the
//     ZK step and returns ProofInvalid — the value commitments are never
//     examined at all.
//   - WITH the fix: rejected up-front with RangeProofInvalid.
//
// The startup precondition is what makes the validation-time branch unreachable
// on a real node. Generator derivation runs under std::call_once, so a failure
// is PERMANENT for the process: without the precondition a broken node does not
// crash, it silently rejects every shielded bundle above the input-binding
// activation height and forks itself off the network while appearing healthy.
//
// Exits non-zero on failure (does NOT rely on assert(), which is a no-op under
// NDEBUG and would not gate a release/CI build).

#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"

#include <cstdio>
#include <string>

using namespace dinero::consensus::shielded;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        std::printf("  ok: %s\n", what);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

// A bundle that reaches the mandatory range-proof branch: one spend with a
// non-empty (garbage) zk_proof so the structural check passes, and a NON-EMPTY
// aggregated_range_proof so the empty-proof rejection is not the thing firing.
ShieldedBundle MakeBundleReachingRangeProofBranch() {
    ShieldedBundle bundle;
    bundle.value_balance = 0;
    ShieldedSpend spend;  // nullifier/anchor default-zero
    spend.zk_proof = {0x01, 0x02, 0x03, 0x04};
    bundle.spends.push_back(spend);
    bundle.aggregated_range_proof = {0xAA, 0xBB, 0xCC, 0xDD};
    return bundle;
}

// Shielded active from height 0, no nullifier set / commitment tree (anchor
// check skipped), zero sighash. shielded_input_binding_activation_height
// defaults to 0, so block_height 100 lands in the mandatory branch.
ValidationContext MakeMandatoryBranchContext() {
    return ValidationContext(
        /*nullifier_set=*/nullptr,
        /*commitment_tree=*/nullptr,
        /*block_height=*/100,
        /*transparent_value_delta=*/0,
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        /*tx_sighash=*/Hash{});
}

}  // namespace

int main() {
    // ── 1. Baseline: the seam is off, so the generators derive normally. ──
    Check(PedersenGeneratorsReady(),
          "generators derive successfully by default");

    std::string error = "unset";
    Check(CheckPedersenGeneratorsStartupPrecondition(&error),
          "startup precondition passes when generators are available");
    Check(error.empty(),
          "startup precondition leaves the error string empty on success");

    // ── 2. Startup precondition rejects an unavailable generator. ──
    SetPedersenGeneratorsUnavailableForTest(true);

    Check(!PedersenGeneratorsReady(),
          "test seam makes PedersenGeneratorsReady() report failure");

    error.clear();
    Check(!CheckPedersenGeneratorsStartupPrecondition(&error),
          "startup precondition FAILS when generators are unavailable");
    Check(!error.empty(),
          "startup precondition explains the failure");

    // ── 3. Consensus validation fails closed rather than skipping the check. ──
    const ShieldedBundle bundle = MakeBundleReachingRangeProofBranch();
    const ValidationContext ctx = MakeMandatoryBranchContext();
    const ShieldedValidationError err = ValidateShieldedBundle(bundle, ctx);

    if (err == ShieldedValidationError::RangeProofInvalid) {
        std::printf("  ok: unavailable generator rejected as RangeProofInvalid\n");
    } else {
        std::fprintf(stderr,
                     "FAIL: unavailable generator did NOT fail closed — expected "
                     "RangeProofInvalid, got error code %d. The range-proof and "
                     "binding-sig checks were skipped and validation fell through "
                     "to ZK verification.\n",
                     static_cast<int>(err));
        ++g_failures;
    }

    // ── 4. Restore the seam so the process ends in a sane state. ──
    SetPedersenGeneratorsUnavailableForTest(false);
    Check(PedersenGeneratorsReady(),
          "test seam is reversible");

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nPASS: shielded consensus crypto fails closed and is gated at startup\n");
    return 0;
}
