// Regression test for the DineroDPI "proof leaf count mismatch" bug.
//
// wallet.getproofbundle must never emit a bundle where any individual proof's
// num_leaves differs from the stump num_leaves. That happens when a block
// connects on the sync thread between proof generation and the stump snapshot
// (the forest grows, so the stump's num_leaves ends up larger than the proofs').
// A proof-based light client (DineroDPI) then rejects the bundle with
// "proof leaf count mismatch" and quarantines the seed, so it can never sync.
//
// The handler now brackets assembly with a forest-commitment equality check +
// bounded retry; this asserts the exact invariant the client validates as a
// final defense-in-depth gate. Throwing checks so the test gates in release
// builds (assert() is a no-op under NDEBUG).

#include "rpc/proof_bundle_consistency.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using dinero::rpc::ProofBundleLeafCountsConsistent;

static void require(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error("FAIL: " + msg);
}

int main() {
    // Consistent: every proof at the same forest size as the stump.
    require(ProofBundleLeafCountsConsistent({100, 100, 100}, 100),
            "all-equal proofs must be accepted");
    require(ProofBundleLeafCountsConsistent({}, 12345),
            "empty proof set is vacuously consistent");
    require(ProofBundleLeafCountsConsistent({777}, 777),
            "single matching proof must be accepted");

    // The bug, reproduced as data: a proof generated at an older forest size
    // than the stump (a block connected mid-assembly) — must be REJECTED.
    require(!ProofBundleLeafCountsConsistent({100, 100, 99}, 100),
            "a proof with stale num_leaves must be rejected");
    require(!ProofBundleLeafCountsConsistent({99, 99, 99}, 100),
            "proofs older than the stump must be rejected");
    require(!ProofBundleLeafCountsConsistent({100, 101}, 100),
            "a proof newer than the stump must be rejected");

    return 0;
}
