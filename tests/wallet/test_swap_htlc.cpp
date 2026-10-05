// DIN <-> BTC swap HTLC builders, checked against the real consensus verifier.
// Design: docs/design/din-btc-atomic-swaps.md. Every Dinero spend here goes
// through ScriptVerifier::VerifyTaproot — the same path blocks use.
#include "wallet/swap/htlc.h"

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

constexpr uint32_t kRefundLock = 1'800'000'000;  // a Unix timestamp (2027)

swap::Bytes32 Sha256(const std::vector<uint8_t>& data) {
    swap::Bytes32 out{};
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(out.data());
    return out;
}

struct Key {
    secp256k1_keypair keypair{};
    swap::Bytes32 xonly{};
};

Key MakeKey(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    std::array<uint8_t, 32> secret{};
    secret.back() = scalar;
    Key key;
    EXPECT_EQ(secp256k1_keypair_create(secp, &key.keypair, secret.data()), 1);
    secp256k1_xonly_pubkey xonly;
    EXPECT_EQ(secp256k1_keypair_xonly_pub(secp, &xonly, nullptr, &key.keypair), 1);
    EXPECT_EQ(secp256k1_xonly_pubkey_serialize(secp, key.xonly.data(), &xonly), 1);
    return key;
}

std::array<uint8_t, 64> Sign(const Key& key, const swap::Bytes32& message) {
    std::array<uint8_t, 64> sig{};
    const std::array<uint8_t, 32> aux{};
    EXPECT_EQ(secp256k1_schnorrsig_sign32(crypto::GetSecp256k1ContextSignVerify(),
                                          sig.data(), message.data(), &key.keypair,
                                          aux.data()),
              1);
    return sig;
}

struct Fixture {
    Key bob = MakeKey(2);    // claims DIN with the preimage
    Key alice = MakeKey(3);  // refunds DIN after the lock
    std::vector<uint8_t> preimage = std::vector<uint8_t>(32, 0x5a);
    DinHtlcTerms terms;
    DinHtlcOutput htlc;
    FundingOutput funding;
    Payout payout;

