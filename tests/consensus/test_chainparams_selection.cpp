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
#include "consensus/shielded/wallet_activation.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/utxo_entry.h"

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
          input_(params->shielded_input_binding_activation_height),
          auth_(params->shielded_spend_auth_activation_height),
          compact_(params->shielded_compact_activation_height),
          timing_(params->sixty_second_activation_height),
          release_(params->release_v8113_activation_height),
          outgoing_(params->shielded_outgoing_recovery_activation_height),
          auth_reset_(params->shielded_spend_auth_epoch_reset_height) {}

    ~ScopedShieldedHeights() {
        params_->shielded_epoch_reset_height = reset_;
        params_->shielded_cv_binding_activation_height = cv_;
        params_->shielded_input_binding_activation_height = input_;
        params_->shielded_spend_auth_activation_height = auth_;
        params_->shielded_compact_activation_height = compact_;
        params_->sixty_second_activation_height = timing_;
        params_->release_v8113_activation_height = release_;
        params_->shielded_outgoing_recovery_activation_height = outgoing_;
        params_->shielded_spend_auth_epoch_reset_height = auth_reset_;
    }

    ScopedShieldedHeights(const ScopedShieldedHeights&) = delete;
    ScopedShieldedHeights& operator=(const ScopedShieldedHeights&) = delete;

private:
    ChainParams* params_;
    uint32_t reset_;
    uint32_t cv_;
    uint32_t input_;
    uint32_t auth_;
    uint32_t compact_;
    uint32_t timing_;
    uint32_t release_;
    uint32_t outgoing_;
    uint32_t auth_reset_;
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

TEST(ChainParamsSelection, CompactRequiresExistingBoundAuthorityAndNoNewReset) {
    SelectParams(Chain::MAINNET);
    auto* params = &dinero::MutableParams();
    const ScopedShieldedHeights restore(params);
    const auto auth = params->shielded_spend_auth_activation_height;
    const auto cv_reset = params->shielded_epoch_reset_height;
    const auto auth_reset = params->shielded_spend_auth_epoch_reset_height;
    // Keep the joint release invariant satisfied so these rejections test
    // compact's shielded prerequisites, not an unrelated partial schedule.
    params->shielded_compact_activation_height = auth;
    params->sixty_second_activation_height = auth;
    params->release_v8113_activation_height = auth;
    EXPECT_THROW(SelectParams(Chain::MAINNET), std::runtime_error);
    params->shielded_compact_activation_height = auth + 1;
    params->sixty_second_activation_height = auth + 1;
    params->release_v8113_activation_height = auth + 1;
    EXPECT_NO_THROW(SelectParams(Chain::MAINNET));
    EXPECT_EQ(Params().shielded_epoch_reset_height, cv_reset);
    EXPECT_EQ(Params().shielded_spend_auth_epoch_reset_height, auth_reset);
    params->shielded_spend_auth_activation_height = UINT32_MAX;
    params->shielded_spend_auth_epoch_reset_height = UINT32_MAX;
    EXPECT_THROW(SelectParams(Chain::MAINNET), std::runtime_error);
}

TEST(ChainParamsSelection, PublicReleaseRejectsPartialOrMismatchedSchedules) {
    SelectParams(Chain::MAINNET);
    auto* params = &dinero::MutableParams();
    const ScopedShieldedHeights restore(params);
    const auto activation = params->shielded_spend_auth_activation_height + 1;

    // Exercise the real selection boundary, in addition to the profile helper
    // tests. Every partial or mismatched public schedule must fail selection.
    for (unsigned mask = 1; mask < 7; ++mask) {
        params->shielded_compact_activation_height = mask & 1 ? activation : UINT32_MAX;
        params->sixty_second_activation_height = mask & 2 ? activation : UINT32_MAX;
        params->release_v8113_activation_height = mask & 4 ? activation : UINT32_MAX;
        EXPECT_THROW(SelectParams(Chain::MAINNET), std::runtime_error) << "mask=" << mask;
    }
    params->shielded_compact_activation_height = activation;
    params->sixty_second_activation_height = activation;
    params->release_v8113_activation_height = activation;
    EXPECT_NO_THROW(SelectParams(Chain::MAINNET));
    for (auto* height : {&params->shielded_compact_activation_height,
                         &params->sixty_second_activation_height,
                         &params->release_v8113_activation_height}) {
        *height = activation + 1;
        EXPECT_THROW(SelectParams(Chain::MAINNET), std::runtime_error);
        *height = activation;
    }
    EXPECT_NO_THROW(SelectParams(Chain::MAINNET));
}

