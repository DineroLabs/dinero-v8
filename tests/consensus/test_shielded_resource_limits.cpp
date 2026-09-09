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
