// Swap watchtower: Bob's package holds no key and no secret, is verified when
// loaded (tampering refused), and the tower claims DIN with Alice's revealed
// secret or refunds BTC after T_btc, escalating fees; it never acts blind.
#include "wallet/swap/tower.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

TEST(SwapTower, ACycleRecordsTheAckAndALearnedSecretInTheInbox) {
    const auto dir = (std::filesystem::temp_directory_path() / "swap_tower_cycle_test").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string id = "0123456789abcdef";
    const std::string h = TowerPackageHash(EncodeTowerPackage(Package()));
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    const auto r = TickAndRecord(tower, dir, id, h, kNow);
    EXPECT_TRUE(r.healthy);
    EXPECT_TRUE(TowerAckFresh(dir, id, h, kNow + 60));
    std::ifstream sf(dir + "/" + id + ".secret");
    std::string hex;
    sf >> hex;
    std::string want;
    for (int i = 0; i < 32; ++i) want += "5a";  // kSecret
    EXPECT_EQ(hex, want);
    std::filesystem::remove_all(dir);
}

// A failed write (disk full, read-only inbox) must not escape: the daemon
// ticks every swap in one loop, so a throw would stop Bob's protection for
// all of them. No fresh ack is the safe outcome: Bob does not fund.
TEST(SwapTower, AFailedInboxWriteIsAnAlertNotACrash) {
    const auto file = (std::filesystem::temp_directory_path() / "swap_tower_not_a_dir").string();
    std::filesystem::remove_all(file);
    std::ofstream(file) << "x";
    const std::string inbox = file;  // a file, not a directory: every write into it fails
    const std::string id = "0123456789abcdef";
    const std::string h = TowerPackageHash(EncodeTowerPackage(Package()));
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    TowerReport r;
    ASSERT_NO_THROW(r = TickAndRecord(tower, inbox, id, h, kNow));
    EXPECT_TRUE(r.learned_secret.has_value()) << "the duty itself is unaffected";
    size_t alerts = 0;
    for (const auto& ev : r.events) alerts += ev.find("ALERT") != std::string::npos;
    EXPECT_EQ(alerts, 2u) << "one for the ack, one for the secret";
    EXPECT_FALSE(TowerAckFresh(inbox, id, h, kNow));
    std::filesystem::remove_all(file);
}

