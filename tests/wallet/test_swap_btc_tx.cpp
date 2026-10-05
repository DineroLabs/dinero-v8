// Bitcoin side of the swap: BIP143 against the official vector, serialization,
// and HTLC claim/refund transactions signed with libsecp256k1 ECDSA.
#include "wallet/swap/btc_tx.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <secp256k1.h>

#include <stdexcept>
#include <string>

namespace {

using namespace dinero;
using namespace dinero::swap;

std::vector<uint8_t> Hex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::array<uint8_t, 32> Secret(uint8_t s) { std::array<uint8_t, 32> a{}; a.back() = s; return a; }

std::array<uint8_t, 33> Pubkey(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Secret(scalar);
    EXPECT_EQ(secp256k1_ec_pubkey_create(secp, &pk, s.data()), 1);
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    EXPECT_EQ(secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED), 1);
    return out;
}

// DER signature + SIGHASH_ALL byte, and a check that it verifies.
std::vector<uint8_t> SignAll(uint8_t scalar, const Bytes32& msg) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    const auto s = Secret(scalar);
    secp256k1_ecdsa_signature sig;
    EXPECT_EQ(secp256k1_ecdsa_sign(secp, &sig, msg.data(), s.data(), nullptr, nullptr), 1);
    std::vector<uint8_t> der(72);
    size_t len = der.size();
    EXPECT_EQ(secp256k1_ecdsa_signature_serialize_der(secp, der.data(), &len, &sig), 1);
    der.resize(len);
    secp256k1_pubkey pk;
    EXPECT_EQ(secp256k1_ec_pubkey_create(secp, &pk, s.data()), 1);
    EXPECT_EQ(secp256k1_ecdsa_verify(secp, &sig, msg.data(), &pk), 1);
    der.push_back(0x01);
    return der;
}

const std::vector<uint8_t> kPreimage(32, 0x5a);

BtcHtlcTerms Terms() {
    BtcHtlcTerms t;
    crypto::CSHA256().Write(kPreimage.data(), kPreimage.size()).Finalize(t.payment_hash.data());
    t.claim_pubkey = Pubkey(4);   // Alice
    t.refund_pubkey = Pubkey(6);  // Bob
    t.refund_locktime_unix = 1'800'000'000;
    return t;
}

BtcFunding Funding() {
    BtcFunding f;
    f.txid.fill(0x11);
    f.vout = 0;
    f.value_sat = 1'000'000;
    return f;
}

const std::vector<uint8_t> kPayout = Hex("0014" + std::string(40, '7'));  // P2WPKH

}  // namespace

// Official BIP143 "Native P2WPKH" example (values verified self-consistent).
TEST(SwapBtcTx, Bip143MatchesTheOfficialVector) {
    const auto tx = ParseBtcTx(Hex(
        "0100000002fff7f7881a8099afa6940d42d1e7f6362bec38171ea3edf433541db4e4ad969f0000000000eeffffff"
        "ef51e1b804cc89d182d279655c3aa89e815b1b309fe287d9b2b55d57b90ec68a0100000000ffffffff02202cb206"
        "000000001976a9148280b37df378db99f66f85c95a783a76ac7a6d5988ac9093510d000000001976a9143bde42db"
        "ee7e4dbe6a21b2d50ce2f0167faa815988ac11000000"));
    ASSERT_EQ(tx.vin.size(), 2U);
    const auto sighash = Bip143SighashAll(
        tx, 1, Hex("76a9141d0f172a0ecb48aee1be1f2687d2963ae33f71a188ac"), 600'000'000);
    EXPECT_EQ(ToHex(sighash.data(), 32),
              "c37af31116d1b27caf68aae9e3ac82f1477929014d5b917657d0eb49478cb670");
}

TEST(SwapBtcTx, ParseAndSerializeRoundTrip) {
    const auto raw = Hex(
        "0100000002fff7f7881a8099afa6940d42d1e7f6362bec38171ea3edf433541db4e4ad969f0000000000eeffffff"
        "ef51e1b804cc89d182d279655c3aa89e815b1b309fe287d9b2b55d57b90ec68a0100000000ffffffff02202cb206"
        "000000001976a9148280b37df378db99f66f85c95a783a76ac7a6d5988ac9093510d000000001976a9143bde42db"
        "ee7e4dbe6a21b2d50ce2f0167faa815988ac11000000");
    EXPECT_EQ(SerializeBtcTx(ParseBtcTx(raw)), raw);
    auto truncated = raw;
    truncated.pop_back();
    EXPECT_THROW(ParseBtcTx(truncated), std::invalid_argument);
}

