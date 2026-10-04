// Swap watchtower: Bob's package holds no key and no secret, is verified when
// loaded (tampering refused), and the tower claims DIN with Alice's revealed
// secret or refunds BTC after T_btc, escalating fees; it never acts blind.
#include "wallet/swap/tower.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using namespace dinero;
using namespace dinero::swap;
using detail::ToHex;

constexpr uint32_t kNow = 1'800'000'000;
constexpr uint32_t kHour = 3600;

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }

const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();
const SwapKeys kAliceKeys{Scalar(3), Scalar(4)};
const SwapKeys kBobKeys{Scalar(5), Scalar(6)};

SwapSession BobSession() {
    SwapSession s;
    SwapRecord& r = s.record;
    r.role = Role::BtcSeller;
    auto& o = r.offer;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 10 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = detail::XOnlyOf(kAliceKeys.din_secret_key);
    o.btc_claim_pubkey = detail::CompressedOf(kAliceKeys.btc_secret_key);
    o.expires_unix = kNow + 2 * kHour;
    o.t_btc_unix = kNow + 26 * kHour;
    o.t_din_unix = kNow + 50 * kHour;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 1;
    r.accept.offer_id = OfferId(o);
    r.accept.din_claim_pubkey = detail::XOnlyOf(kBobKeys.din_secret_key);
    r.accept.btc_refund_pubkey = detail::CompressedOf(kBobKeys.btc_secret_key);
    r.state = SwapState::BtcLocked;
    s.btc_scan_from_height = 101;
    s.din_payout_script = {0x51, 0x20};
    s.din_payout_script.resize(34, 0x88);
    s.btc_payout_script = {0x51, 0x20};
    s.btc_payout_script.resize(34, 0x99);
    return s;
}

FundingOutput DinFunding(const SwapSession& s) {
    FundingOutput f;
    f.txid = TxId(uint256::FromHexUnsafe(std::string(64, 'a')));
    f.vout = 1;
    f.value = AmountUna::Una(s.record.offer.din_amount_una);
    f.script_pubkey = BuildDinHtlc(MakeDinTerms(s.record.offer, s.record.accept)).script_pubkey;
    return f;
}

BtcFunding BtcFundingOf(const SwapSession& s) {
    BtcFunding f;
    f.txid.fill(0xbb);
    f.vout = 0;
    f.value_sat = s.record.offer.btc_amount_sat;
    return f;
}

FeeLadderPolicy DinPolicy() {
    FeeLadderPolicy p;
    p.start_feerate_una_per_vb = 1'000;
    p.max_rungs = 4;
    p.max_fee_una = 50'000'000;
    p.min_payout_una = 500'000'000;
    return p;
}

BtcFeeLadderPolicy BtcPolicy() {
    BtcFeeLadderPolicy p;
    p.start_feerate_sat_per_vb = 2;
    p.max_rungs = 4;
    p.max_fee_sat = 50'000;
    p.min_payout_sat = 500'000;
    return p;
}

TowerPackage Package() {
    const auto s = BobSession();
    return BuildTowerPackage(s, kBobKeys, DinFunding(s), BtcFundingOf(s), DinPolicy(), BtcPolicy());
}

struct FakeChains : SwapChainIo {
    DinWatchReport din;
    BtcWatchReport btc;
    std::vector<std::vector<uint8_t>> din_broadcasts, btc_broadcasts;
    FakeChains() {
        const auto s = BobSession();
        din.ok = btc.ok = true;
        din.mtp_unix = btc.mtp_unix = kNow;
        din.funding = DinFunding(s);
        din.htlc.output_seen = true;
        din.htlc.output_confirmations = 40;
        din.htlc.output_value = s.record.offer.din_amount_una;
        btc.funding = BtcFundingOf(s);
        btc.htlc.output_seen = true;
        btc.htlc.output_confirmations = 5;
        btc.htlc.output_value = s.record.offer.btc_amount_sat;
    }
    void AliceClaimsBtc(const Bytes32& preimage) {
        btc.htlc.spent = true;
        btc.htlc.spent_by_claim = true;
        btc.htlc.revealed_preimage = preimage;
    }
    DinWatchReport ObserveDin() override { return din; }
    BtcWatchReport ObserveBtc() override { return btc; }
    std::string FundDin(const std::string&, uint64_t) override { throw std::logic_error("tower must not fund"); }
    std::string FundBtc(const std::string&, uint64_t) override { throw std::logic_error("tower must not fund"); }
    std::string BroadcastDin(const std::vector<uint8_t>& t) override { din_broadcasts.push_back(t); return "d"; }
    std::string BroadcastBtc(const std::vector<uint8_t>& t) override { btc_broadcasts.push_back(t); return "b"; }
};

TowerConfig Config() {
    TowerConfig c;
    c.escalate_after_seconds = 1800;
    c.din_urgent_before_seconds = 6 * kHour;
    return c;
}

uint64_t DinFeeOf(const std::vector<uint8_t>& raw) {
    Transaction tx;
    size_t used = 0;
    EXPECT_TRUE(TransactionSerializer::Deserialize(tx, raw, used));
    return BobSession().record.offer.din_amount_una - tx.vout.at(0).value.GetUna();
}