TEST(ChainParamsSelection, SixtySecondRuleIsDormantAndChecksumCommitsActivation) {
    const auto original = dinero::GetActiveChain();
    for (auto chain : {Chain::MAINNET, Chain::TESTNET, Chain::REGTEST}) {
        SelectParams(chain);
        const auto baseline = Params();
        EXPECT_EQ(baseline.sixty_second_activation_height, UINT32_MAX);
        EXPECT_EQ(baseline.TargetSpacing(UINT32_MAX), 120u);
        auto activated = baseline;
        activated.sixty_second_activation_height = 1000;
        EXPECT_EQ(activated.TargetSpacing(999), 120u);
        EXPECT_EQ(activated.TargetSpacing(1000), 60u);
        EXPECT_NE(dinero::ConsensusChecksum(baseline), dinero::ConsensusChecksum(activated));
    }
    SelectParams(original);
}

TEST(ChainParamsSelection, SixtySecondRuleKeepsCoinbaseMaturityAcrossActivation) {
    const auto original = dinero::GetActiveChain();
    SelectParams(Chain::MAINNET);
    auto& params = dinero::MutableParams();
    const auto saved = params.sixty_second_activation_height;
    params.sixty_second_activation_height = 100'000;
    for (const uint32_t created : {99'999u, 100'000u, 100'001u}) {
        dinero::consensus::UTXOEntry coin(dinero::AmountUna::Una(100'000'000), {0x51}, created, true);
        EXPECT_FALSE(coin.isMature(created + 99));
        EXPECT_TRUE(coin.isMature(created + 100));
        using namespace dinero::consensus;
        EXPECT_EQ(EvaluateUtreexoStatelessMaturity(created + 99, created, true),
                  UtreexoStatelessMaturityStatus::IMMATURE_COINBASE);
        EXPECT_EQ(EvaluateUtreexoStatelessMaturity(created + 100, created, true),
                  UtreexoStatelessMaturityStatus::SPENDABLE);
    }
    params.sixty_second_activation_height = saved;
    SelectParams(original);
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

// Spend authority is a STRICT SUPERSET of cv-binding: it adds the pk_d = s·G
// ownership proof on top of every cv-bound constraint. An auth-without-cv
// combination would prove ownership while leaving value unbound, reopening the
// mint-from-nothing hole cv-binding closed. The ordering must be unreachable
// by configuration.
TEST(ChainParamsSelection, SpendAuthMustNotPrecedeCvBinding) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    // Shipped regtest satisfies the invariant (both dormant at UINT32_MAX).
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));

    regtest->shielded_input_binding_activation_height = 300;
    regtest->shielded_cv_binding_activation_height = 400;
    regtest->shielded_epoch_reset_height = 400;   // keep the equality check happy
    regtest->shielded_spend_auth_activation_height = 399;  // one block too early
    regtest->shielded_spend_auth_epoch_reset_height = 399;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);

    // Equal is legal — both rules may activate at the same height.
    regtest->shielded_spend_auth_activation_height = 401;
    regtest->shielded_spend_auth_epoch_reset_height = 401;
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));
}

TEST(ChainParamsSelection, SpendAuthResetMustMatchActivationAndBeDistinct) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    regtest->shielded_input_binding_activation_height = 300;
    regtest->shielded_cv_binding_activation_height = 400;
    regtest->shielded_epoch_reset_height = 400;
    regtest->shielded_spend_auth_activation_height = 500;
    regtest->shielded_spend_auth_epoch_reset_height = 499;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);

    regtest->shielded_spend_auth_epoch_reset_height = 500;
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));

    regtest->shielded_spend_auth_activation_height = 400;
    regtest->shielded_spend_auth_epoch_reset_height = 400;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);
}

