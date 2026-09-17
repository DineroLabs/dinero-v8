#include <gtest/gtest.h>
#include "consensus/shielded/resource_limits.h"
#include "p2p/structural_validator.h"

using namespace dinero;
namespace sh = dinero::consensus::shielded;

namespace {
Transaction ShieldedTx(size_t spends = 1, size_t outputs = 2) {
    Transaction tx;
    tx.version = Transaction::TX_VERSION_SHIELDED_V2;
    tx.witness_version = 0;
    tx.SetExplicitFee(1'000'000);
    sh::ShieldedBundle bundle;
    for (size_t i=0; i<spends; ++i) {
        sh::ShieldedSpend spend;
        spend.nullifier[0] = i+1;
        spend.zk_proof = {6};
        bundle.spends.push_back(spend);
    }
    for (size_t i=0; i<outputs; ++i) {
        sh::ShieldedOutput output;
        output.commitment[0] = i+1;
        output.zk_proof = {4};
        bundle.outputs.push_back(output);
    }
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    return tx;
}
Transaction SizedTx(size_t bytes) {
    auto tx = ShieldedTx();
    sh::ShieldedBundle bundle;
    EXPECT_EQ(sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle), sh::BundleDecodeError::Ok);
    // All tested lengths use the same five-byte CompactSize prefix.
    bundle.spends[0].zk_proof.resize(70'000, 6);
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    bundle.spends[0].zk_proof.resize(70'000 + bytes - tx.GetSize(), 6);
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    EXPECT_EQ(tx.GetSize(), bytes);
    return tx;
}
}

TEST(ShieldedResources, TransactionBoundaryAndActivationReorg) {
    for (size_t bytes : {sh::kAuthMaxTxBytes-1, sh::kAuthMaxTxBytes, sh::kAuthMaxTxBytes+1}) {
        auto tx = SizedTx(bytes);
        size_t proofs = 0;
        std::string error;
        EXPECT_EQ(sh::CheckAuthTransactionResources(tx, 100, 100, proofs, error), bytes <= sh::kAuthMaxTxBytes);
        EXPECT_EQ(sh::CheckTxResourceEnvelope(tx, true, error), bytes <= sh::kAuthMaxTxBytes);
        EXPECT_FALSE(sh::CheckTxResourceEnvelope(tx, sh::AuthResourcesActive(99,100), error));
        EXPECT_EQ(sh::CheckTxResourceEnvelope(tx, sh::AuthResourcesActive(100,100), error), bytes <= sh::kAuthMaxTxBytes);
        // Dormancy is explicit even at the sentinel height.
        EXPECT_FALSE(sh::CheckTxResourceEnvelope(tx, sh::AuthResourcesActive(UINT32_MAX, UINT32_MAX), error));
    }
}
TEST(ShieldedResources, HistoricalConsensusIsUnchanged) {
    std::string error;
    auto tx = SizedTx(sh::kAuthMaxTxBytes+1);
    size_t proofs = 0;
    EXPECT_TRUE(sh::CheckAuthTransactionResources(tx, 99, 100, proofs, error));
    EXPECT_TRUE(sh::CheckAuthTransactionResources(tx, UINT32_MAX, UINT32_MAX, proofs, error));
}
TEST(ShieldedResources, AuthActivationRequiresTxidCommittingV6) {
    auto legacy = ShieldedTx();
    legacy.version = Transaction::TX_VERSION_SHIELDED;
    std::string error;
    size_t proofs = 0;
    EXPECT_TRUE(sh::CheckAuthTransactionResources(legacy, 99, 100, proofs, error));
    EXPECT_FALSE(sh::CheckAuthTransactionResources(legacy, 100, 100, proofs, error));
    EXPECT_EQ(error, "shielded-auth-requires-tx-v6");

    auto oversized_legacy = SizedTx(168'000);
    oversized_legacy.version = Transaction::TX_VERSION_SHIELDED;
    EXPECT_EQ(sh::WireTxByteLimit(oversized_legacy.Serialize()),
              dinero::consensus::MAX_TX_SIZE);
    EXPECT_EQ(sh::WireTxByteLimit(SizedTx(168'000).Serialize()),
              sh::kAuthMaxTxBytes);
}
TEST(ShieldedResources, LegacyRelayByteBoundary) {
    std::string error;
    for (size_t bytes : {99'999u,100'000u,100'001u}) {
        auto tx = SizedTx(bytes);
        EXPECT_EQ(sh::CheckTxResourceEnvelope(tx, false, error), bytes <= 100'000);
    }
}
TEST(ShieldedResources, TransparentTransactionDoesNotGetShieldedAllowance) {
    Transaction tx;
    tx.version = 2;
    TxInput in;
    in.prevout.vout=1;
    tx.vin.push_back(in);
    tx.vout.emplace_back(AmountUna::Una(1), std::vector<uint8_t>(100'000, 0x51));
    std::string error;
    EXPECT_FALSE(sh::CheckTxResourceEnvelope(tx, true, error));
    tx.shielded_bundle_bytes = {1};
    EXPECT_FALSE(sh::HasShieldedResources(tx));
}
TEST(ShieldedResources, BundleCountsBoundProofWork) {
    std::string error;
    for (size_t spends : {3u,4u,5u}) {
        for (size_t outputs : {1u,2u,3u}) {
            auto tx = ShieldedTx(spends,outputs);
            size_t proofs=0;
            EXPECT_EQ(sh::CheckAuthTransactionResources(tx,100,100,proofs,error), spends<=4 && outputs<=2);
        }
    }
}
TEST(ShieldedResources, BlockProofBoundaryAndDormancy) {
    std::string error;
    for (size_t count : {7u,8u,9u}) {
        std::vector<Transaction> txs(count, ShieldedTx(0,1));
        EXPECT_EQ(sh::CheckAuthBlockResources(txs,100,100,error), count<=8);
        EXPECT_TRUE(sh::CheckAuthBlockResources(txs,99,100,error));
        EXPECT_TRUE(sh::CheckAuthBlockResources(txs,UINT32_MAX,UINT32_MAX,error));
    }
}
TEST(ShieldedResources, StructuralRelayAcceptsShieldedOnlyAndBoundsAllocation) {
    p2p::StructuralValidator validator;
    EXPECT_TRUE(validator.validateTx(ShieldedTx().Serialize()).ok);
    EXPECT_TRUE(validator.validateTx(SizedTx(168'000).Serialize()).ok);
    EXPECT_TRUE(validator.validateTx(SizedTx(sh::kAuthMaxTxBytes).Serialize()).ok);
    EXPECT_FALSE(validator.validateTx(SizedTx(sh::kAuthMaxTxBytes+1).Serialize()).ok);
    auto legacy_oversized = SizedTx(168'000);
    legacy_oversized.version = Transaction::TX_VERSION_SHIELDED;
    EXPECT_FALSE(validator.validateTx(legacy_oversized.Serialize()).ok);
    auto raw = ShieldedTx().Serialize();
    raw.push_back(0);
    EXPECT_FALSE(validator.validateTx(raw).ok);
    auto empty = ShieldedTx();
    empty.shielded_bundle_bytes.clear();
    EXPECT_FALSE(validator.validateTx(empty.Serialize()).ok);
}

TEST(ShieldedResources, BlockShieldedByteBoundary) {
    std::string error;
    for (size_t total : {999'999u,1'000'000u,1'000'001u}) {
        std::vector<Transaction> txs{SizedTx(500'000), SizedTx(total-500'000)};
        EXPECT_EQ(sh::CheckAuthBlockResources(txs,100,100,error), total<=1'000'000);
        EXPECT_TRUE(sh::CheckAuthBlockResources(txs,99,100,error));
    }
}

TEST(ShieldedResources, FailedBlockAccumulationDoesNotMutateUsage) {
    std::string error;
    sh::AuthBlockResourceUsage usage{7, 900'000};
    const auto before = usage;
    EXPECT_FALSE(sh::AccumulateAuthBlockResources(
        ShieldedTx(0, 2), 100, 100, usage, error));
    EXPECT_EQ(error, "shielded-block-proof-limit");
    EXPECT_EQ(usage.proofs, before.proofs);
    EXPECT_EQ(usage.shielded_bytes, before.shielded_bytes);

    error.clear();
    usage = {0, 900'000};
    const auto byte_before = usage;
    EXPECT_FALSE(sh::AccumulateAuthBlockResources(
        SizedTx(100'001), 100, 100, usage, error));
    EXPECT_EQ(error, "shielded-block-byte-limit");
    EXPECT_EQ(usage.proofs, byte_before.proofs);
    EXPECT_EQ(usage.shielded_bytes, byte_before.shielded_bytes);
}

TEST(ShieldedResources, PackageProfileIsScopedAndAccommodatesStandaloneMaximum) {
    EXPECT_EQ(sh::PackageByteLimit(false,true), 103'424u);
    EXPECT_EQ(sh::PackageByteLimit(true,false), 103'424u);
    EXPECT_EQ(sh::PackageByteLimit(true,true), 600'000u);
    EXPECT_LE(sh::kAuthMaxTxBytes, sh::PackageByteLimit(true,true));
    EXPECT_EQ(sh::kMaxPackageTransactions, 25u);
}

#ifdef DINERO_ENABLE_COMPACT_REGTEST
namespace {
constexpr sh::CompactRegtestRules kCompactRules{true, 124};
Transaction CompactSizedTx(size_t bytes) {
    auto tx = SizedTx(bytes);
    tx.version = Transaction::TX_VERSION_COMPACT_REGTEST;
    return tx;
}
Transaction CompactShape(size_t spends, size_t outputs) {
    auto tx = ShieldedTx(spends, outputs);
    tx.version = Transaction::TX_VERSION_COMPACT_REGTEST;
    return tx;
}
}

TEST(CompactResources, ByteBoundaryIncludesCanonicalWireAndWeight) {
    p2p::StructuralValidator relay;
    for (size_t bytes : {511'999u, 512'000u, 512'001u}) {
        SCOPED_TRACE(bytes);
        auto tx = CompactSizedTx(bytes);
        std::string error; size_t proofs = 999;
        EXPECT_EQ(tx.GetSize(), bytes);
        // Empty-vin compact wire has a mandatory two-byte witness marker.
        // Weight is 4*wire-6; the byte cap binds before the 2,048,000 weight
        // cap. A purported independently reachable exact-weight case would
        // necessarily exceed the byte limit or omit the required marker.
        EXPECT_EQ(tx.GetWeight(), 4 * bytes - 6);
        EXPECT_LT(4 * sh::kAuthMaxTxBytes - 6, sh::kAuthMaxTxWeight);
        EXPECT_EQ(sh::CheckAuthTransactionResources(tx, 124, 2, proofs, error, kCompactRules),
                  bytes <= 512'000);
        if (bytes <= 512'000) EXPECT_EQ(proofs, 3u); else EXPECT_EQ(proofs, 0u);
        EXPECT_EQ(relay.validateTx(tx.Serialize(true)).ok, bytes <= 512'000);
        EXPECT_FALSE(sh::CheckAuthTransactionResources(tx, 123, 2, proofs, error, kCompactRules));
        EXPECT_EQ(proofs, 0u);
    }
}

TEST(CompactResources, ExactCountsAndPlusOneCannotBuyExtraProofSlots) {
    for (size_t spends : {0u, 3u, 4u, 5u}) {
        for (size_t outputs : {0u, 1u, 2u, 3u}) {
            if (spends + outputs == 0) continue;
            auto tx = CompactShape(spends, outputs);
            SCOPED_TRACE(std::to_string(spends) + "/" + std::to_string(outputs));
            std::string error; size_t proofs = 999;
            const bool accepted = spends <= 4 && outputs <= 2;
            EXPECT_EQ(sh::CheckAuthTransactionResources(tx, 124, 2, proofs, error, kCompactRules), accepted);
            EXPECT_EQ(proofs, accepted ? spends + outputs : 0);
        }
    }
    for (size_t count : {7u, 8u, 9u}) {
        std::vector<Transaction> txs(count, CompactShape(0, 1));
        std::string error;
        EXPECT_EQ(sh::CheckAuthBlockResources(txs, 124, 2, error, kCompactRules), count <= 8);
        EXPECT_FALSE(sh::CheckAuthBlockResources(txs, 123, 2, error, kCompactRules));
    }
    // Malformed one-byte proof payloads still consume their claimed slots.
    // Resource acceptance is not cryptographic acceptance: validation is a
    // separate gate, exercised by the fixed real-proof vectors.
    sh::AuthBlockResourceUsage usage;
    std::string error;
    ASSERT_TRUE(sh::AccumulateAuthBlockResources(CompactShape(4, 2), 124, 2, usage, error, kCompactRules));
    ASSERT_TRUE(sh::AccumulateAuthBlockResources(CompactShape(0, 2), 124, 2, usage, error, kCompactRules));
    const auto before = usage;
    EXPECT_EQ(usage.proofs, 8u);
    EXPECT_FALSE(sh::AccumulateAuthBlockResources(CompactShape(0, 1), 124, 2, usage, error, kCompactRules));
    EXPECT_EQ(error, "shielded-block-proof-limit");
    EXPECT_EQ(usage.proofs, before.proofs); EXPECT_EQ(usage.shielded_bytes, before.shielded_bytes);
}

TEST(CompactResources, AggregateBytesAndFailedAccumulationAreExact) {
    for (size_t total : {999'999u, 1'000'000u, 1'000'001u}) {
        auto first = CompactSizedTx(500'000);
        auto second = CompactSizedTx(total - 500'000);
        sh::AuthBlockResourceUsage usage;
        std::string error;
        ASSERT_TRUE(sh::AccumulateAuthBlockResources(first, 124, 2, usage, error, kCompactRules));
        const auto before = usage;
        const bool accepted = total <= 1'000'000;
        EXPECT_EQ(sh::AccumulateAuthBlockResources(second, 124, 2, usage, error, kCompactRules), accepted);
        EXPECT_EQ(usage.shielded_bytes, accepted ? total : before.shielded_bytes);
        EXPECT_EQ(usage.proofs, accepted ? 6u : before.proofs);
        if (!accepted) EXPECT_EQ(error, "shielded-block-byte-limit");
    }
    // Arithmetic must remain fail-closed even if supplied counters are invalid.
    for (auto usage : {sh::AuthBlockResourceUsage{SIZE_MAX, 0},
                       sh::AuthBlockResourceUsage{0, SIZE_MAX}}) {
        const auto before = usage;
        std::string error;
        EXPECT_FALSE(sh::AccumulateAuthBlockResources(CompactShape(0, 1), 124, 2, usage, error, kCompactRules));
        EXPECT_EQ(usage.proofs, before.proofs); EXPECT_EQ(usage.shielded_bytes, before.shielded_bytes);
    }
}
#endif