    Fixture() {
        terms.payment_hash = Sha256(preimage);
        terms.claim_pubkey = bob.xonly;
        terms.refund_pubkey = alice.xonly;
        terms.refund_locktime_unix = kRefundLock;
        htlc = BuildDinHtlc(terms);
        funding.txid = TxId(uint256::FromHexUnsafe(std::string(64, '1')));
        funding.vout = 0;
        funding.value = AmountUna::Una(100'000);
        funding.script_pubkey = htlc.script_pubkey;
        payout.script_pubkey = std::vector<uint8_t>{op::OP_1, 0x20};
        payout.script_pubkey.insert(payout.script_pubkey.end(), 32, 0x77);
        payout.fee = AmountUna::Una(1'000);
    }

    bool Verify(const Transaction& tx) const {
        std::vector<op::UTXOEntry> prevouts(1);
        prevouts[0].value = funding.value;
        prevouts[0].scriptPubKey = funding.script_pubkey;
        std::string error;
        return op::ScriptVerifier::VerifyTaproot(tx, 0, prevouts, error,
                                                 op::SCRIPT_VERIFY_STANDARD);
    }
};

void Push(std::vector<uint8_t>& script, const uint8_t* data, size_t size) {
    script.push_back(static_cast<uint8_t>(size));
    script.insert(script.end(), data, data + size);
}

}  // namespace

// --- Script bytes, assembled independently from opcode constants ---------

TEST(SwapHtlc, DinLeavesAreTheDesignedScripts) {
    Fixture f;
    std::vector<uint8_t> claim{op::OP_SIZE, 0x01, 0x20, op::OP_EQUALVERIFY, op::OP_SHA256};
    Push(claim, f.terms.payment_hash.data(), 32);
    claim.push_back(op::OP_EQUALVERIFY);
    Push(claim, f.bob.xonly.data(), 32);
    claim.push_back(op::OP_CHECKSIG);
    EXPECT_EQ(f.htlc.claim_script, claim);

    // 1'800'000'000 = 0x6B49D200 as a 4-byte little-endian script number.
    std::vector<uint8_t> refund{0x04, 0x00, 0xD2, 0x49, 0x6B, op::OP_CHECKLOCKTIMEVERIFY,
                                op::OP_DROP};
    Push(refund, f.alice.xonly.data(), 32);
    refund.push_back(op::OP_CHECKSIG);
    EXPECT_EQ(f.htlc.refund_script, refund);
}

TEST(SwapHtlc, OutputHasNoKeyPath) {
    Fixture f;
    const swap::Bytes32 nums{0x50, 0x92, 0x9b, 0x74, 0xc1, 0xa0, 0x49, 0x54, 0xb7, 0x8b, 0x4b,
                             0x60, 0x35, 0xe9, 0x7a, 0x5e, 0x07, 0x8a, 0x5a, 0x0f, 0x28, 0xec,
                             0x96, 0xd5, 0x47, 0xbf, 0xee, 0x9a, 0xce, 0x80, 0x3a, 0xc0};
    EXPECT_EQ(f.htlc.internal_key, nums);
    ASSERT_EQ(f.htlc.claim_control_block.size(), 65U);
    EXPECT_TRUE(std::equal(nums.begin(), nums.end(), f.htlc.claim_control_block.begin() + 1));
    EXPECT_TRUE(std::equal(nums.begin(), nums.end(), f.htlc.refund_control_block.begin() + 1));
}

// --- Claim path through consensus ----------------------------------------

TEST(SwapHtlc, ClaimSignedFirstThenPreimageInsertedIsValid) {
    Fixture f;
    Transaction tx = BuildDinClaimTx(f.htlc, f.funding, f.payout);
    const auto message = DinClaimSighash(tx, f.funding, f.htlc);  // no preimage yet
    const auto sig = Sign(f.bob, message);
    SetDinClaimWitness(tx, f.terms, f.htlc, sig, f.preimage);
    EXPECT_EQ(DinClaimSighash(tx, f.funding, f.htlc), message)
        << "the sighash must not depend on the witness";
    EXPECT_TRUE(f.Verify(tx));
    EXPECT_EQ(tx.vout.size(), 1U);
    EXPECT_EQ(tx.vout[0].value.GetUna(), 99'000U);
    EXPECT_EQ(tx.lockTime, 0U);
}

TEST(SwapHtlc, WrongPreimageIsRefusedAndInvalid) {
    Fixture f;
    Transaction tx = BuildDinClaimTx(f.htlc, f.funding, f.payout);
    const auto sig = Sign(f.bob, DinClaimSighash(tx, f.funding, f.htlc));
    const std::vector<uint8_t> wrong(32, 0x5b);
    EXPECT_THROW(SetDinClaimWitness(tx, f.terms, f.htlc, sig, wrong), std::invalid_argument);
    EXPECT_THROW(SetDinClaimWitness(tx, f.terms, f.htlc, sig, std::vector<uint8_t>(31, 0x5a)),
                 std::invalid_argument);

    // Bypass the helper: consensus must reject it too.
    tx.vin[0].witness = {std::vector<uint8_t>(sig.begin(), sig.end()), wrong, f.htlc.claim_script,
                         f.htlc.claim_control_block};
    EXPECT_FALSE(f.Verify(tx));
}

TEST(SwapHtlc, RedirectedPayoutAfterSigningIsInvalid) {
    Fixture f;
    Transaction tx = BuildDinClaimTx(f.htlc, f.funding, f.payout);
    const auto sig = Sign(f.bob, DinClaimSighash(tx, f.funding, f.htlc));
    SetDinClaimWitness(tx, f.terms, f.htlc, sig, f.preimage);
    ASSERT_TRUE(f.Verify(tx));

    Transaction redirected = tx;
    redirected.vout[0].scriptPubKey.back() ^= 0x01;
    EXPECT_FALSE(f.Verify(redirected)) << "payout address changed after signing";

    Transaction reduced = tx;
    reduced.vout[0].value = AmountUna::Una(90'000);
    EXPECT_FALSE(f.Verify(reduced)) << "payout amount changed after signing";
}

TEST(SwapHtlc, ClaimByTheRefundKeyIsInvalid) {
    Fixture f;
    Transaction tx = BuildDinClaimTx(f.htlc, f.funding, f.payout);
    const auto sig = Sign(f.alice, DinClaimSighash(tx, f.funding, f.htlc));
    SetDinClaimWitness(tx, f.terms, f.htlc, sig, f.preimage);
    EXPECT_FALSE(f.Verify(tx));
}

// --- Refund path through consensus ---------------------------------------

TEST(SwapHtlc, RefundAtTheLockIsValid) {
    Fixture f;
    Transaction tx = BuildDinRefundTx(f.terms, f.htlc, f.funding, f.payout);
    EXPECT_EQ(tx.lockTime, kRefundLock);
    EXPECT_NE(tx.vin[0].sequence, 0xffffffffU) << "CLTV needs a non-final input";
    SetDinRefundWitness(tx, f.htlc, Sign(f.alice, DinRefundSighash(tx, f.funding, f.htlc)));
    EXPECT_TRUE(f.Verify(tx));
}

TEST(SwapHtlc, RefundBeforeTheLockOrWithAHeightLockIsInvalid) {
    Fixture f;
    for (uint32_t lock_time : {kRefundLock - 1, 123'456U}) {
        Transaction tx = BuildDinRefundTx(f.terms, f.htlc, f.funding, f.payout);
        tx.lockTime = lock_time;
        SetDinRefundWitness(tx, f.htlc, Sign(f.alice, DinRefundSighash(tx, f.funding, f.htlc)));
        EXPECT_FALSE(f.Verify(tx)) << "nLockTime " << lock_time;
    }
}

TEST(SwapHtlc, RefundByTheClaimKeyIsInvalid) {
    Fixture f;
    Transaction tx = BuildDinRefundTx(f.terms, f.htlc, f.funding, f.payout);
    SetDinRefundWitness(tx, f.htlc, Sign(f.bob, DinRefundSighash(tx, f.funding, f.htlc)));
    EXPECT_FALSE(f.Verify(tx));
}

// --- Explicit-input validation -------------------------------------------

TEST(SwapHtlc, HeightLocksAndBadInputsAreRejected) {
    Fixture f;
    DinHtlcTerms height_lock = f.terms;
    height_lock.refund_locktime_unix = 150'000;
    EXPECT_THROW(BuildDinHtlc(height_lock), std::invalid_argument);

    DinHtlcTerms bad_key = f.terms;
    bad_key.claim_pubkey.fill(0xff);  // not an x-coordinate on the curve
    EXPECT_THROW(BuildDinHtlc(bad_key), std::invalid_argument);

    FundingOutput other = f.funding;
    other.script_pubkey.back() ^= 0x01;
    EXPECT_THROW(BuildDinClaimTx(f.htlc, other, f.payout), std::invalid_argument);

    Payout too_much = f.payout;
    too_much.fee = f.funding.value;
    EXPECT_THROW(BuildDinRefundTx(f.terms, f.htlc, f.funding, too_much), std::invalid_argument);
}

// --- Bitcoin side --------------------------------------------------------

TEST(SwapHtlc, BtcWitnessScriptAndP2wsh) {
    BtcHtlcTerms terms;
    terms.payment_hash = Sha256(std::vector<uint8_t>(32, 0x5a));
    terms.claim_pubkey.fill(0x02);
    terms.refund_pubkey.fill(0x03);
    terms.refund_locktime_unix = kRefundLock;

    std::vector<uint8_t> expected{0x63, 0x82, 0x01, 0x20, 0x88, 0xa8};  // IF SIZE 32 EQUALVERIFY SHA256
    Push(expected, terms.payment_hash.data(), 32);
    expected.push_back(0x88);
    Push(expected, terms.claim_pubkey.data(), 33);
    expected.insert(expected.end(), {0x67, 0x04, 0x00, 0xD2, 0x49, 0x6B, 0xb1, 0x75});  // ELSE <T> CLTV DROP
    Push(expected, terms.refund_pubkey.data(), 33);
    expected.insert(expected.end(), {0x68, 0xac});  // ENDIF CHECKSIG
    const auto script = BuildBtcHtlcWitnessScript(terms);
    EXPECT_EQ(script, expected);

    std::vector<uint8_t> p2wsh{0x00, 0x20};
    const auto hash = Sha256(script);
    p2wsh.insert(p2wsh.end(), hash.begin(), hash.end());
    EXPECT_EQ(BtcP2wshScriptPubKey(script), p2wsh);
}

TEST(SwapHtlc, ClaimWithANonMinimalSelectorStillRevealsThePreimage) {
    // MINIMALIF is only relay policy for P2WSH: a miner can include a claim whose
    // OP_IF selector is 0x02 (or any non-zero push). It still carries the secret;
    // classifying by witness shape would call it a refund and hide the secret.
    const std::vector<uint8_t> preimage(32, 0x5a);
    const auto hash = Sha256(preimage);
    for (const std::vector<uint8_t>& selector : {std::vector<uint8_t>{0x02}, std::vector<uint8_t>{0x01, 0x00},
                                                 std::vector<uint8_t>{0x81}}) {
        const std::vector<std::vector<uint8_t>> claim{std::vector<uint8_t>(72, 0x30), preimage, selector,
                                                      std::vector<uint8_t>(100, 0x63)};
        const auto found = ExtractPreimageFromBtcClaim(claim, hash);
        ASSERT_TRUE(found.has_value()) << int(selector[0]);
        EXPECT_TRUE(std::equal(found->begin(), found->end(), preimage.begin()));
    }
    // A refund with a non-minimal false selector carries no secret.
    const std::vector<std::vector<uint8_t>> refund{std::vector<uint8_t>(72, 0x30), {0x00},
                                                   std::vector<uint8_t>(100, 0x63)};
    EXPECT_FALSE(ExtractPreimageFromBtcClaim(refund, hash).has_value());
}

TEST(SwapHtlc, PreimageExtractedOnlyWhenItMatches) {
    const std::vector<uint8_t> preimage(32, 0x5a);
    const auto hash = Sha256(preimage);
    const std::vector<std::vector<uint8_t>> claim{std::vector<uint8_t>(72, 0x30), preimage, {0x01},
                                                  std::vector<uint8_t>(100, 0x63)};
    const auto found = ExtractPreimageFromBtcClaim(claim, hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(std::equal(found->begin(), found->end(), preimage.begin()));

    auto wrong = claim;
    wrong[1][0] ^= 0x01;
    EXPECT_FALSE(ExtractPreimageFromBtcClaim(wrong, hash).has_value());
    const std::vector<std::vector<uint8_t>> refund{std::vector<uint8_t>(72, 0x30), {},
                                                   std::vector<uint8_t>(100, 0x63)};
    EXPECT_FALSE(ExtractPreimageFromBtcClaim(refund, hash).has_value());
}