TEST(ChainParamsSelection, OutgoingRecoveryCannotPrecedeSpendAuthority) {
    SelectParams(Chain::REGTEST);
    ChainParams* regtest = &dinero::MutableParams();
    const ScopedShieldedHeights restore(regtest);

    regtest->shielded_input_binding_activation_height = 300;
    regtest->shielded_cv_binding_activation_height = 400;
    regtest->shielded_epoch_reset_height = 400;
    regtest->shielded_spend_auth_activation_height = 500;
    regtest->shielded_spend_auth_epoch_reset_height = 500;

    regtest->shielded_outgoing_recovery_activation_height = 499;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);
    regtest->shielded_outgoing_recovery_activation_height = 500;
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));
    regtest->shielded_outgoing_recovery_activation_height = UINT32_MAX;
    EXPECT_NO_THROW(SelectParams(Chain::REGTEST));

    regtest->shielded_spend_auth_activation_height = UINT32_MAX;
    regtest->shielded_spend_auth_epoch_reset_height = UINT32_MAX;
    regtest->shielded_outgoing_recovery_activation_height = 500;
    EXPECT_THROW(SelectParams(Chain::REGTEST), std::runtime_error);
}

// Spend authority and outgoing recovery remain opt-in on test networks.
// Regtest is deliberately not active by default: explicit command-line
// heights make lifecycle rehearsals opt-in without weakening the production
// sentinel or making unrelated hand-built block fixtures cross the fork.
TEST(ChainParamsSelection, SpendAuthDormantOnTestNetworks) {
    for (const Chain chain : {Chain::TESTNET, Chain::REGTEST}) {
        SelectParams(chain);
        EXPECT_EQ(Params().shielded_spend_auth_activation_height, UINT32_MAX)
            << "spend authority must stay dormant until activation review";
        EXPECT_EQ(Params().shielded_spend_auth_epoch_reset_height, UINT32_MAX);
        EXPECT_EQ(Params().shielded_outgoing_recovery_activation_height,
                  UINT32_MAX);
    }
}

TEST(ChainParamsSelection, WalletWaitsForCommittedAuthResetAndRelocksOnReorg) {
    using dinero::consensus::shielded::WalletAuthEpochReady;
    EXPECT_FALSE(WalletAuthEpochReady(109999, 110000, 110000));
    EXPECT_TRUE(WalletAuthEpochReady(110000, 110000, 110000));
    EXPECT_TRUE(WalletAuthEpochReady(110001, 110000, 110000));
    EXPECT_FALSE(WalletAuthEpochReady(109999, 110000, 110000));
    EXPECT_FALSE(WalletAuthEpochReady(UINT32_MAX, UINT32_MAX, UINT32_MAX));
    EXPECT_FALSE(WalletAuthEpochReady(110000, 110000, 109999));
}

TEST(ChainParamsSelection, MainnetAuthCutoverPreservesHistoricalRules) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(Params().shielded_activation_height, 8650U);
    EXPECT_EQ(Params().shielded_input_binding_activation_height, 32300U);
    EXPECT_EQ(Params().shielded_cv_binding_activation_height, 61000U);
    EXPECT_EQ(Params().shielded_epoch_reset_height, 61000U);
    EXPECT_EQ(Params().shielded_spend_auth_activation_height, 110000U);
    EXPECT_EQ(Params().shielded_private_covenant_activation_height, 110000U);
    EXPECT_EQ(Params().shielded_spend_auth_epoch_reset_height, 110000U);
    EXPECT_EQ(Params().shielded_outgoing_recovery_activation_height, 110000U);
    EXPECT_EQ(Params().shielded_coinbase_reject_activation_height, 110000U);
    EXPECT_EQ(Params().state_commitment_activation_height, 111000U);
    EXPECT_GT(Params().state_commitment_activation_height, Params().shielded_spend_auth_epoch_reset_height);
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
