// Swap offer/accept messages: round trip, tamper detection and every refusal.
#include "wallet/swap/offer.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;
constexpr uint32_t kHour = 3600;

std::array<uint8_t, 32> Secret(uint8_t scalar) {
    std::array<uint8_t, 32> s{};
    s.back() = scalar;
    return s;
}

Bytes32 XOnly(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair kp;
    const auto s = Secret(scalar);
    EXPECT_EQ(secp256k1_keypair_create(secp, &kp, s.data()), 1);
    secp256k1_xonly_pubkey x;
    EXPECT_EQ(secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp), 1);
    Bytes32 out{};
    EXPECT_EQ(secp256k1_xonly_pubkey_serialize(secp, out.data(), &x), 1);
    return out;
}

std::array<uint8_t, 33> Compressed(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Secret(scalar);
    EXPECT_EQ(secp256k1_ec_pubkey_create(secp, &pk, s.data()), 1);
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    EXPECT_EQ(secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED), 1);
    return out;
}

SwapOffer ValidOffer() {
    SwapOffer o;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 1'000 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    o.payment_hash.fill(0xab);
    o.din_refund_pubkey = XOnly(3);      // Alice
    o.btc_claim_pubkey = Compressed(4);  // Alice
    o.expires_unix = kNow + 2 * kHour;
    o.t_btc_unix = kNow + 26 * kHour;
    o.t_din_unix = kNow + 50 * kHour;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 2;
    return o;
}

SwapAccept ValidAccept(const SwapOffer& offer) {
    SwapAccept a;
    a.offer_id = OfferId(offer);
    a.din_claim_pubkey = XOnly(5);       // Bob
    a.btc_refund_pubkey = Compressed(6);  // Bob
    return a;
}

std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

std::string ToHex(const std::vector<uint8_t>& bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto b : bytes) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

// Re-encode a tampered payload with a correct checksum, as a malicious
// counterparty could, to test decode-side validation.
std::string Reseal(const std::string& prefix, std::vector<uint8_t> payload) {
    std::array<uint8_t, 32> h{};
    crypto::CSHA256().Write(payload.data(), payload.size()).Finalize(h.data());
    payload.insert(payload.end(), h.begin(), h.begin() + 4);
    return prefix + ToHex(payload);
}

std::vector<uint8_t> PayloadOf(const std::string& text, const std::string& prefix) {
    auto bytes = FromHex(text.substr(prefix.size()));
    bytes.resize(bytes.size() - 4);
    return bytes;
}

}  // namespace

TEST(SwapOffer, OfferAndAcceptRoundTrip) {
    const auto offer = ValidOffer();
    const auto text = EncodeOffer(offer);
    EXPECT_EQ(text.rfind(kOfferPrefix, 0), 0U);
    const auto back = DecodeOffer(text);
    EXPECT_EQ(EncodeOffer(back), text);
    EXPECT_EQ(back.t_din_unix, offer.t_din_unix);
    EXPECT_EQ(back.btc_claim_pubkey, offer.btc_claim_pubkey);
    EXPECT_EQ(OfferId(back), OfferId(offer));

    const auto accept = ValidAccept(offer);
    const auto atext = EncodeAccept(accept);
    EXPECT_EQ(atext.rfind(kAcceptPrefix, 0), 0U);
    const auto aback = DecodeAccept(atext);
    EXPECT_EQ(aback.offer_id, accept.offer_id);
    EXPECT_EQ(aback.din_claim_pubkey, accept.din_claim_pubkey);
    EXPECT_EQ(aback.btc_refund_pubkey, accept.btc_refund_pubkey);
}

TEST(SwapOffer, TamperingIsDetected) {
    const auto text = EncodeOffer(ValidOffer());
    std::string flipped = text;
    flipped[flipped.size() / 2] = flipped[flipped.size() / 2] == 'a' ? 'b' : 'a';
    EXPECT_THROW(DecodeOffer(flipped), std::invalid_argument) << "checksum";
    EXPECT_THROW(DecodeOffer(text.substr(0, text.size() - 2)), std::invalid_argument) << "truncated";
    EXPECT_THROW(DecodeOffer(text + "00"), std::invalid_argument) << "trailing bytes";
    EXPECT_THROW(DecodeAccept(text), std::invalid_argument) << "an offer is not an accept";
    EXPECT_THROW(DecodeOffer(std::string("dinswap1x") + text.substr(9)), std::invalid_argument);

    auto payload = PayloadOf(text, kOfferPrefix);
    payload[0] = 2;  // unknown version, correctly checksummed
    EXPECT_THROW(DecodeOffer(Reseal(kOfferPrefix, payload)), std::invalid_argument);
    payload = PayloadOf(text, kOfferPrefix);
    payload[1] = 9;  // unknown network
    EXPECT_THROW(DecodeOffer(Reseal(kOfferPrefix, payload)), std::invalid_argument);
    payload = PayloadOf(text, kOfferPrefix);
    // Alice's DIN refund key -> an x value above the field prime (never a valid key;
    // flipping one byte is not enough, about half of all x values are on the curve).
    std::fill(payload.begin() + 2 + 8 + 8 + 32, payload.begin() + 2 + 8 + 8 + 32 + 32, 0xff);
    EXPECT_THROW(DecodeOffer(Reseal(kOfferPrefix, payload)), std::invalid_argument);
}