TEST(SwapTower, PackageHoldsNoKeyNoSecretAndRoundTrips) {
    const auto p = Package();
    EXPECT_EQ(p.din_claims.size(), 4u);
    EXPECT_EQ(p.btc_refunds.size(), 4u);
    const std::string text = EncodeTowerPackage(p);
    for (const auto& k : {kBobKeys.din_secret_key, kBobKeys.btc_secret_key, kSecret}) {
        EXPECT_EQ(text.find(ToHex(std::vector<uint8_t>(k.begin(), k.end()))), std::string::npos);
    }
    EXPECT_EQ(EncodeTowerPackage(DecodeTowerPackage(text)), text);
}

TEST(SwapTower, TamperedPackagesAreRefused) {
    auto check = [](const char* what, auto mutate) {
        auto p = Package();
        mutate(p);
        EXPECT_THROW(VerifyTowerPackage(p), std::invalid_argument) << what;
    };
    check("DIN payout redirected", [](TowerPackage& p) { p.din_claims[1].tx.vout[0].scriptPubKey[5] ^= 1; });
    check("DIN signature", [](TowerPackage& p) { p.din_claims[0].signature[10] ^= 1; });
    check("DIN fees not rising", [](TowerPackage& p) { std::swap(p.din_claims[0], p.din_claims[1]); });
    check("BTC payout redirected", [](TowerPackage& p) { p.btc_refunds[2].tx.vout[0].script_pubkey[5] ^= 1; });
    check("BTC signature", [](TowerPackage& p) { p.btc_refunds[0].tx.vin[0].witness[0][8] ^= 1; });
    check("BTC locktime", [](TowerPackage& p) { p.btc_refunds[0].tx.locktime -= 1; });
    check("BTC fees not rising", [](TowerPackage& p) { std::swap(p.btc_refunds[0], p.btc_refunds[3]); });
    check("other accept", [](TowerPackage& p) { p.accept.offer_id[0] ^= 1; });
    // Validly signed by Bob, yet wrong: only the dedicated checks catch these.
    check("DIN rung signed to another payout", [](TowerPackage& p) {
        auto s = BobSession();
        s.din_payout_script[10] ^= 1;
        p.din_claims[1] = BuildTowerPackage(s, kBobKeys, DinFunding(s), BtcFundingOf(s), DinPolicy(), BtcPolicy())
                              .din_claims[1];
    });
    check("BTC rung signed with locktime below T_btc", [](TowerPackage& p) {
        const auto s = BobSession();
        const auto terms = MakeBtcTerms(s.record.offer, s.record.accept);
        auto& tx = p.btc_refunds[0].tx;
        tx.locktime = s.record.offer.t_btc_unix - 1;
        SetBtcRefundWitness(tx, terms,
                            detail::EcdsaSignAll(kBobKeys.btc_secret_key, BtcHtlcSighash(tx, terms, BtcFundingOf(s))));
    });
    check("no rungs", [](TowerPackage& p) { p.btc_refunds.clear(); });
    // Alice cannot build a package for Bob's swap.
    const auto s = BobSession();
    EXPECT_THROW(BuildTowerPackage(s, kAliceKeys, DinFunding(s), BtcFundingOf(s), DinPolicy(), BtcPolicy()),
                 std::invalid_argument);
}

TEST(SwapTower, ClaimsDinWithTheRevealedSecret) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    tower.Tick(kNow + kHour);
    EXPECT_TRUE(chains.din_broadcasts.empty() && chains.btc_broadcasts.empty()) << "nothing to do yet";

    chains.AliceClaimsBtc(kSecret);
    tower.Tick(kNow + 2 * kHour);
    ASSERT_EQ(chains.din_broadcasts.size(), 1u);
    Transaction tx;
    size_t used = 0;
    ASSERT_TRUE(TransactionSerializer::Deserialize(tx, chains.din_broadcasts[0], used));
    const auto& w = tx.vin.at(0).witness;
    ASSERT_EQ(w.size(), 4u);
    EXPECT_EQ(w[1], std::vector<uint8_t>(kSecret.begin(), kSecret.end()));
    EXPECT_EQ(tx.vout.at(0).scriptPubKey, BobSession().din_payout_script);
    const auto s = BobSession();
    const auto terms = MakeDinTerms(s.record.offer, s.record.accept);
    std::array<uint8_t, 64> sig{};
    std::copy(w[0].begin(), w[0].end(), sig.begin());
    EXPECT_TRUE(detail::SchnorrVerify(terms.claim_pubkey, DinClaimSighash(tx, DinFunding(s), BuildDinHtlc(terms)), sig));
}

TEST(SwapTower, IgnoresASecretThatDoesNotMatch) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    Bytes32 wrong = kSecret;
    wrong[0] ^= 1;
    chains.AliceClaimsBtc(wrong);
    const auto r = tower.Tick(kNow + kHour);
    EXPECT_TRUE(chains.din_broadcasts.empty());
    EXPECT_FALSE(r.finished);
}

