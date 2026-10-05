// Swap watchtower: Bob's package holds no key and no secret, is verified when
// loaded (tampering refused), and the tower claims DIN with Alice's revealed
// secret or refunds BTC after T_btc, escalating fees; it never acts blind.
#include "wallet/swap/tower.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <filesystem>
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
const SwapKeys kBobKeys{Scalar(5), Scalar(6), Scalar(7)};

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
    s.din_sweep_pubkey = detail::XOnlyOf(Scalar(7));  // CPFP: Bob's claims pay his sweep output
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
    std::optional<bool> claim_output_unspent;
    std::optional<bool> DinOutputUnspent(const TxId&, uint32_t) override { return claim_output_unspent; }
};

TowerConfig Config() {
    TowerConfig c;
    c.escalate_after_seconds = 1800;
    c.din_urgent_before_seconds = 6 * kHour;
    return c;
}

Transaction ParseDin(const std::vector<uint8_t>& raw) {
    Transaction tx;
    size_t used = 0;
    EXPECT_TRUE(TransactionSerializer::Deserialize(tx, raw, used));
    return tx;
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
    EXPECT_EQ(tx.vout.at(0).scriptPubKey, BuildDinSweepOutput(BobSession().din_sweep_pubkey).script_pubkey)
        << "CPFP: the claim pays Bob's sweep output";
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
    // Claims only (CPFP children of a stuck claim are broadcast too).
    auto claims = [&] {
        std::vector<std::vector<uint8_t>> out;
        const auto htlc_txid = DinFunding(BobSession()).txid;
        for (const auto& b : chains.din_broadcasts) {
            Transaction tx;
            size_t used = 0;
            if (TransactionSerializer::Deserialize(tx, b, used) && tx.vin.at(0).prevout.txid == htlc_txid) {
                out.push_back(b);
            }
        }
        return out;
    };
    chains.din.mtp_unix = t_din - 40 * kHour;
    for (uint32_t t = 0; t < 10 * kHour; t += kHour) tower.Tick(kNow + t);  // hours pass, far from T_din
    for (const auto& b : claims()) EXPECT_EQ(DinFeeOf(b), fees.front()) << "no time escalation";

    chains.din.mtp_unix = t_din - 15 * kHour;  // halfway between 24 h and 6 h
    tower.Tick(kNow + 11 * kHour);
    const uint64_t mid = DinFeeOf(claims().back());
    EXPECT_GT(mid, fees.front());
    EXPECT_LT(mid, fees.back());

    chains.din.mtp_unix = t_din - 2 * kHour;
    tower.Tick(kNow + 11 * kHour);
    EXPECT_EQ(DinFeeOf(claims().back()), fees.back());
}

TEST(SwapTower, BtcRefundStillEscalatesOverTime) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix + 1;
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
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix;  // equal: not final yet
    tower.Tick(kNow + 30 * kHour);
    EXPECT_TRUE(chains.btc_broadcasts.empty());
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix + 1;
    tower.Tick(kNow + 30 * kHour);
    ASSERT_EQ(chains.btc_broadcasts.size(), 1u);
    EXPECT_EQ(chains.btc_broadcasts[0], SerializeBtcTx(Package().btc_refunds[0].tx));
    EXPECT_TRUE(chains.din_broadcasts.empty());
}

TEST(SwapTower, RefundsBtcEvenWhileDineroIsUnobservable) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.din.ok = false;
    chains.btc.mtp_unix = BobSession().record.offer.t_btc_unix + 1;
    tower.Tick(kNow);
    EXPECT_EQ(chains.btc_broadcasts.size(), 1u);
    EXPECT_TRUE(chains.din_broadcasts.empty());
}

TEST(SwapTower, AnAckIsBoundToThePackageAndRefreshedOnlyWhileHealthy) {
    const auto dir = (std::filesystem::temp_directory_path() / "swap_tower_ack_test").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string id = "0123456789abcdef";
    const std::string h = TowerPackageHash(EncodeTowerPackage(Package()));
    EXPECT_FALSE(TowerAckFresh(dir, id, h, kNow));
    MarkTowerArmed(dir, id, h, kNow);
    EXPECT_TRUE(TowerAckFresh(dir, id, h, kNow + 60));
    EXPECT_FALSE(TowerAckFresh(dir, id, h, kNow + 600)) << "not refreshed: the tower stopped being healthy";
    EXPECT_FALSE(TowerAckFresh(dir, id, std::string(64, '0'), kNow + 60)) << "another package";
    EXPECT_FALSE(TowerAckFresh(dir, "fedcba9876543210", h, kNow + 60)) << "another swap";
    std::filesystem::remove_all(dir);
}

