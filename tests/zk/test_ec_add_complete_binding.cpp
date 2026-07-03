/**
 * Regression test for the ec_add_complete soundness fix (cv-binding audit, 2026-07-01).
 *
 * THE BUG: ec_add_complete chose between add(P,Q) / double(P) / identity via
 * `is_same_pt` / `is_opposite`, which were FREE prover witnesses (only
 * enforce_boolean'd). A malicious prover could pick the branch and forge the
 * result — e.g. force `double` so cv = 2*val*V, or force `identity` so val*V = O
 * — decoupling the value commitment from the note value (shielded mint-from-nothing),
 * plus an analogous forgery in the Schnorr circuit that shares this gadget.
 *
 * THE FIX: the selectors are now PINNED (cs.enforce_equal) to constrained
 * predicates derived from the actual geometry:
 *     is_same_pt  == dx_is_zero AND dy_is_zero AND neither_inf   (labels *_sptbind)
 *     is_opposite == dx_is_zero AND NOT dy_is_zero AND neither_inf (labels *_oppbind)
 *
 * These tests FAIL without the fix: (1) the _sptbind / _oppbind constraints do not
 * exist, and (2) a free selector can be flipped without violating any constraint.
 */

#include <gtest/gtest.h>
#include "zk/zkvm/ec_gadget.h"
#include "zk/zkvm/r1cs.h"
#include <string>

using namespace dinero::zk::zkvm;

namespace {

// Two finite points with caller-chosen coordinates. ec_add_complete does not
// assert on-curve, so arbitrary distinct coords exercise the branch selection
// (the selector logic depends only on coordinate equality, not curve membership).
ECPoint AllocPoint(R1CS& cs, uint64_t x, uint64_t y, const std::string& label) {
    return ec_alloc(cs, Uint256(static_cast<uint64_t>(x)),
                        Uint256(static_cast<uint64_t>(y)), false, label);
}

// Return the first variable of the `a` linear combination of the (single)
// constraint whose label ends with `suffix`.
bool BoundVarFromConstraint(const R1CS& cs, const std::string& suffix, Variable& out) {
    for (const auto& c : cs.constraints()) {
        if (c.label.size() >= suffix.size() &&
            c.label.compare(c.label.size() - suffix.size(), suffix.size(), suffix) == 0) {
            const auto& terms = c.a.terms();
            if (!terms.empty()) { out = terms[0].var; return true; }
        }
    }
    return false;
}

bool HasConstraintWithSuffix(const R1CS& cs, const std::string& suffix) {
    for (const auto& c : cs.constraints()) {
        if (c.label.size() >= suffix.size() &&
            c.label.compare(c.label.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

// GATE: without the fix the gadget emits no selector-binding constraint, so the
// branch selectors are free witnesses. Their presence is the fix.
TEST(EcAddCompleteBinding, EmitsSelectorBindingConstraints) {
    R1CS cs;
    ECPoint P = AllocPoint(cs, 3, 5, "P");
    ECPoint Q = AllocPoint(cs, 7, 11, "Q");  // distinct x
    ec_add_complete(cs, P, Q, "t");
    EXPECT_TRUE(HasConstraintWithSuffix(cs, "_sptbind"))
        << "ec_add_complete must PIN is_same_pt to the geometry; missing bind => free selector => forgeable";
    EXPECT_TRUE(HasConstraintWithSuffix(cs, "_oppbind"))
        << "ec_add_complete must PIN is_opposite to the geometry";
}

// FUNCTIONAL: distinct points => is_same_pt is forced to 0; forcing it to 1
// (the "double" branch forgery) must be rejected.
TEST(EcAddCompleteBinding, DistinctPointsCannotForceDouble) {
    R1CS cs;
    ECPoint P = AllocPoint(cs, 3, 5, "P");
    ECPoint Q = AllocPoint(cs, 7, 11, "Q");  // distinct x
    ec_add_complete(cs, P, Q, "t");
    ASSERT_TRUE(cs.is_satisfied()) << "honest add of distinct points must satisfy";

    Variable is_same_pt;
    ASSERT_TRUE(BoundVarFromConstraint(cs, "_sptbind", is_same_pt))
        << "no _sptbind constraint => fix missing";
    EXPECT_TRUE(cs.get_value(is_same_pt).is_zero())
        << "distinct points => is_same_pt constrained to 0";

    cs.set_value(is_same_pt, Scalar::one());  // force the doubling branch
    EXPECT_FALSE(cs.is_satisfied())
        << "forcing is_same_pt=1 for distinct points must be rejected by the pin (mint-from-nothing vector)";
}

// FUNCTIONAL: identical points => is_same_pt is forced to 1; forcing it to 0
// (to escape doubling into the general-add branch) must be rejected.
TEST(EcAddCompleteBinding, SamePointCannotEscapeDouble) {
    R1CS cs;
    ECPoint P = AllocPoint(cs, 9, 13, "P");
    ECPoint Q = AllocPoint(cs, 9, 13, "Q");  // identical coords
    ec_add_complete(cs, P, Q, "t");
    ASSERT_TRUE(cs.is_satisfied()) << "honest double must satisfy";

    Variable is_same_pt;
    ASSERT_TRUE(BoundVarFromConstraint(cs, "_sptbind", is_same_pt));
    EXPECT_FALSE(cs.get_value(is_same_pt).is_zero())
        << "identical points => is_same_pt constrained to 1";

    cs.set_value(is_same_pt, Scalar::zero());
    EXPECT_FALSE(cs.is_satisfied())
        << "forcing is_same_pt=0 for identical points must be rejected by the pin";
}

}  // namespace