TEST(SwapTower, AFailedTickIsAnAlertNotACrash) {
    struct Broken : FakeChains {
        DinWatchReport ObserveDin() override { throw std::runtime_error("rpc exploded"); }
    } chains;
    Watchtower tower(Package(), Config(), chains);
    const auto dir = (std::filesystem::temp_directory_path() / "swap_tower_broken_tick").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TowerReport r;
    ASSERT_NO_THROW(r = TickAndRecord(tower, dir, "0123456789abcdef", "h", kNow));
    EXPECT_FALSE(r.healthy);
    EXPECT_FALSE(r.finished);
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_NE(r.events[0].find("ALERT: tick failed: rpc exploded"), std::string::npos);
    EXPECT_FALSE(TowerAckFresh(dir, "0123456789abcdef", "h", kNow)) << "no ack without a tick";
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

TEST(SwapTower, AMinedClaimThatIsNoRungIsNotSettledWhileItsDinSitsUnswept) {
    // A claim Bob's node built itself (not a package rung) was mined. The tower
    // has no child for it: it must not report the swap settled while the DIN
    // still sits at the sweep key, and must say so.
    FakeChains chains;
    Watchtower tower(Package(), Config(), chains);
    chains.AliceClaimsBtc(kSecret);
    FundingOutput mined;
    mined.txid = TxId(uint256::FromHexUnsafe(std::string(64, 'd')));  // no rung has this txid
    mined.vout = 0;
    mined.value = Package().din_claims[0].tx.vout[0].value;
    mined.script_pubkey = Package().din_claims[0].tx.vout[0].scriptPubKey;
    chains.din.claim_output = mined;
    chains.din.htlc.spent = chains.din.htlc.spent_by_claim = true;
    chains.din.htlc.spend_confirmations = BobSession().record.offer.n_din_confirmations;
    chains.claim_output_unspent = true;
    const auto r = tower.Tick(kNow);
    EXPECT_FALSE(r.finished) << "the DIN is not in Bob's wallet yet";
    bool alerted = false;
    for (const auto& e : r.events) alerted |= e.rfind("ALERT", 0) == 0 && e.find("sweep") != std::string::npos;
    EXPECT_TRUE(alerted);
    chains.claim_output_unspent = false;  // Bob's own node swept it
    EXPECT_TRUE(tower.Tick(kNow + 60).finished);
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

// Golden bytes: dinero-swap-tower is a separate program, so the package text a
// wallet writes to its inbox is a contract between two builds. A stored
// package must keep decoding and re-encoding byte for byte.
// ---- A tower strangers may arm (the public tower's intake) ----

std::string FreshDir(const char* name) {
    const auto dir = (std::filesystem::temp_directory_path() / name).string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

TEST(SwapTowerIntake, AVerifiedPackageIsKeptUnderItsOwnHash) {
    const auto dir = FreshDir("swap_tower_intake_store");
    const std::string text = EncodeTowerPackage(Package());
    const auto r = TowerIntakeStore(dir, text, {});
    EXPECT_EQ(r.status, TowerIntakeStatus::Stored) << r.reason;
    EXPECT_EQ(r.hash, TowerPackageHash(text));
    EXPECT_TRUE(IsTowerPackageHash(r.hash));
    std::ifstream in(dir + "/" + r.hash + ".pkg");
    std::stringstream held;
    held << in.rdbuf();
    EXPECT_EQ(held.str(), text);
    EXPECT_EQ(TowerIntakeStore(dir, text, {}).status, TowerIntakeStatus::AlreadyHeld) << "the same package again";
    // The tower finished with it: sending it again does not bring it back.
    std::filesystem::rename(dir + "/" + r.hash + ".pkg", dir + "/" + r.hash + ".pkg.done");
    EXPECT_EQ(TowerIntakeStore(dir, text, {}).status, TowerIntakeStatus::AlreadyHeld);
    EXPECT_FALSE(std::filesystem::exists(dir + "/" + r.hash + ".pkg"));
    std::filesystem::remove_all(dir);
}

TEST(SwapTowerIntake, WhatTheTowerWouldRefuseIsNeverKept) {
    const auto dir = FreshDir("swap_tower_intake_refuse");
    auto empty = [&] { return std::filesystem::is_empty(dir); };
    EXPECT_EQ(TowerIntakeStore(dir, "hello", {}).status, TowerIntakeStatus::Invalid);
    EXPECT_TRUE(empty());
    // Well formed, but a signature does not verify.
    auto bad = Package();
    bad.din_claims.front().signature[5] ^= 1;
    const auto r = TowerIntakeStore(dir, EncodeTowerPackage(bad), {});
    EXPECT_EQ(r.status, TowerIntakeStatus::Invalid);
    EXPECT_FALSE(r.reason.empty());
    EXPECT_TRUE(empty());
    TowerIntakeLimits small;
    small.max_package_bytes = 100;
    EXPECT_EQ(TowerIntakeStore(dir, EncodeTowerPackage(Package()), small).status, TowerIntakeStatus::TooLarge);
    EXPECT_TRUE(empty());
    TowerIntakeLimits mainnet;
    mainnet.network = SwapNetwork::Mainnet;  // the test package is a regtest one
    EXPECT_EQ(TowerIntakeStore(dir, EncodeTowerPackage(Package()), mainnet).status, TowerIntakeStatus::Invalid);
    EXPECT_TRUE(empty());
    std::filesystem::remove_all(dir);
}

TEST(SwapTowerIntake, AFullInboxTakesNoMoreAndNobodyCanReplaceAnothersPackage) {
    const auto dir = FreshDir("swap_tower_intake_full");
    const std::string mine = EncodeTowerPackage(Package());
    ASSERT_EQ(TowerIntakeStore(dir, mine, {}).status, TowerIntakeStatus::Stored);
    // Another package for the same swap (other fee levels): kept beside mine, not over it.
    auto s = BobSession();
    FeeLadderPolicy din_policy;
    din_policy.start_feerate_una_per_vb = 2;
    din_policy.max_rungs = 3;
    din_policy.max_fee_una = s.record.offer.din_amount_una / 100;
    din_policy.min_payout_una = s.record.offer.din_amount_una / 2;
    BtcFeeLadderPolicy btc_policy;
    btc_policy.max_rungs = 3;
    btc_policy.max_fee_sat = s.record.offer.btc_amount_sat / 100;
    btc_policy.min_payout_sat = s.record.offer.btc_amount_sat / 2;
    const std::string other =
        EncodeTowerPackage(BuildTowerPackage(s, kBobKeys, DinFunding(s), BtcFundingOf(s), din_policy, btc_policy));
    ASSERT_NE(other, mine);
    TowerIntakeLimits one;
    one.max_packages = 1;
    EXPECT_EQ(TowerIntakeStore(dir, other, one).status, TowerIntakeStatus::Full);
    const auto r = TowerIntakeStore(dir, other, {});
    EXPECT_EQ(r.status, TowerIntakeStatus::Stored) << r.reason;
    std::ifstream in(dir + "/" + TowerPackageHash(mine) + ".pkg");
    std::stringstream held;
    held << in.rdbuf();
    EXPECT_EQ(held.str(), mine) << "my package is untouched";
    std::filesystem::remove_all(dir);
}

TEST(SwapTowerIntake, ArmedIsOnlyAFreshAckForExactlyThatPackage) {
    const auto dir = FreshDir("swap_tower_intake_armed");
    const std::string h = TowerPackageHash(EncodeTowerPackage(Package()));
    EXPECT_FALSE(TowerIntakeArmedAge(dir, h, kNow));
    MarkTowerArmed(dir, h, h, kNow);  // the tower names a public package by its hash
    EXPECT_EQ(TowerIntakeArmedAge(dir, h, kNow + 30), std::optional<uint32_t>(30));
    EXPECT_FALSE(TowerIntakeArmedAge(dir, h, kNow + 600)) << "the tower stopped refreshing it";
    EXPECT_FALSE(TowerIntakeArmedAge(dir, std::string(64, '0'), kNow + 30)) << "another package";
    // Only a hash ever reaches the file system.
    {  // a file a path trick would reach, holding what such a request would match
        std::ofstream trick(dir + "/../" + h + ".armed");
        trick << "../" << h << " " << (kNow + 30) << "\n";
    }
    EXPECT_FALSE(TowerIntakeArmedAge(dir, "../" + h, kNow + 30));
    std::filesystem::remove(dir + "/../" + h + ".armed");
    EXPECT_FALSE(IsTowerPackageHash("../etc/passwd"));
    EXPECT_FALSE(IsTowerPackageHash(std::string(64, 'A')));
    std::filesystem::remove_all(dir);
}

TEST(SwapTowerIntake, APackageWhoseBuyerNeverLockedIsDroppedOnlyLongAfterItsDeadline) {
    FakeChains chains;
    chains.btc.funding.reset();
    chains.btc.htlc.output_seen = false;  // Bob armed the tower and never funded
    const auto package = Package();
    Watchtower tower(package, Config(), chains);
    const uint32_t t_din = package.offer.t_din_unix, week = 7 * 24 * kHour;
    EXPECT_FALSE(TowerPackageAbandoned(package, tower.Tick(t_din + week), t_din + week, week)) << "not past the grace";
    EXPECT_TRUE(TowerPackageAbandoned(package, tower.Tick(t_din + week + 1), t_din + week + 1, week));
    // Never while a chain cannot be seen: the lock may be there.
    chains.btc.ok = false;
    EXPECT_FALSE(TowerPackageAbandoned(package, tower.Tick(t_din + 2 * week), t_din + 2 * week, week));
    chains.btc.ok = true;
    chains.din.ok = false;
    EXPECT_FALSE(TowerPackageAbandoned(package, tower.Tick(t_din + 2 * week), t_din + 2 * week, week));
    chains.din.ok = true;
    // Never while his coins are locked, however late.
    chains.btc.funding = BtcFundingOf(BobSession());
    chains.btc.htlc.output_seen = true;
    EXPECT_FALSE(TowerPackageAbandoned(package, tower.Tick(t_din + 2 * week), t_din + 2 * week, week));
}

TEST(SwapTower, GoldenPackageBytesAreStable) {
    const std::string kPackage = "format=dinswap1t1\noffer=dinswap1o010200ca9a3b0000000040420f000000000060bf07c488aad18fda339df07e4fbc47b4f00be71711936f18d04d352ad01890f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f902e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd1320914c6ba03f4b6b1e0000000100000020ee496bc7eacd78\naccept=dinswap1a01c7eacd78cf7f1dc8c4c3d08126dfd74345f40eb179de1aea88ca7e6730b4e46a2f8bde4d1a07209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe403fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a1460297556a9da8cc2\nbtc_scan_from_height=101\ndin_scan_from_height=0\ndin_sweep_pubkey=5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc\ndin_claim=02000000000101aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0100000000fdffffff01886c983b00000000225120dd6d56968f2e02d0ff94c172b1a3d48f06cd66623b67fed2af98f2a41c17aaf00000000000:bc762e901a35adabb7f58783d03c6f59bd36d44a7aa3929c18b2aae125f9d2bfc195be8ca6cd5f111710b5750e00afbb565d15bcd7db63f64000a0181e15da60:155000\ndin_claim=02000000000101aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0100000000fdffffff01100f963b00000000225120dd6d56968f2e02d0ff94c172b1a3d48f06cd66623b67fed2af98f2a41c17aaf00000000000:bc2506c8a1fb64aa66129a6cafbc8cb7334980839be93b06a1b98c7edd931c5deecbe020a57063cdb47500e93cbd76c1ce8c4391b1e09c62f2b30168cfde3a51:310000\ndin_claim=02000000000101aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0100000000fdffffff012054913b00000000225120dd6d56968f2e02d0ff94c172b1a3d48f06cd66623b67fed2af98f2a41c17aaf00000000000:d1c21a8882ca762a9e2aa349853dee4564bbc2114bc0be7974476ac98b27b0379379e2f3b132332d0bbc74c13e4c775cc7cdc5362130be53075bdf8b2a57cbd1:620000\ndin_claim=02000000000101aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0100000000fdffffff0140de873b00000000225120dd6d56968f2e02d0ff94c172b1a3d48f06cd66623b67fed2af98f2a41c17aaf00000000000:0db3be41b774710fd9cd60977fb6d04fa60c6a5977ee149cecc57f88adb25a33efe5aa803832d9fdb0ea8011a7771a0feef3b205a9f368e3303d95f674c3aa53:1240000\ndin_child=0:02000000000101692a5ea5fc0b69ff5a3eb303156a841f278c44953cd7a43ed174d5b0672603af0000000000fdffffff01100f963b00000000225120888888888888888888888888888888888888888888888888888888888888888803407a94b34cdb619e3cc151b156a9c5517de87a950a05aa6bfb7b1d26217523285671101ac8fa525270015d9e74f79bed6fdc752b4cfa83092e4294456914893b5822205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:155000\ndin_child=0:02000000000101692a5ea5fc0b69ff5a3eb303156a841f278c44953cd7a43ed174d5b0672603af0000000000fdffffff01a8f68e3b000000002251208888888888888888888888888888888888888888888888888888888888888888034020e14876bb5732af22575899b5949a33c1541a2a3bcede475e84ff10f78a2d4eafb80b08011eeb17f898c8ae9a60e16017e05eeb00260f95ce22b58677fbf31f22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:620000\ndin_child=0:02000000000101692a5ea5fc0b69ff5a3eb303156a841f278c44953cd7a43ed174d5b0672603af0000000000fdffffff010895723b00000000225120888888888888888888888888888888888888888888888888888888888888888803403f9496c9bff680454702ac2651a6b3c8800f47468764ee452fd3cec18a6edca9849c57e3d5f3fa30fb6110351ed05c30613072f4732377f17350062c09c8d75c22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:2480000\ndin_child=0:02000000000101692a5ea5fc0b69ff5a3eb303156a841f278c44953cd7a43ed174d5b0672603af0000000000fdffffff01880e013b0000000022512088888888888888888888888888888888888888888888888888888888888888880340ab48c7e8676bb42bd394241736891caf1722eb1621f6b41fe045312552f8884734a3e08e8ca128b69d2807656fef3b604150b6ea0a26548beac0b5096e4c70d422205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:9920000\ndin_child=1:020000000001015757b1a61f00730cd3d31d69d622d215494705966a84c1b392e16d814bbcab5f0000000000fdffffff012054913b00000000225120888888888888888888888888888888888888888888888888888888888888888803409baa08297c2d8661a17907aec8a569fe97af16cd90e88b17e09c6e26b79be6201ab52fa85a127d46cc8ce514ae7310d471f285228e81b6d251fd70a84c95c92122205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:310000\ndin_child=1:020000000001015757b1a61f00730cd3d31d69d622d215494705966a84c1b392e16d814bbcab5f0000000000fdffffff015023833b000000002251208888888888888888888888888888888888888888888888888888888888888888034041a8034c99db1d66379977ee0db07bfbdbb541a9748b4b158526599f69c91147b511c6259920e04bd9cb986ec5be00b599ab1dac644024248d7c7ef5f890d55c22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:1240000\ndin_child=1:020000000001015757b1a61f00730cd3d31d69d622d215494705966a84c1b392e16d814bbcab5f0000000000fdffffff0110604a3b000000002251208888888888888888888888888888888888888888888888888888888888888888034084bfa073a846db3cd2dd28b3bbda3fb70e5f4786585ff4339e1f3b6628eb989dd2bb3b86595a7154ebb4f2c8c1e573438fbc6fa2bda4104dbed70e4e70cb423c22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:4960000\ndin_child=1:020000000001015757b1a61f00730cd3d31d69d622d215494705966a84c1b392e16d814bbcab5f0000000000fdffffff011053673a00000000225120888888888888888888888888888888888888888888888888888888888888888803408543c978c69524ac97961bdc456853235c4581bad0ae01ef1decc07f4cb8816feb13bb745cb90bdc1d0a5ce88c6396f808d8386b6e2999de9824ae49218fc82022205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:19840000\ndin_child=2:020000000001018506f7cd6df4602b720077ffe72f44ebedc1520040ba37e032d6da5f78fb12fd0000000000fdffffff0140de873b0000000022512088888888888888888888888888888888888888888888888888888888888888880340b3b0690ba9256d19751440e230b603fa91f9f12704043ba07e26bc968e04a4389d671ab8d1b4ee3e066e88875bf9b77e0aa68590194eb64fc28f2bec373b380d22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:620000\ndin_child=2:020000000001018506f7cd6df4602b720077ffe72f44ebedc1520040ba37e032d6da5f78fb12fd0000000000fdffffff01a07c6b3b000000002251208888888888888888888888888888888888888888888888888888888888888888034019bed08f61fa9d9438ead19d3995465ec9cc60ed1b3c8334176e921bf258b692626a33733e5209f83c8bcdb59db9bf20293891c5a31b7e4ad1e0151b0293a28022205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:2480000\ndin_child=2:020000000001018506f7cd6df4602b720077ffe72f44ebedc1520040ba37e032d6da5f78fb12fd0000000000fdffffff0120f6f93a0000000022512088888888888888888888888888888888888888888888888888888888888888880340e41dc24b0c73ff90e42bc5697eb7bafd82315e2cff8f4d4cf76ec9efcee3ec6a63aec65ca18950ca38c71a7da95503b71c41df35da152523ccaecf2080de523222205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:9920000\ndin_child=2:020000000001018506f7cd6df4602b720077ffe72f44ebedc1520040ba37e032d6da5f78fb12fd0000000000fdffffff0120dc33390000000022512088888888888888888888888888888888888888888888888888888888888888880340e42b496f950e39acffd985085121264206c81272703520ac4909d005625bb7e0c69c26e3de78f08e0ba3744ace73fe95ae6842fd0e7f6bf108439345482aac1022205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:39680000\ndin_child=3:02000000000101f48e6034813d6b3bd426eaa336cca8cdc91267d97040550dd77ed630cea34e4d0000000000fdffffff0180f2743b0000000022512088888888888888888888888888888888888888888888888888888888888888880340b898cc22e7a54567a8431862796bd2b965402abf367af5d41eda184a8adccf0ff07717ad8b3bb0d2e4f3eb6c0df262c75eb8f1e16bbda7ee073110cceecde1e622205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:1240000\ndin_child=3:02000000000101f48e6034813d6b3bd426eaa336cca8cdc91267d97040550dd77ed630cea34e4d0000000000fdffffff01402f3c3b00000000225120888888888888888888888888888888888888888888888888888888888888888803408beded1116a8dadeca0b59a2d657e5af8c0950b01b19a5d5ac7f14e493dc536b3898c90682795ddcec72dbcbd9bcd38f9500b50daefb9592ad5a783e7d4815dd22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:4960000\ndin_child=3:02000000000101f48e6034813d6b3bd426eaa336cca8cdc91267d97040550dd77ed630cea34e4d0000000000fdffffff014022593a00000000225120888888888888888888888888888888888888888888888888888888888888888803404a507286586e5634ec3c0ddb20e3bf4311dbf3a9a7a9b8ecf7932d307afecdb54f1da21839d29fabcb983565df3980a3c0a9ba7dc75a6f75a339ec6dee1b9a1b22205cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bcac21c150929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000:19840000\nbtc_refund=02000000000101bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb0000000000fdffffff0120410f0000000000225120999999999999999999999999999999999999999999999999999999999999999903483045022100f53bd50991183a9d6c45489daad4996260120191cb8f603a3347a6c0ceedf1e602205d89301489707ea9888536af40d6f8143b1a118834b8445a66d3bed336253ad60100766382012088a82060bf07c488aad18fda339df07e4fbc47b4f00be71711936f18d04d352ad01890882102e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd136704a03f4b6bb1752103fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a146029755668aca03f4b6b:288\nbtc_refund=02000000000101bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb0000000000fdffffff0100400f000000000022512099999999999999999999999999999999999999999999999999999999999999990347304402203197accf56affa74b8b469524f80d0d43a2bf2beabf21815f5fd4c810ca8664602207fcf364bb75cc42db96f72362c27efae0536631023b029446016b382f790937f0100766382012088a82060bf07c488aad18fda339df07e4fbc47b4f00be71711936f18d04d352ad01890882102e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd136704a03f4b6bb1752103fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a146029755668aca03f4b6b:576\nbtc_refund=02000000000101bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb0000000000fdffffff01c03d0f0000000000225120999999999999999999999999999999999999999999999999999999999999999903483045022100f5888719c72c8d471b04be50d7241bbaf42a9ccffbe24b4b9571a082d290cdd40220454925fe29c756a17beb9f88876af54fa8664efe1be1969b5de25526562e3d910100766382012088a82060bf07c488aad18fda339df07e4fbc47b4f00be71711936f18d04d352ad01890882102e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd136704a03f4b6bb1752103fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a146029755668aca03f4b6b:1152\nbtc_refund=02000000000101bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb0000000000fdffffff0140390f00000000002251209999999999999999999999999999999999999999999999999999999999999999034830450221008b2c567a6f8dd7a7265878bbc7b83948e574e94a2d41cd05acceb4c44fa4011b022044990098abd77ae46c054ab91f67f8c852a2fbe8d8ec6b69d1de17303356fdd40100766382012088a82060bf07c488aad18fda339df07e4fbc47b4f00be71711936f18d04d352ad01890882102e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd136704a03f4b6bb1752103fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a146029755668aca03f4b6b:2304\n";
    EXPECT_EQ(EncodeTowerPackage(Package()), kPackage);
    EXPECT_EQ(EncodeTowerPackage(DecodeTowerPackage(kPackage)), kPackage);
}

}  // namespace