TEST(SwapTower, ItIsHealthyOnlyWithBothChainsAndThePackagesDinLock) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    EXPECT_TRUE(tower.Tick(kNow).healthy);
    chains.btc.ok = false;
    EXPECT_FALSE(tower.Tick(kNow).healthy) << "Bitcoin unreachable";
    chains.btc.ok = true;
    chains.din.ok = false;
    EXPECT_FALSE(tower.Tick(kNow).healthy) << "Dinero unreachable";
    chains.din.ok = true;
    chains.din.funding.reset();
    chains.din.htlc.output_seen = false;
    EXPECT_FALSE(tower.Tick(kNow).healthy) << "the DIN lock of the package is not on chain";
}

TEST(SwapTower, OnceItKnowsTheSecretABitcoinOutageDoesNotStopTheDinClaim) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    const auto r = tower.Tick(kNow);
    ASSERT_TRUE(r.learned_secret.has_value()) << "reported once, for the daemon to keep";
    EXPECT_EQ(*r.learned_secret, kSecret);
    chains.btc.ok = false;
    const size_t before = chains.din_broadcasts.size();
    tower.Tick(kNow + 60);
    EXPECT_GT(chains.din_broadcasts.size(), before);
    // A restarted tower given the kept secret does the same.
    FakeChains c2;
    c2.btc.ok = false;
    Watchtower t2(Package(), Config(), c2);
    t2.SetKnownSecret(kSecret);
    t2.Tick(kNow);
    EXPECT_FALSE(c2.din_broadcasts.empty());
}

TEST(SwapTower, AfterARestartTheBumpReachesWhicheverRungIsInTheMempool) {
    // A rung chosen before the restart sits in mempools; urgency now points
    // elsewhere. The bump must still offer a child of that rung.
    const auto pkg = Package();
    ASSERT_GE(pkg.din_claims.size(), 3u);
    FakeChains chains;
    Watchtower tower(pkg, Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    tower.Tick(kNow);
    chains.din.mtp_unix = BobSession().record.offer.t_din_unix - 10 * kHour;  // the bump is due
    tower.Tick(kNow + 25 * 60);
    bool child_of_rung2 = false;
    for (const auto& raw : chains.din_broadcasts) {
        const auto tx = ParseDin(raw);
        child_of_rung2 |= tx.vin.at(0).prevout.txid == TxId::Compute(pkg.din_claims[2].tx);
    }
    EXPECT_TRUE(child_of_rung2);
}

TEST(SwapTower, PackageChildrenAreVerified) {
    const auto p = Package();
    ASSERT_FALSE(p.din_claims.empty());
    for (const auto& rung : p.din_claims) {
        ASSERT_GE(rung.children.size(), 2u) << "a few fee levels per rung";
        for (const auto& c : rung.children) EXPECT_EQ(c.tx.vin.at(0).prevout.txid, TxId::Compute(rung.tx));
    }
    auto check = [](const char* what, auto mutate) {
        auto q = Package();
        mutate(q);
        EXPECT_THROW(VerifyTowerPackage(q), std::invalid_argument) << what;
    };
    check("child signature", [](TowerPackage& q) { q.din_claims[0].children[0].tx.vin[0].witness[0][5] ^= 1; });
    check("child of another rung", [](TowerPackage& q) { std::swap(q.din_claims[0].children, q.din_claims[1].children); });
    check("child fees not rising", [](TowerPackage& q) {
        std::swap(q.din_claims[0].children[0], q.din_claims[0].children[1]);
    });
    check("child signed to another payout", [](TowerPackage& q) {
        auto s = BobSession();
        s.din_payout_script[10] ^= 1;
        q.din_claims[0].children =
            BuildTowerPackage(s, kBobKeys, DinFunding(s), BtcFundingOf(s), DinPolicy(), BtcPolicy()).din_claims[0].children;
    });
    EXPECT_EQ(EncodeTowerPackage(DecodeTowerPackage(EncodeTowerPackage(p))), EncodeTowerPackage(p));
}

TEST(SwapTower, BumpsAStuckClaimWithAPreSignedChildThenSweeps) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    tower.Tick(kNow);
    ASSERT_EQ(chains.din_broadcasts.size(), 1u);
    const auto claim = ParseDin(chains.din_broadcasts[0]);
    tower.Tick(kNow + 10 * 60);
    EXPECT_EQ(chains.din_broadcasts.size(), 2u) << "re-broadcast only, no child yet";
    tower.Tick(kNow + 25 * 60);  // unmined but T_din far: keep the one bump
    EXPECT_EQ(chains.din_broadcasts.size(), 3u) << "claim re-broadcasts only";
    chains.din.mtp_unix = BobSession().record.offer.t_din_unix - 10 * kHour;  // within 12 h of T_din
    tower.Tick(kNow + 30 * 60);
    bool child_seen = false;
    for (const auto& raw : chains.din_broadcasts) {
        const auto tx = ParseDin(raw);
        if (tx.vin.at(0).prevout.txid != TxId::Compute(claim)) continue;  // the claim itself
        child_seen = true;
        EXPECT_EQ(tx.vout.at(0).scriptPubKey, BobSession().din_payout_script) << "the child pays Bob's wallet";
        // The largest child of that rung.
        bool largest = false;
        for (const auto& rung : Package().din_claims) {
            if (TxId::Compute(rung.tx) == TxId::Compute(claim)) largest = TxId::Compute(rung.children.back().tx) == TxId::Compute(tx);
        }
        EXPECT_TRUE(largest) << "the one bump uses the largest child";
    }
    EXPECT_TRUE(child_seen) << "a child of the claim it broadcast";

    // The claim is mined (as rung 0): sweep it, finish only once swept and deep.
    FakeChains c2;
    Watchtower t2(Package(), Config(), c2);
    c2.AliceClaimsBtc(kSecret);
    const auto pkg = Package();
    FundingOutput mined;
    mined.txid = TxId::Compute(pkg.din_claims[0].tx);
    mined.vout = 0;
    mined.value = pkg.din_claims[0].tx.vout[0].value;
    mined.script_pubkey = pkg.din_claims[0].tx.vout[0].scriptPubKey;
    c2.din.claim_output = mined;
    c2.din.htlc.spent = c2.din.htlc.spent_by_claim = true;
    c2.din.htlc.spend_confirmations = 2;
    c2.claim_output_unspent = true;
    t2.Tick(kNow);
    ASSERT_EQ(c2.din_broadcasts.size(), 1u);
    EXPECT_EQ(ParseDin(c2.din_broadcasts[0]).vin.at(0).prevout.txid, mined.txid) << "sweep of the mined rung";
    c2.din.htlc.spend_confirmations = BobSession().record.offer.n_din_confirmations;
    EXPECT_FALSE(t2.Tick(kNow + 60).finished) << "not swept yet";
    c2.claim_output_unspent = false;
    EXPECT_TRUE(t2.Tick(kNow + 120).finished);
}

