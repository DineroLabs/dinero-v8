// Regression: the standalone Spartan decider must require the relaxed-R1CS error
// vector E to be zero. Without that check a malicious prover commits the residual
// E := A·z∘B·z − u·C·z of an INVALID witness and forges a proof of a false
// statement (arbitrary shielded mint). See r1cs_spartan_verify(require_zero_error).
//
// The three tests together prove the guard is load-bearing:
//   HonestProofVerifies              — honest proof of a SATISFIED R1CS (E=0) verifies.
//   ForgedNonzeroErrorRejected       — forged proof of an UNSATISFIED R1CS is rejected
//                                       (this is the fix; it fails if the check is removed).
//   ForgedVerifiesWhenCheckDisabled  — with require_zero_error=false the same forgery
//                                       verifies, proving the default-true check is the
//                                       ONLY thing standing between forgery and acceptance.
#include <gtest/gtest.h>

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/transcript.h"
#include "zk/zkvm/scalar.h"

#include <secp256k1.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace dinero::zk::zkvm;

namespace {

secp256k1_context* Ctx() {
    static secp256k1_context* c =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return c;
}

// R1CS with two constraints x*x==x and y*y==y (satisfied only for x,y in {0,1}).
R1CS MakeCircuit(uint64_t xv, uint64_t yv) {
    R1CS cs;
    Variable x = cs.alloc(Scalar(xv));
    Variable y = cs.alloc(Scalar(yv));
    cs.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "x2");
    cs.constrain(LinearCombination(y), LinearCombination(y), LinearCombination(y), "y2");
    return cs;
}

// The pointwise residual E = A·z ∘ B·z − C·z (u = 1) for the assigned witness.
// Zero iff the witness satisfies the R1CS.
std::vector<Scalar> ResidualError(const R1CS& cs) {
    const std::vector<Scalar>& z = cs.witness();
    const auto& cons = cs.constraints();
    std::vector<Scalar> E(cs.num_constraints());
    for (size_t i = 0; i < cons.size(); ++i) {
        E[i] = cons[i].a.evaluate(z) * cons[i].b.evaluate(z) - cons[i].c.evaluate(z);
    }
    return E;
}

bool ProveThenVerify(const R1CS& cs, const std::vector<Scalar>& E, bool require_zero_error) {
    secp256k1_context* ctx = Ctx();
    const size_t gens_need = std::max<size_t>(4,
        std::max(HyraxParams::from_n(cs.num_variables()).n_cols,
                 HyraxParams::from_n(cs.num_constraints()).n_cols));
    const GeneratorSet& gens = GeneratorSet::cached(gens_need, ctx);

    Transcript tp("spartan.regression");
    SpartanProof proof = r1cs_spartan_prove(cs, E, Scalar::one(), gens, tp, ctx,
                                            /*bind_public_inputs=*/true);

    Transcript tv("spartan.regression");
    const std::vector<uint8_t> no_hash;  // skip fast-reject; still FS-bound via proof.circuit_hash
    return r1cs_spartan_verify(proof, cs, cs.num_constraints(), cs.num_variables(),
                               no_hash, Scalar::one(), gens, tv, ctx,
                               /*bind_public_inputs=*/true,
                               /*require_zero_error=*/require_zero_error);
}

}  // namespace

TEST(SpartanSoundness, HonestProofVerifies) {
    R1CS cs = MakeCircuit(/*xv=*/1, /*yv=*/1);
    std::string why;
    ASSERT_TRUE(cs.is_satisfied(why)) << "witness should satisfy the R1CS";
    const std::vector<Scalar> zeroE(cs.num_constraints(), Scalar::zero());
    EXPECT_TRUE(ProveThenVerify(cs, zeroE, /*require_zero_error=*/true))
        << "an honest proof of a satisfied R1CS must verify";
}

TEST(SpartanSoundness, ForgedNonzeroErrorRejected) {
    R1CS cs = MakeCircuit(/*xv=*/5, /*yv=*/1);  // 5*5 != 5 -> unsatisfiable
    std::string why;
    ASSERT_FALSE(cs.is_satisfied(why)) << "witness must NOT satisfy the R1CS";
    const std::vector<Scalar> forgedE = ResidualError(cs);  // nonzero at the violated row
    EXPECT_FALSE(ProveThenVerify(cs, forgedE, /*require_zero_error=*/true))
        << "SOUNDNESS: a forged proof for an UNSATISFIED R1CS must be rejected";
}

TEST(SpartanSoundness, ForgedVerifiesWhenCheckDisabled) {
    // Documents the escape hatch and the pre-fix behavior: with the E==0 guard
    // disabled (folding-only mode), the exact same forgery verifies. This proves
    // the default-true guard is the sole barrier — i.e. the regression above fails
    // the moment the check is removed.
    R1CS cs = MakeCircuit(/*xv=*/5, /*yv=*/1);
    const std::vector<Scalar> forgedE = ResidualError(cs);
    EXPECT_TRUE(ProveThenVerify(cs, forgedE, /*require_zero_error=*/false))
        << "with require_zero_error=false the forgery is (intentionally) accepted";
}
