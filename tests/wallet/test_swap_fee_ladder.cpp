// Pre-signed fee ladders: every rung valid through consensus, same payout,
// strictly rising fees, exact sizing, caps, and no signature reuse across rungs.
#include "wallet/swap/fee_ladder.h"

#include "consensus/script.h"
#include "consensus/script_verify.h"
#include "consensus/utxo_entry.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <stdexcept>
#include <string>

namespace {

using namespace dinero;
using namespace dinero::swap;
namespace op = dinero::consensus;

struct Key {
    secp256k1_keypair keypair{};
    Bytes32 xonly{};
};

Key MakeKey(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    std::array<uint8_t, 32> secret{};
    secret.back() = scalar;
    Key key;
    EXPECT_EQ(secp256k1_keypair_create(secp, &key.keypair, secret.data()), 1);
    secp256k1_xonly_pubkey x;
    EXPECT_EQ(secp256k1_keypair_xonly_pub(secp, &x, nullptr, &key.keypair), 1);
    EXPECT_EQ(secp256k1_xonly_pubkey_serialize(secp, key.xonly.data(), &x), 1);
    return key;
}

std::array<uint8_t, 64> Sign(const Key& key, const Bytes32& msg) {
    std::array<uint8_t, 64> sig{};
    const std::array<uint8_t, 32> aux{};
    EXPECT_EQ(secp256k1_schnorrsig_sign32(crypto::GetSecp256k1ContextSignVerify(), sig.data(),
                                          msg.data(), &key.keypair, aux.data()),
              1);
    return sig;
}

struct Fixture {
    Key bob = MakeKey(2), alice = MakeKey(3);
    std::vector<uint8_t> preimage = std::vector<uint8_t>(32, 0x5a);
    DinHtlcTerms terms;
    DinHtlcOutput htlc;
    FundingOutput funding;
    std::vector<uint8_t> payout;
    FeeLadderPolicy policy;

    Fixture() {
        crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(terms.payment_hash.data());
        terms.claim_pubkey = bob.xonly;
        terms.refund_pubkey = alice.xonly;
        terms.refund_locktime_unix = 1'800'000'000;
        htlc = BuildDinHtlc(terms);
        funding.txid = TxId(uint256::FromHexUnsafe(std::string(64, '2')));
        funding.vout = 1;
        funding.value = AmountUna::Una(10'000'000);
        funding.script_pubkey = htlc.script_pubkey;
        payout = {op::OP_1, 0x20};
        payout.insert(payout.end(), 32, 0x77);
        policy.start_feerate_una_per_vb = 2;
        policy.max_rungs = 8;
        policy.max_fee_una = 1'000'000;
        policy.min_payout_una = 1'000;
    }

    bool Verify(const Transaction& tx) const {
        std::vector<op::UTXOEntry> prevouts(1);
        prevouts[0].value = funding.value;
        prevouts[0].scriptPubKey = funding.script_pubkey;
        std::string error;
        return op::ScriptVerifier::VerifyTaproot(tx, 0, prevouts, error, op::SCRIPT_VERIFY_STANDARD);
    }
};

void ExpectWellFormedLadder(const std::vector<FeeRung>& rungs, const Fixture& f) {
    for (size_t i = 0; i < rungs.size(); ++i) {
        const auto& r = rungs[i];
        ASSERT_EQ(r.tx.vout.size(), 1U);
        EXPECT_EQ(r.tx.vout[0].scriptPubKey, f.payout) << "rung " << i << " pays elsewhere";
        EXPECT_EQ(r.tx.vout[0].value.GetUna() + r.fee_una, f.funding.value.GetUna());
        EXPECT_LT(r.tx.vin[0].sequence, 0xfffffffeU) << "rung " << i << " not replaceable";
        EXPECT_LE(r.fee_una, f.policy.max_fee_una);
        EXPECT_GE(r.tx.vout[0].value.GetUna(), f.policy.min_payout_una);
        if (i > 0) {
            EXPECT_GT(r.fee_una, rungs[i - 1].fee_una);
            EXPECT_EQ(r.feerate_una_per_vb, 2 * rungs[i - 1].feerate_una_per_vb);
        }
    }
}

}  // namespace

