// Regression test: an EMPTY aggregated_range_proof must NOT bypass the
// mandatory range-proof / binding-sig checks once shielded value is active.
//
// Vulnerability (shielded INFLATION): the range-proof and Schnorr binding-sig
// checks were both gated on `!bundle.aggregated_range_proof.empty()`, so an
// attacker who sent an EMPTY range proof skipped both checks, leaving hidden
// note values unconstrained -> mint from nothing.
//
// Discriminator: a non-empty bundle (one spend, garbage zk_proof) with an
// EMPTY aggregated_range_proof, validated at a height >= the input-binding
// activation height (the default 0 in a directly-constructed context, which is
// the "always mandatory" rule).
//   - WITHOUT the fix: both new checks are skipped; validation proceeds to ZK
//     proof verification and returns ProofInvalid (or balances out) — NOT a
//     range-proof rejection.
//   - WITH the fix: the empty proof is rejected up-front with RangeProofInvalid.
//
// Exit non-zero on failure (does NOT rely on assert()).

#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_tx.h"

#include <cstdio>

using namespace dinero::consensus::shielded;

int main() {
    // One spend with a NON-EMPTY (garbage) zk_proof so the structural check
    // passes, but an EMPTY aggregated_range_proof on the bundle.
    ShieldedBundle bundle;
    bundle.value_balance = 0;
    ShieldedSpend spend;            // nullifier/anchor default-zero
    spend.zk_proof = {0x01, 0x02, 0x03, 0x04};  // non-empty garbage
    bundle.spends.push_back(spend);
    // bundle.aggregated_range_proof intentionally left EMPTY.

    // Context: shielded active from height 0 (so the non-empty bundle is past
    // the NotActive gate), no nullifier set / commitment tree (anchor check
    // skipped), zero sighash. shielded_input_binding_activation_height defaults
    // to 0 -> block_height(100) is in the mandatory branch.
    ValidationContext ctx(
        /*nullifier_set=*/nullptr,
        /*commitment_tree=*/nullptr,
        /*block_height=*/100,
        /*transparent_value_delta=*/0,
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        /*tx_sighash=*/Hash{});

    const ShieldedValidationError err = ValidateShieldedBundle(bundle, ctx);

    if (err != ShieldedValidationError::RangeProofInvalid) {
        std::fprintf(stderr,
                     "FAIL: empty aggregated_range_proof was NOT rejected as "
                     "RangeProofInvalid (got error code %d) -> inflation vector "
                     "open\n",
                     static_cast<int>(err));
        return 1;
    }

    std::printf("PASS: empty aggregated_range_proof rejected (RangeProofInvalid)\n");
    return 0;
}
