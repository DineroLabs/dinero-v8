// SelectParams() must reject a chain ATOMICALLY.
//
// THE DEFECT
// ----------
// SelectParams() published the chain before validating it:
//
//     g_paramsSelected = true;                  // (1) committed
//     switch (chain) { g_active = &g_<chain>; } // (2) committed
//     ... validate ...                          // (3) may throw
//
// Because the validators read g_active, they could not run any earlier. So a
// rejected chain stayed committed: Params(), GetActiveChain(), MutableParams(),
// and IsChainSelected() all reported a chain that consensus had just refused.
//
// WHY IT MATTERS BEYOND A TIDINESS ARGUMENT
// -----------------------------------------
// The two validators involved are the shielded ones -- cv-binding ordering and
// epoch-reset equality. Those guard the mint-from-nothing window documented at
// length in chainparams_impl.cpp. A caller that catches the exception and
// carries on would be running with shielded parameters that failed exactly
// those checks.
//
// Today main.cpp and nodecore_start() both call SelectParams() uncaught, so a
// throw kills the process and the partial state is not observed in production.
// That is fail-closed by process death, not by design: nodecore_ffi.cpp already
// converts exceptions into NODECORE_ERROR_START_FAILED elsewhere, and wrapping
// this call the same way would silently put a node on refused parameters. Test
// binaries observe it immediately, since gtest catches.
//
// SCOPE OF THIS FILE
// ------------------
// These tests exercise rejection through the SHIELDED invariants, which exist
// independently of any covenant work, so they stay valid regardless of which
// opcodes are activated. The never-selected case -- where SelectParams() marked
// a chain selected while rejecting an unknown enum -- cannot be reached from
// inside a gtest binary (something has always selected already, and death tests
// fork the parent's globals rather than resetting them); it is covered by the
// separate SelectParamsFreshProcess executable.

#include "consensus/chainparams.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

namespace {

using dinero::Chain;
using dinero::ChainParams;
using dinero::Params;
using dinero::SelectParams;

// Restores the mutated fields even if an assertion aborts the test, so a
// poisoned regtest cannot leak into other tests in this binary.
class ScopedShieldedHeights {
public:
    explicit ScopedShieldedHeights(ChainParams* params)
        : params_(params),
          reset_(params->shielded_epoch_reset_height),
          cv_(params->shielded_cv_binding_activation_height),
          input_(params->shielded_input_binding_activation_height) {}

    ~ScopedShieldedHeights() {
        params_->shielded_epoch_reset_height = reset_;
        params_->shielded_cv_binding_activation_height = cv_;
        params_->shielded_input_binding_activation_height = input_;
    }

    ScopedShieldedHeights(const ScopedShieldedHeights&) = delete;
    ScopedShieldedHeights& operator=(const ScopedShieldedHeights&) = delete;

private:
    ChainParams* params_;
    uint32_t reset_;
    uint32_t cv_;
    uint32_t input_;
};

TEST(ChainParamsSelection, EpochResetMustEqualCvBindingActivation) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    // Shipped regtest satisfies the invariant.
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));

    // Break only the equality: reset must coincide with cv-binding activation.
    regtest->shielded_epoch_reset_height =
        regtest->shielded_cv_binding_activation_height - 1;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);
}

TEST(ChainParamsSelection, CvBindingMustNotPrecedeInputBinding) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    // cv-binding strictly before input-binding is the ordering that would leave
    // the cv circuit running unbound.
    regtest->shielded_input_binding_activation_height = 500;
    regtest->shielded_cv_binding_activation_height = 400;
    regtest->shielded_epoch_reset_height = 400;  // keep the other check happy
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);
}

// The assertion this file exists for. A plain EXPECT_THROW passes against the
// broken ordering too -- only this catches it.
TEST(ChainParamsSelection, RejectedSelectionCommitsNoGlobalState) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    // Make regtest invalid.
    regtest->shielded_epoch_reset_height =
        regtest->shielded_cv_binding_activation_height - 1;

    // Establish a known-good active chain, so any leaked commit is attributable
    // to the rejected call rather than to leftover state.
    SelectParams(Chain::MAINNET);
    ASSERT_EQ(dinero::GetActiveChain(), Chain::MAINNET);
    const uint32_t mainnet_reset = Params().shielded_epoch_reset_height;

    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);

    // The rejected chain must NOT have become active.
    EXPECT_EQ(dinero::GetActiveChain(), Chain::MAINNET)
        << "SelectParams published the rejected chain before validating it; "
           "Params() now returns parameters consensus refused";
    EXPECT_EQ(Params().shielded_epoch_reset_height, mainnet_reset)
        << "active shielded parameters came from the rejected chain";
    EXPECT_TRUE(dinero::IsChainSelected());

    // Leave a valid selection behind regardless of the outcome above.
    SelectParams(Chain::MAINNET);
}

}  // namespace
