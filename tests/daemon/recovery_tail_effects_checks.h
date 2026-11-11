#pragma once
#include "wallet/recovery_tail_effects.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
// v6-recovery stage C step 1: the typed-tail extractor over the composed
// account replay. Structural refusals (fabricated linkage, capacity) cannot be
// unit-triggered here because the replay type is only constructible through
// real verification; they are exercised by the extractor's own Require-driven
// inputs and by the stage-D composed fixture.
TEST(RecoveryTailEffects, ExtractsSelectedTailWithKnownSpendAndChange) {
    CanonicalRecoveryFixture f(true);
    f.MineEmpty();
    const auto receiver = orchard::WalletKeys::FromSeed(std::array<uint8_t, 64>{61}, 0);
    const auto [body, bundle] = f.Shield(receiver);
    (void)bundle;
    (void)f.Mine(body);
    const auto view = f.f.service->getRuntimeAccountReplay();
    ASSERT_TRUE(view.ok());
    const auto effects = ReadRecoveryTailEffects(**view);
    const auto activation = (*view)->Profile().activation_height;
    EXPECT_EQ(effects.base_height, activation - 1);
    const auto* tip = f.f.service->GetActiveTip();
    ASSERT_NE(tip, nullptr);
    ASSERT_EQ(effects.blocks.size(), size_t(tip->height) - effects.base_height);
    // Contiguous linkage from the activation parent to the selected tip, with
    // a historical coinbase (no spends, at least one output) leading each block.
    uint256 prior = effects.base_hash;
    uint32_t height = effects.base_height;
    for (const auto& block : effects.blocks) {
        EXPECT_EQ(block.parent, prior);
        EXPECT_EQ(block.height, height + 1);
        EXPECT_GT(block.timestamp, 0u);
        ASSERT_FALSE(block.transactions.empty());
        EXPECT_TRUE(block.transactions.front().spends.empty());
        EXPECT_FALSE(block.transactions.front().outputs.empty());
        prior = block.hash;
        height = block.height;
    }
    EXPECT_EQ(prior, tip->hash);
    // The shield's transparent facts, from the fixture's own ground truth: it
    // spends the block-1 coinbase coin and returns known change to f.f.script.
    const OutPoint funded(f.f.blocks[1].vtx.front().GetTxid(), 0);
    const auto& coin = f.f.replay->ProvenUtxos().at(funded);
    const auto& tip_block = effects.blocks.back();
    ASSERT_EQ(tip_block.transactions.size(), 2u);
    const auto& shield = tip_block.transactions[1];
    EXPECT_EQ(shield.txid.AsUint256(), body.GetTxid().AsUint256());
    ASSERT_EQ(shield.spends.size(), 1u);
    EXPECT_EQ(shield.spends[0], funded);
    ASSERT_EQ(shield.outputs.size(), 1u);
    EXPECT_EQ(shield.outputs[0].value_una, coin.value.GetUna() - 600000u);
    EXPECT_EQ(shield.outputs[0].script_pub_key, f.f.script);
    EXPECT_FALSE(shield.outputs[0].is_confidential);
}
TEST(RecoveryTailEffects, FoldsDisconnectIntoSelectedChain) {
    CanonicalRecoveryFixture f(true);
    f.MineEmpty();
    const auto* detached_tip = f.f.service->GetActiveTip();
    ASSERT_NE(detached_tip, nullptr);
    const auto detached = detached_tip->hash;
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service, f.f.service->GetActiveTip()));
    // Replace the detached block with one that cannot collide: it carries the
    // shield transaction, so the replacement body differs from the empty one.
    const auto receiver = orchard::WalletKeys::FromSeed(std::array<uint8_t, 64>{67}, 0);
    const auto [body, bundle] = f.Shield(receiver);
    (void)bundle;
    (void)f.Mine(body);
    const auto view = f.f.service->getRuntimeAccountReplay();
    ASSERT_TRUE(view.ok());
    const auto effects = ReadRecoveryTailEffects(**view);
    ASSERT_FALSE(effects.blocks.empty());
    for (const auto& block : effects.blocks) EXPECT_NE(block.hash, detached);
    EXPECT_EQ(effects.blocks.back().hash, f.f.service->GetActiveTip()->hash);
    // The disconnected block's effects are gone; the replacement's shield is
    // present, so the folded chain is exactly the selected one.
    ASSERT_EQ(effects.blocks.back().transactions.size(), 2u);
    EXPECT_EQ(effects.blocks.back().transactions[1].txid.AsUint256(), body.GetTxid().AsUint256());
}
}  // namespace dinero
#endif