TEST(SwapTower, DinRungFollowsUrgencyNotElapsedTime) {
    // Dinero nodes do not replace by fee, so the DIN rung is chosen by how
    // close T_din is (by Dinero's median time), never escalated over time.
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);  // relaxed >= 24 h, urgent <= 6 h
    chains.AliceClaimsBtc(kSecret);
    const auto t_din = BobSession().record.offer.t_din_unix;
    const auto fees = [] {
        std::vector<uint64_t> f;
        for (const auto& r : Package().din_claims) f.push_back(r.fee_una);
        return f;
    }();
    chains.din.mtp_unix = t_din - 40 * kHour;
    for (uint32_t t = 0; t < 10 * kHour; t += kHour) tower.Tick(kNow + t);  // hours pass, far from T_din
    for (const auto& b : chains.din_broadcasts) EXPECT_EQ(DinFeeOf(b), fees.front()) << "no time escalation";

    chains.din.mtp_unix = t_din - 15 * kHour;  // halfway between 24 h and 6 h
    tower.Tick(kNow + 11 * kHour);
    const uint64_t mid = DinFeeOf(chains.din_broadcasts.back());
    EXPECT_GT(mid, fees.front());
    EXPECT_LT(mid, fees.back());

    chains.din.mtp_unix = t_din - 2 * kHour;
    tower.Tick(kNow + 11 * kHour);
    EXPECT_EQ(DinFeeOf(chains.din_broadcasts.back()), fees.back());
}

TEST(SwapTower, BtcRefundStillEscalatesOverTime) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix;
    tower.Tick(kNow);
    tower.Tick(kNow + 1800);
    ASSERT_EQ(chains.btc_broadcasts.size(), 2u);
    EXPECT_EQ(chains.btc_broadcasts[0], SerializeBtcTx(Package().btc_refunds[0].tx));
    EXPECT_EQ(chains.btc_broadcasts[1], SerializeBtcTx(Package().btc_refunds[1].tx));
}

TEST(SwapTower, RefundsBtcOnlyOnceBitcoinTimeReachesTBtc) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix - 1;
    tower.Tick(kNow + 30 * kHour);  // wall clock past T_btc, chain not: wait
    EXPECT_TRUE(chains.btc_broadcasts.empty());
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix;
    tower.Tick(kNow + 30 * kHour);
    ASSERT_EQ(chains.btc_broadcasts.size(), 1u);
    EXPECT_EQ(chains.btc_broadcasts[0], SerializeBtcTx(Package().btc_refunds[0].tx));
    EXPECT_TRUE(chains.din_broadcasts.empty());
}

TEST(SwapTower, NeverActsBlindAndStopsWhenSettled) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    chains.din.ok = false;
    EXPECT_FALSE(tower.Tick(kNow).observed);
    EXPECT_TRUE(chains.din_broadcasts.empty());

    chains.din.ok = true;
    chains.din.htlc.spent = true;
    chains.din.htlc.spent_by_claim = true;
    chains.din.htlc.spend_confirmations = Config().settle_confirmations;
    const auto r = tower.Tick(kNow);
    EXPECT_TRUE(r.finished);
    EXPECT_TRUE(chains.din_broadcasts.empty());
}

}  // namespace

namespace {

TEST(SwapTower, KeepsWatchingUntilTheSpendIsBuriedAndRebroadcastsAfterAReorg) {
    FakeChains chains;
    auto config = Config();
    config.settle_confirmations = 6;
    Watchtower tower(Package(), config, chains);
    chains.AliceClaimsBtc(kSecret);
    tower.Tick(kNow);
    ASSERT_EQ(chains.din_broadcasts.size(), 1u);

    chains.din.htlc.spent = true;  // the tower's claim is mined
    chains.din.htlc.spent_by_claim = true;
    chains.din.htlc.spend_confirmations = 1;
    EXPECT_FALSE(tower.Tick(kNow + 60).finished) << "one block is not settled";

    chains.din.htlc.spent = false;  // a reorg drops it
    chains.din.htlc.spent_by_claim = false;
    chains.din.htlc.spend_confirmations = 0;
    tower.Tick(kNow + 120);
    EXPECT_EQ(chains.din_broadcasts.size(), 2u) << "re-broadcast after the reorg";

    chains.din.htlc.spent = chains.din.htlc.spent_by_claim = true;
    chains.din.htlc.spend_confirmations = 6;
    EXPECT_TRUE(tower.Tick(kNow + 600).finished);

    // Same for Bob's BTC refund.
    FakeChains c2;
    Watchtower t2(Package(), config, c2);
    c2.btc.mtp_unix = BobSession().record.offer.t_btc_unix;
    c2.btc.htlc.spent = true;
    c2.btc.htlc.spent_by_claim = false;
    c2.btc.htlc.spend_confirmations = 2;
    EXPECT_FALSE(t2.Tick(kNow).finished);
    c2.btc.htlc.spend_confirmations = 6;
    EXPECT_TRUE(t2.Tick(kNow).finished);
}

}  // namespace