TEST(SwapOffer, UnsafeOffersAreRefused) {
    auto expect_refused = [](SwapOffer o, const char* why) {
        EXPECT_THROW(EncodeOffer(o), std::invalid_argument) << why;
    };
    auto o = ValidOffer(); o.din_amount_una = 0; expect_refused(o, "zero DIN");
    o = ValidOffer(); o.btc_amount_sat = 0; expect_refused(o, "zero BTC");
    o = ValidOffer(); o.t_btc_unix = 150'000; expect_refused(o, "height-type BTC lock");
    o = ValidOffer(); o.t_din_unix = o.t_btc_unix + kMinDeadlineGapSeconds - 1; expect_refused(o, "gap < 24 h");
    o = ValidOffer(); o.n_din_confirmations = kMinDinConfirmations - 1; expect_refused(o, "too few DIN confs");
    o = ValidOffer(); o.n_btc_confirmations = 0; expect_refused(o, "zero BTC confs");
    o = ValidOffer(); o.expires_unix = o.t_btc_unix - 11 * kHour; expect_refused(o, "expiry too close to BTC deadline");
    o = ValidOffer(); o.btc_claim_pubkey[0] = 0x04; expect_refused(o, "uncompressed BTC key");
    o = ValidOffer(); o.din_refund_pubkey.fill(0xff); expect_refused(o, "invalid x-only key");
}

TEST(SwapOffer, AcceptIsBoundToItsOffer) {
    const auto offer = ValidOffer();
    auto accept = ValidAccept(offer);
    auto other = offer;
    other.btc_amount_sat += 1;
    EXPECT_NE(OfferId(other), OfferId(offer)) << "every field is committed";
    EXPECT_THROW(MakeDinTerms(other, accept), std::invalid_argument);
    EXPECT_THROW(MakeBtcTerms(other, accept), std::invalid_argument);

    auto bad = accept;
    bad.btc_refund_pubkey[0] = 0x04;
    EXPECT_THROW(EncodeAccept(bad), std::invalid_argument);
}

TEST(SwapOffer, TermsMapRolesCorrectlyAndBuild) {
    const auto offer = ValidOffer();
    const auto accept = ValidAccept(offer);
    const auto din = MakeDinTerms(offer, accept);
    EXPECT_EQ(din.payment_hash, offer.payment_hash);
    EXPECT_EQ(din.claim_pubkey, accept.din_claim_pubkey) << "Bob claims DIN";
    EXPECT_EQ(din.refund_pubkey, offer.din_refund_pubkey) << "Alice refunds DIN";
    EXPECT_EQ(din.refund_locktime_unix, offer.t_din_unix);
    EXPECT_NO_THROW(BuildDinHtlc(din));

    const auto btc = MakeBtcTerms(offer, accept);
    EXPECT_EQ(btc.claim_pubkey, offer.btc_claim_pubkey) << "Alice claims BTC";
    EXPECT_EQ(btc.refund_pubkey, accept.btc_refund_pubkey) << "Bob refunds BTC";
    EXPECT_EQ(btc.refund_locktime_unix, offer.t_btc_unix);
    EXPECT_NO_THROW(BuildBtcHtlcWitnessScript(btc));
}

TEST(SwapOffer, OnePartyCannotHoldBothPathsOfAnHtlc) {
    const auto offer = ValidOffer();
    auto accept = ValidAccept(offer);
    accept.din_claim_pubkey = offer.din_refund_pubkey;  // Bob reuses Alice's key
    EXPECT_THROW(MakeDinTerms(offer, accept), std::invalid_argument);
    accept = ValidAccept(offer);
    accept.btc_refund_pubkey = offer.btc_claim_pubkey;
    EXPECT_THROW(MakeBtcTerms(offer, accept), std::invalid_argument);
}

TEST(SwapOffer, TakerRefusesExpiredOrShortDeadlineOffers) {
    const auto offer = ValidOffer();
    EXPECT_NO_THROW(RequireAcceptableNow(offer, kNow));
    EXPECT_THROW(RequireAcceptableNow(offer, offer.expires_unix + 1), std::invalid_argument);
    auto tight = offer;
    tight.t_din_unix = kNow + 35 * kHour;
    tight.t_btc_unix = tight.t_din_unix - kMinDeadlineGapSeconds;
    tight.expires_unix = kNow + kHour;
    EXPECT_THROW(RequireAcceptableNow(tight, kNow), std::invalid_argument) << "DIN deadline < 36 h away";
}
