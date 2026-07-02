// #353: coin-selection / spendable-balance must exclude accumulator-anchored coins
// (not in the node's active set) so the auto-picker can't grab an un-spendable input,
// while keeping them visible in the total. Pure/dependency-injected — no live node.
#include <gtest/gtest.h>

#include "wallet/wallet_spendability.h"

using dinero::uint256;
using dinero::AmountUna;
using dinero::CanonicalWalletUTXO;
using dinero::wallet::PartitionBySpendability;

namespace {

CanonicalWalletUTXO mk(uint8_t seed, uint64_t una) {
    CanonicalWalletUTXO u;
    uint256 t;
    t.data[0] = seed;
    u.txid = t;
    u.vout = 0;
    u.value = AmountUna::Una(una);
    return u;
}

// The core fix: coins the node can't spend go to `anchored` (excluded from the
// spendable set + total), not `spendable`. This is what stops the send footgun.
TEST(WalletSpendability, AnchoredCoinsExcludedFromSpendableButKept) {
    std::vector<CanonicalWalletUTXO> utxos = { mk(1, 100), mk(2, 200), mk(3, 300) };
    // Node can spend #1 and #3; #2 is anchored (hasCoin == false).
    auto check = [](const uint256& txid, uint32_t) -> bool { return txid.data[0] != 2; };
    auto p = PartitionBySpendability(utxos, check);
    EXPECT_EQ(p.spendable.size(), 2u);
    EXPECT_EQ(p.anchored.size(), 1u);
    EXPECT_EQ(p.spendable_una, 400u);   // 100 + 300, NOT 600
    EXPECT_EQ(p.anchored_una, 200u);
    EXPECT_EQ(p.anchored[0].txid.data[0], 2);
}

// Every coin anchored (e.g. a snapshot node with no backfilled proofs) → 0 spendable,
// but the funds are still accounted (visible in total via anchored_una).
TEST(WalletSpendability, AllAnchoredMeansZeroSpendable) {
    std::vector<CanonicalWalletUTXO> utxos = { mk(1, 100), mk(2, 200) };
    auto check = [](const uint256&, uint32_t) -> bool { return false; };
    auto p = PartitionBySpendability(utxos, check);
    EXPECT_TRUE(p.spendable.empty());
    EXPECT_EQ(p.spendable_una, 0u);
    EXPECT_EQ(p.anchored_una, 300u);
}

// Legacy safety: nodes that don't distinguish an active set pass a null check and get
// the old behavior (everything spendable) — no regression on full nodes.
TEST(WalletSpendability, NullCheckTreatsAllSpendable) {
    std::vector<CanonicalWalletUTXO> utxos = { mk(1, 100), mk(2, 200) };
    auto p = PartitionBySpendability(utxos, nullptr);
    EXPECT_EQ(p.spendable.size(), 2u);
    EXPECT_TRUE(p.anchored.empty());
    EXPECT_EQ(p.spendable_una, 300u);
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