TEST(SwapBtcTx, ClaimCarriesTheSecretAndAValidSignature) {
    const auto terms = Terms();
    auto tx = BuildBtcClaimTx(Funding(), kPayout, 1'000);
    EXPECT_EQ(tx.locktime, 0U);
    ASSERT_EQ(tx.vin.size(), 1U);
    EXPECT_EQ(tx.vin[0].sequence, 0xfffffffdU);
    EXPECT_EQ(tx.vout[0].value_sat, 999'000U);
    const auto msg = BtcHtlcSighash(tx, terms, Funding());
    const auto txid_before = BtcTxid(tx);
    SetBtcClaimWitness(tx, terms, SignAll(4, msg), kPreimage);
    EXPECT_EQ(BtcHtlcSighash(tx, terms, Funding()), msg) << "sighash must not depend on the witness";
    EXPECT_EQ(BtcTxid(tx), txid_before) << "txid must not depend on the witness";

    const auto& w = tx.vin[0].witness;
    ASSERT_EQ(w.size(), 4U);
    EXPECT_EQ(w[1], kPreimage);
    EXPECT_EQ(w[2], std::vector<uint8_t>{0x01});
    EXPECT_EQ(w[3], BuildBtcHtlcWitnessScript(terms));
    const auto found = ExtractPreimageFromBtcClaim(w, terms.payment_hash);
    ASSERT_TRUE(found.has_value());

    const auto wire = SerializeBtcTx(tx);
    EXPECT_EQ(wire[4], 0x00);
    EXPECT_EQ(wire[5], 0x01) << "segwit marker and flag";
    EXPECT_EQ(SerializeBtcTx(ParseBtcTx(wire)), wire);
}

TEST(SwapBtcTx, RefundUsesTheLockAndTheElseBranch) {
    const auto terms = Terms();
    auto tx = BuildBtcRefundTx(terms, Funding(), kPayout, 1'000);
    EXPECT_EQ(tx.locktime, terms.refund_locktime_unix);
    EXPECT_NE(tx.vin[0].sequence, 0xffffffffU);
    SetBtcRefundWitness(tx, terms, SignAll(6, BtcHtlcSighash(tx, terms, Funding())));
    const auto& w = tx.vin[0].witness;
    ASSERT_EQ(w.size(), 3U);
    EXPECT_TRUE(w[1].empty()) << "empty element selects OP_ELSE";
    EXPECT_FALSE(ExtractPreimageFromBtcClaim(w, terms.payment_hash).has_value());
}

TEST(SwapBtcTx, SighashCommitsToPayoutFeeAndLock) {
    const auto terms = Terms();
    const auto base = BtcHtlcSighash(BuildBtcClaimTx(Funding(), kPayout, 1'000), terms, Funding());
    auto other_payout = kPayout;
    other_payout.back() ^= 1;
    EXPECT_NE(BtcHtlcSighash(BuildBtcClaimTx(Funding(), other_payout, 1'000), terms, Funding()), base);
    EXPECT_NE(BtcHtlcSighash(BuildBtcClaimTx(Funding(), kPayout, 1'001), terms, Funding()), base);
    EXPECT_NE(BtcHtlcSighash(BuildBtcRefundTx(terms, Funding(), kPayout, 1'000), terms, Funding()), base);
}

TEST(SwapBtcTx, BadInputsAreRefused) {
    const auto terms = Terms();
    auto tx = BuildBtcClaimTx(Funding(), kPayout, 1'000);
    const auto sig = SignAll(4, BtcHtlcSighash(tx, terms, Funding()));
    EXPECT_THROW(SetBtcClaimWitness(tx, terms, sig, std::vector<uint8_t>(32, 0x5b)), std::invalid_argument);
    EXPECT_THROW(BuildBtcClaimTx(Funding(), kPayout, Funding().value_sat), std::invalid_argument);
    EXPECT_THROW(BuildBtcClaimTx(Funding(), {}, 1'000), std::invalid_argument);
}