TEST(SwapTower, AShallowAdverseRefundDoesNotEndTheTowersDuty) {
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    chains.din.htlc.spent = true;  // Alice's refund, one block deep
    chains.din.htlc.spent_by_claim = false;
    chains.din.htlc.spend_confirmations = 1;
    EXPECT_FALSE(tower.Tick(kNow).finished) << "a reorg can still undo it";
    chains.din.htlc.spent = false;  // undone: claim again
    chains.din.htlc.spend_confirmations = 0;
    const size_t before = chains.din_broadcasts.size();
    tower.Tick(kNow + 60);
    EXPECT_GT(chains.din_broadcasts.size(), before);
    chains.din.htlc.spent = true;
    chains.din.htlc.spend_confirmations = BobSession().record.offer.n_din_confirmations;
    EXPECT_TRUE(tower.Tick(kNow + 120).finished);
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
    chains.din.htlc.spend_confirmations = BobSession().record.offer.n_din_confirmations;
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
    EXPECT_FALSE(tower.Tick(kNow + 600).finished) << "Dinero outcomes settle at N_din, not 6";
    chains.din.htlc.spend_confirmations = BobSession().record.offer.n_din_confirmations;
    EXPECT_TRUE(tower.Tick(kNow + 700).finished);

    // Same for Bob's BTC refund.
    FakeChains c2;
    Watchtower t2(Package(), config, c2);
    c2.btc.mtp_unix = BobSession().record.offer.t_btc_unix + 1;
    c2.btc.htlc.spent = true;
    c2.btc.htlc.spent_by_claim = false;
    c2.btc.htlc.spend_confirmations = 2;
    EXPECT_FALSE(t2.Tick(kNow).finished);
    c2.btc.htlc.spend_confirmations = 6;
    EXPECT_TRUE(t2.Tick(kNow).finished);
}

}  // namespace
