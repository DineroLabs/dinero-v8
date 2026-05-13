// Copyright (c) 2026 Dinero Labs.
//
// Regression for consensus replay callers: a shielded ValidationContext
// must carry the transaction sighash used to sign the bundle.

#include <gtest/gtest.h>

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_validation.h"
#include "primitives/transaction.h"

#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::Hash;
using shielded::NoteCommitment;
using shielded::OutputPublicInputs;
using shielded::OutputWitness;
using shielded::PoseidonHash2;
using shielded::ProveOutput;
using shielded::ShieldedBundle;
using shielded::ShieldedValidationError;
using shielded::ValidateShieldedBundle;

Hash MakeHash(uint8_t seed, uint8_t tail = 0xC7) {
    Hash h{};
    h[0]  = seed;
    h[31] = tail;
    return h;
}

Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

TEST(ShieldedValidationContextTest, MissingTxSighashRejectsRealBindingSigBundle) {
    constexpr uint64_t kValue = 100'000'000;

    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.lockTime = 0;
    const Hash tx_sighash = shielded::ComputeShieldedTxSighash(tx);

    const Hash sk = MakeHash(0x11);
    OutputWitness ow;
    ow.value      = ValueAsHash(kValue);
    ow.public_key = PoseidonHash2(sk, Hash{});
    ow.randomness = MakeHash(0x12);
    ow.d          = Hash{};

    OutputPublicInputs opi;
    opi.commitment = NoteCommitment(ow.d, ow.public_key, ow.value, ow.randomness);
    auto output_proof = ProveOutput(ow, opi, nullptr);
    ASSERT_FALSE(output_proof.empty());

    shielded::PlannedOutput po;
    po.commitment     = opi.commitment;
    po.value_una      = kValue;
    po.rcv            = MakeHash(0x13);
    po.encrypted_note = std::vector<uint8_t>(96, 0xAA);
    po.output_proof   = std::move(output_proof);
    po.nonce          = MakeHash(0x14);

    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({}, {po}, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    shielded::CommitmentTree tree;

    // Production-shape context: real tx_sighash → bundle validates Ok.
    shielded::ValidationContext ctx_with_sighash{
        /*nullifier_set=*/nullptr,
        /*commitment_tree=*/&tree,
        /*block_height=*/100,
        /*transparent_value_delta=*/static_cast<int64_t>(kValue),
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        /*tx_sighash=*/tx_sighash,
    };
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx_with_sighash),
              ShieldedValidationError::Ok);

    // Reindex-bug shape: tx_sighash silently defaulted to Hash{}.
    // Re-construct (rather than mutate) so the test mirrors how a
    // production caller would have to obtain a wrong context: by
    // explicitly passing zeros, no longer by forgetting a field.
    shielded::ValidationContext ctx_zero_sighash{
        /*nullifier_set=*/nullptr,
        /*commitment_tree=*/&tree,
        /*block_height=*/100,
        /*transparent_value_delta=*/static_cast<int64_t>(kValue),
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        /*tx_sighash=*/Hash{},
    };
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx_zero_sighash),
              ShieldedValidationError::BindingSigInvalid);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