TEST(SwapFeeLadder, EveryClaimRungIsValidAndExactlySized) {
    Fixture f;
    auto rungs = BuildDinClaimLadder(f.htlc, f.funding, f.payout, f.policy);
    ASSERT_EQ(rungs.size(), 8U);
    EXPECT_EQ(rungs[0].feerate_una_per_vb, 2U);
    ExpectWellFormedLadder(rungs, f);
    for (auto& r : rungs) {
        EXPECT_EQ(r.sighash, DinClaimSighash(r.tx, f.funding, f.htlc));
        SetDinClaimWitness(r.tx, f.terms, f.htlc, Sign(f.bob, r.sighash), f.preimage);
        EXPECT_TRUE(f.Verify(r.tx));
        EXPECT_EQ(r.fee_una, r.feerate_una_per_vb * r.tx.GetVirtualSize())
            << "fee must match the real signed size";
    }
}

TEST(SwapFeeLadder, EveryRefundRungIsValidAndExactlySized) {
    Fixture f;
    auto rungs = BuildDinRefundLadder(f.terms, f.htlc, f.funding, f.payout, f.policy);
    ASSERT_EQ(rungs.size(), 8U);
    ExpectWellFormedLadder(rungs, f);
    for (auto& r : rungs) {
        EXPECT_EQ(r.tx.lockTime, f.terms.refund_locktime_unix);
        EXPECT_EQ(r.sighash, DinRefundSighash(r.tx, f.funding, f.htlc));
        SetDinRefundWitness(r.tx, f.htlc, Sign(f.alice, r.sighash));
        EXPECT_TRUE(f.Verify(r.tx));
        EXPECT_EQ(r.fee_una, r.feerate_una_per_vb * r.tx.GetVirtualSize());
    }
}

TEST(SwapFeeLadder, ASignatureForOneRungDoesNotValidateAnother) {
    Fixture f;
    auto rungs = BuildDinClaimLadder(f.htlc, f.funding, f.payout, f.policy);
    ASSERT_GE(rungs.size(), 2U);
    const auto sig0 = Sign(f.bob, rungs[0].sighash);
    SetDinClaimWitness(rungs[1].tx, f.terms, f.htlc, sig0, f.preimage);
    EXPECT_FALSE(f.Verify(rungs[1].tx));
}

TEST(SwapFeeLadder, CapAndMinimumPayoutEndTheLadderEarly) {
    Fixture f;
    f.policy.max_fee_una = 2'000;  // only the lowest rungs fit
    const auto capped = BuildDinClaimLadder(f.htlc, f.funding, f.payout, f.policy);
    ASSERT_FALSE(capped.empty());
    EXPECT_LT(capped.size(), 8U);
    ExpectWellFormedLadder(capped, f);

    f.policy.max_fee_una = 1'000'000;
    f.policy.min_payout_una = f.funding.value.GetUna() - 3'000;  // payout floor binds
    const auto floored = BuildDinClaimLadder(f.htlc, f.funding, f.payout, f.policy);
    ASSERT_FALSE(floored.empty());
    EXPECT_LT(floored.size(), 8U);
    ExpectWellFormedLadder(floored, f);
}

TEST(SwapFeeLadder, InvalidPoliciesAreRefused) {
    Fixture f;
    auto expect_refused = [&](FeeLadderPolicy p, const char* why) {
        EXPECT_THROW(BuildDinClaimLadder(f.htlc, f.funding, f.payout, p), std::invalid_argument) << why;
    };
    auto p = f.policy; p.start_feerate_una_per_vb = 0; expect_refused(p, "zero feerate");
    p = f.policy; p.max_rungs = 0; expect_refused(p, "no rungs");
    p = f.policy; p.max_rungs = 17; expect_refused(p, "too many rungs");
    p = f.policy; p.max_fee_una = f.funding.value.GetUna(); expect_refused(p, "cap >= funding value");
    p = f.policy; p.max_fee_una = 10; expect_refused(p, "first rung over the cap");
}
