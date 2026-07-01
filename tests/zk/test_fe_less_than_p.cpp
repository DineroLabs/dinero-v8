/**
 * Unit test for fe_assert_less_than_p (secp256k1 field canonicity check).
 *
 * The original implementation computed the overflow flag from the WITNESS and
 * merely asserted `no_overflow == 1`, never constraining that flag to the actual
 * limbs of `a`. So a malicious prover could override the flag and "prove" a >= p.
 * This test pins the SOUND behaviour:
 *   - canonical a < p  => satisfiable
 *   - a >= p (honest)  => unsatisfiable
 *   - a >= p must stay unsatisfiable even if the prover overrides any single
 *     gadget variable (this is the soundness gate the old impl fails).
 */

#include <gtest/gtest.h>
#include "zk/zkvm/secp256k1_fe_gadget.h"
#include "zk/zkvm/r1cs.h"

using namespace dinero::zk::zkvm;

namespace {

Uint256 U(uint64_t l0, uint64_t l1, uint64_t l2, uint64_t l3) {
    Uint256 u;
    u.limbs[0] = l0; u.limbs[1] = l1; u.limbs[2] = l2; u.limbs[3] = l3;
    return u;
}
Uint256 Pval() {
    return U(SECP256K1_P_LIMB0, SECP256K1_P_LIMB1, SECP256K1_P_LIMB2, SECP256K1_P_LIMB3);
}

TEST(FeLessThanP, CanonicalValuesSatisfy) {
    const Uint256 cases[] = {
        U(0, 0, 0, 0),
        U(1, 0, 0, 0),
        U(0xFFFFFFFFFFFFFFFFull, 0, 0, 0),
        U(SECP256K1_P_LIMB0 - 1, SECP256K1_P_LIMB1, SECP256K1_P_LIMB2, SECP256K1_P_LIMB3),  // p-1
    };
    for (const auto& a : cases) {
        R1CS cs;
        FieldElement fa = fe_alloc_uint256(cs, a, "a");
        fe_assert_less_than_p(cs, fa, "t");
        EXPECT_TRUE(cs.is_satisfied()) << "canonical a < p must satisfy";
    }
}

TEST(FeLessThanP, OutOfRangeHonestRejected) {
    const Uint256 cases[] = {
        Pval(),                                                                    // a == p
        U(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
          0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull),                           // a == 2^256-1
    };
    for (const auto& a : cases) {
        R1CS cs;
        FieldElement fa = fe_alloc_uint256(cs, a, "a");
        fe_assert_less_than_p(cs, fa, "t");
        EXPECT_FALSE(cs.is_satisfied()) << "a >= p must be rejected by the honest witness";
    }
}

// Soundness gate: a == p must remain unprovable even if a malicious prover
// overrides any single variable the gadget introduced. The original impl fails
// this (setting its free `no_overflow` flag to 1 rescues the proof).
TEST(FeLessThanP, OutOfRangeNotForgeableBySingleOverride) {
    R1CS cs;
    FieldElement fa = fe_alloc_uint256(cs, Pval(), "a");
    const size_t first_gadget_var = cs.num_variables();
    fe_assert_less_than_p(cs, fa, "t");
    ASSERT_FALSE(cs.is_satisfied()) << "a == p must be rejected honestly";

    bool rescued = false;
    for (size_t idx = first_gadget_var; idx < cs.num_variables() && !rescued; ++idx) {
        Variable v{idx};
        const Scalar orig = cs.get_value(v);
        for (const Scalar& cand : {Scalar::zero(), Scalar::one()}) {
            cs.set_value(v, cand);
            if (cs.is_satisfied()) { rescued = true; break; }
        }
        cs.set_value(v, orig);
    }
    EXPECT_FALSE(rescued)
        << "a == p became provable by overriding a single gadget variable "
           "=> the canonicity flag is not bound to a's limbs (unsound)";
}

}  // namespace
