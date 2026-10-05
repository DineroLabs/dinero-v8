// Swap runner: the executor contract around the engine (persist before acting,
// never act blind, never fund twice), key checks, and signed spends that verify.
#include "wallet/swap/runner.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/btc_tx.h"
#include "wallet/swap/tower.h"

#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;
constexpr uint32_t kHour = 3600;

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }

Bytes32 XOnly(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair kp;
    const auto s = Scalar(scalar);
    EXPECT_EQ(secp256k1_keypair_create(secp, &kp, s.data()), 1);
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

std::array<uint8_t, 33> Compressed(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Scalar(scalar);
    EXPECT_EQ(secp256k1_ec_pubkey_create(secp, &pk, s.data()), 1);
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();

// Alice: DIN refund key 3, BTC claim key 4. Bob: DIN claim key 5, BTC refund key 6.
const SwapKeys kAliceKeys{Scalar(3), Scalar(4)};
const SwapKeys kBobKeys{Scalar(5), Scalar(6)};

SwapSession MakeSession(Role role) {
    SwapSession s;
    SwapRecord& r = s.record;
    r.role = role;
    auto& o = r.offer;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 10 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = XOnly(3);
    o.btc_claim_pubkey = Compressed(4);
    o.expires_unix = kNow + 2 * kHour;
    o.t_btc_unix = kNow + 26 * kHour;
    o.t_din_unix = kNow + 50 * kHour;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 1;
    r.accept.offer_id = OfferId(o);
    r.accept.din_claim_pubkey = XOnly(5);
    r.accept.btc_refund_pubkey = Compressed(6);
    r.state = SwapState::Accepted;
    r.state_since_unix = kNow;
    if (role == Role::DinSeller) r.secret = kSecret;
    s.btc_scan_from_height = 101;
    s.din_payout_script = std::vector<uint8_t>(34, 0x51);
    s.din_payout_script[1] = 0x20;
    s.btc_payout_script = {0x00, 0x14};
    s.btc_payout_script.resize(22, 0x77);
    return s;
}

// Shared, ordered log of everything that reaches a store or a chain.
struct Log {
    std::vector<std::string> lines;
};

struct FakeStore : SwapStore {
    Log& log;
    bool fail{false};
    std::optional<SwapSession> saved;
    explicit FakeStore(Log& l) : log(l) {}
    void Save(const SwapSession& s) override {
        if (fail) throw std::runtime_error("disk full");
        saved = s;
        log.lines.push_back(std::string("save:") + StateName(s.record.state));
    }
};

struct FakeChains : SwapChainIo {
    Log& log;
    DinWatchReport din;
    BtcWatchReport btc;
    bool refuse_writes{false};
    std::vector<std::vector<uint8_t>> din_broadcasts, btc_broadcasts;
    explicit FakeChains(Log& l) : log(l) {
        din.ok = btc.ok = true;
        din.mtp_unix = btc.mtp_unix = kNow;
    }
    DinWatchReport ObserveDin() override { return din; }
    BtcWatchReport ObserveBtc() override { return btc; }
    std::string Write(const std::string& what) {
        log.lines.push_back(what);
        if (refuse_writes) throw std::runtime_error("node refused");
        return std::string(64, 'f');
    }
    std::string FundDin(const std::string& a, uint64_t v) override { return Write("fund_din:" + a + ":" + std::to_string(v)); }
    std::string FundBtc(const std::string& a, uint64_t v) override { return Write("fund_btc:" + a + ":" + std::to_string(v)); }
    std::string BroadcastDin(const std::vector<uint8_t>& t) override { din_broadcasts.push_back(t); return Write("bcast_din"); }
    std::string BroadcastBtc(const std::vector<uint8_t>& t) override { btc_broadcasts.push_back(t); return Write("bcast_btc"); }
    std::vector<std::pair<std::string, std::string>> pins;
    void PinFunding(const std::string& d, const std::string& b) override {
        pins.push_back({d, b});
        log.lines.push_back("pin");
    }
    int tower_refusals{0};
    std::vector<std::string> armed;
    void ArmTower(const std::string& package) override {
        log.lines.push_back("arm_tower");
        if (tower_refusals > 0) { --tower_refusals; throw std::runtime_error("tower offline"); }
        armed.push_back(package);
    }
};

RunnerConfig Config() {
    RunnerConfig c;
    c.din_hrp = "rdin";
    c.btc_hrp = "bcrt";
    return c;
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

TEST(SwapRunner, SavesTheNewStateBeforeFunding) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    SwapRunner alice(MakeSession(Role::DinSeller), kAliceKeys, Config(), chains, store);
    const auto r = alice.Tick(kNow);
    ASSERT_TRUE(r.observed);
    EXPECT_EQ(r.after, SwapState::DinLockBroadcast);
    ASSERT_GE(log.lines.size(), 2u);  // then: save of the lock txid, pin
    EXPECT_EQ(log.lines[0], std::string("save:") + StateName(SwapState::DinLockBroadcast));
    EXPECT_EQ(log.lines[1], "fund_din:" + DinHtlcAddressFor(alice.session().record, "rdin") + ":1000000000");
}

TEST(SwapRunner, StoreFailureMeansNothingIsDone) {
    Log log;
    FakeStore store(log);
    store.fail = true;
    FakeChains chains(log);
    SwapRunner alice(MakeSession(Role::DinSeller), kAliceKeys, Config(), chains, store);
    EXPECT_THROW(alice.Tick(kNow), std::runtime_error);
    EXPECT_TRUE(log.lines.empty());
    EXPECT_EQ(alice.session().record.state, SwapState::Accepted);
}

TEST(SwapRunner, ReloadedAfterFundingNeverFundsAgain) {
    // Crash after the save but before (or during) the wallet call: the reloaded
    // session says DinLockBroadcast and nothing is on chain yet.
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    auto s = MakeSession(Role::DinSeller);
    s.record.state = SwapState::DinLockBroadcast;
    SwapRunner alice(s, kAliceKeys, Config(), chains, store);
    for (uint32_t t = kNow; t < kNow + 5 * kHour; t += kHour / 2) alice.Tick(t);
    for (const auto& l : log.lines) EXPECT_EQ(l.rfind("fund_", 0), std::string::npos) << l;
}

TEST(SwapRunner, UnreachableChainDecidesNothing) {
    for (int which = 0; which < 2; ++which) {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        (which == 0 ? chains.din.ok : chains.btc.ok) = false;
        SwapRunner alice(MakeSession(Role::DinSeller), kAliceKeys, Config(), chains, store);
        const auto r = alice.Tick(kNow);
        EXPECT_FALSE(r.observed);
        EXPECT_TRUE(log.lines.empty());
        EXPECT_EQ(alice.session().record.state, SwapState::Accepted);
    }
}

TEST(SwapRunner, FailedFundingKeepsTheStateAndReportsIt) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    chains.refuse_writes = true;
    SwapRunner alice(MakeSession(Role::DinSeller), kAliceKeys, Config(), chains, store);
    const auto r = alice.Tick(kNow);
    EXPECT_EQ(alice.session().record.state, SwapState::DinLockBroadcast);
    bool reported = false;
    for (const auto& e : r.events) reported |= e.find("node refused") != std::string::npos;
    EXPECT_TRUE(reported);
}

TEST(SwapRunner, RefusesKeysThatDoNotMatchTheRecord) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    EXPECT_THROW(SwapRunner(MakeSession(Role::DinSeller), kBobKeys, Config(), chains, store), std::invalid_argument);
    EXPECT_THROW(SwapRunner(MakeSession(Role::BtcSeller), kAliceKeys, Config(), chains, store), std::invalid_argument);
    auto no_secret = MakeSession(Role::DinSeller);
    no_secret.record.secret.reset();
    EXPECT_THROW(SwapRunner(no_secret, kAliceKeys, Config(), chains, store), std::invalid_argument);
    EXPECT_NO_THROW(SwapRunner(MakeSession(Role::BtcSeller), kBobKeys, Config(), chains, store));
}

TEST(SwapRunner, BobClaimsDinWithTheSecretFromAlicesBtcClaim) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    auto s = MakeSession(Role::BtcSeller);
    s.record.state = SwapState::BtcLocked;
    SwapRunner bob(s, kBobKeys, Config(), chains, store);
    chains.din.funding = DinFunding(s);
    chains.din.htlc.output_seen = true;
    chains.din.htlc.output_confirmations = 40;
    chains.din.htlc.output_value = s.record.offer.din_amount_una;
    chains.btc.funding = BtcFundingOf(s);
    chains.btc.htlc.output_seen = true;
    chains.btc.htlc.output_confirmations = 3;
    chains.btc.htlc.output_value = s.record.offer.btc_amount_sat;
    chains.btc.htlc.spent = true;
    chains.btc.htlc.spent_by_claim = true;
    chains.btc.htlc.revealed_preimage = kSecret;
    const auto r = bob.Tick(kNow + kHour);
    EXPECT_EQ(r.after, SwapState::DinClaimBroadcast);
    ASSERT_EQ(store.saved->record.secret, kSecret);  // learned secret is saved before claiming
    ASSERT_EQ(log.lines.size(), 2u);
    EXPECT_EQ(log.lines[1], "bcast_din");
    ASSERT_EQ(chains.din_broadcasts.size(), 1u);
    EXPECT_EQ(chains.din_broadcasts[0], SignedDinClaim(bob.session(), kBobKeys, DinFunding(s), Config().din_fee_una));
}

TEST(SwapRunner, SignedSpendsVerify) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    const auto alice = MakeSession(Role::DinSeller);
    auto bob = MakeSession(Role::BtcSeller);
    EXPECT_THROW(SignedDinClaim(bob, kBobKeys, DinFunding(bob), 100'000), std::invalid_argument);  // secret unknown
    bob.record.secret = kSecret;  // learned from Alice's BTC claim
    const auto terms = MakeDinTerms(alice.record.offer, alice.record.accept);
    const auto htlc = BuildDinHtlc(terms);
    const auto fund = DinFunding(alice);

    // DIN claim (Bob) and refund (Alice): BIP340 signatures over the leaf sighash.
    for (bool claim : {true, false}) {
        const auto raw = claim ? SignedDinClaim(bob, kBobKeys, fund, 100'000)
                               : SignedDinRefund(alice, kAliceKeys, fund, 100'000);
        Transaction tx;
        size_t used = 0;
        ASSERT_TRUE(TransactionSerializer::Deserialize(tx, raw, used));
        ASSERT_EQ(tx.vin.size(), 1u);
        const auto& w = tx.vin[0].witness;
        const Bytes32 msg = claim ? DinClaimSighash(tx, fund, htlc) : DinRefundSighash(tx, fund, htlc);
        secp256k1_xonly_pubkey pk;
        const Bytes32 key = claim ? terms.claim_pubkey : terms.refund_pubkey;
        ASSERT_EQ(secp256k1_xonly_pubkey_parse(secp, &pk, key.data()), 1);
        ASSERT_EQ(w[0].size(), 64u);
        EXPECT_EQ(secp256k1_schnorrsig_verify(secp, w[0].data(), msg.data(), 32, &pk), 1) << claim;
        if (claim) EXPECT_EQ(w[1], std::vector<uint8_t>(kSecret.begin(), kSecret.end()));
        EXPECT_EQ(tx.vout[0].scriptPubKey, bob.din_payout_script);
    }

    // BTC claim (Alice) and refund (Bob): low-S DER ECDSA + SIGHASH_ALL.
    const auto bterms = MakeBtcTerms(alice.record.offer, alice.record.accept);
    const auto bfund = BtcFundingOf(alice);
    for (bool claim : {true, false}) {
        const auto tx = ParseBtcTx(claim ? SignedBtcClaim(alice, kAliceKeys, bfund, 1'000)
                                         : SignedBtcRefund(bob, kBobKeys, bfund, 1'000));
        const auto& w = tx.vin.at(0).witness;
        ASSERT_GE(w.size(), 3u);
        EXPECT_EQ(w[0].back(), 0x01);
        secp256k1_ecdsa_signature sig;
        ASSERT_EQ(secp256k1_ecdsa_signature_parse_der(secp, &sig, w[0].data(), w[0].size() - 1), 1);
        secp256k1_pubkey pk;
        const auto& key = claim ? bterms.claim_pubkey : bterms.refund_pubkey;
        ASSERT_EQ(secp256k1_ec_pubkey_parse(secp, &pk, key.data(), key.size()), 1);
        const auto msg = BtcHtlcSighash(tx, bterms, bfund);
        EXPECT_EQ(secp256k1_ecdsa_verify(secp, &sig, msg.data(), &pk), 1) << claim;
        if (claim) EXPECT_EQ(w[1], std::vector<uint8_t>(kSecret.begin(), kSecret.end()));
        EXPECT_EQ(tx.vout.at(0).value_sat, bfund.value_sat - 1'000);
    }

    // Wrong party's keys are refused rather than producing a useless spend.
    EXPECT_THROW(SignedDinClaim(bob, kAliceKeys, fund, 100'000), std::invalid_argument);
    EXPECT_THROW(SignedBtcClaim(alice, kBobKeys, bfund, 1'000), std::invalid_argument);
}

TEST(SwapRunner, RpcIoReadsTheNodesRealResultShapes) {
    // Shapes seen on regtest: dinerod's sendrawtransaction nests the txid as
    // {"result": "<txid>"}; wallet calls report failure in-band as {"error": ...}.
    Json::Value din_result;
    const DinRpc din = [&](const std::string&, const Json::Value&) { return std::optional<Json::Value>(din_result); };
    const BtcRpc btc = [&](const std::string&, const Json::Value&) { return std::optional<Json::Value>(Json::Value("b1")); };
    RpcSwapChainIo io(din, btc, MakeSession(Role::BtcSeller), Config());

    din_result = Json::Value(Json::objectValue);
    din_result["result"] = "d1";
    EXPECT_EQ(io.BroadcastDin({0x00}), "d1");
    din_result = Json::Value("d2");
    EXPECT_EQ(io.BroadcastDin({0x00}), "d2");
    din_result = Json::Value(Json::objectValue);
    din_result["txid"] = "d3";
    EXPECT_EQ(io.FundDin("rdin1x", 5), "d3");
    din_result = Json::Value(Json::objectValue);
    din_result["error"] = "No confirmed UTXOs available";
    EXPECT_THROW(io.FundDin("rdin1x", 5), std::runtime_error);
    EXPECT_EQ(io.BroadcastBtc({0x00}), "b1");
}

void BothLocksSeen(FakeChains& chains, const SwapSession& s) {
    chains.din.funding = DinFunding(s);
    chains.din.htlc.output_seen = true;
    chains.din.htlc.output_confirmations = 40;
    chains.din.htlc.output_value = s.record.offer.din_amount_una;
    chains.btc.funding = BtcFundingOf(s);
    chains.btc.htlc.output_seen = true;
    chains.btc.htlc.output_confirmations = 1;
    chains.btc.htlc.output_value = s.record.offer.btc_amount_sat;
}

TEST(SwapRunner, BobArmsTheTowerOnceBothLocksAreSeen) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    auto s = MakeSession(Role::BtcSeller);
    s.record.state = SwapState::BtcLockBroadcast;
    BothLocksSeen(chains, s);
    auto config = Config();
    config.use_tower = true;
    SwapRunner bob(s, kBobKeys, config, chains, store);
    bob.Tick(kNow + kHour);
    EXPECT_EQ(bob.session().record.state, SwapState::BtcLocked);
    ASSERT_EQ(chains.armed.size(), 1u);
    const auto package = DecodeTowerPackage(chains.armed[0]);  // verifies every rung
    EXPECT_EQ(package.din_claims.front().tx.vout[0].scriptPubKey, s.din_payout_script);
    EXPECT_TRUE(store.saved->tower_armed);
    EXPECT_TRUE(bob.session().tower_armed);
    bob.Tick(kNow + 2 * kHour);
    EXPECT_EQ(chains.armed.size(), 1u) << "armed once";
}

TEST(SwapRunner, TowerArmingIsRetriedUntilAccepted) {
    Log log;
    FakeStore store(log);
    FakeChains chains(log);
    chains.tower_refusals = 2;
    auto s = MakeSession(Role::BtcSeller);
    s.record.state = SwapState::BtcLocked;
    BothLocksSeen(chains, s);
    auto config = Config();
    config.use_tower = true;
    SwapRunner bob(s, kBobKeys, config, chains, store);
    auto r = bob.Tick(kNow + kHour);
    EXPECT_FALSE(bob.session().tower_armed);
    bool alerted = false;
    for (const auto& e : r.events) alerted |= e.rfind("ALERT: watchtower not armed", 0) == 0;
    EXPECT_TRUE(alerted);
    bob.Tick(kNow + kHour + 60);
    bob.Tick(kNow + kHour + 120);
    EXPECT_TRUE(bob.session().tower_armed);
    EXPECT_EQ(chains.armed.size(), 1u);
}

TEST(SwapRunner, BobsDinClaimFeeFollowsUrgency) {
    // No replace-by-fee on Dinero: Bob's first DIN claim must already carry a
    // fee fit for the time left before Alice's refund opens.
    auto fee_at = [](uint32_t hours_before_t_din) {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        auto s = MakeSession(Role::BtcSeller);
        s.record.state = SwapState::BtcLocked;
        BothLocksSeen(chains, s);
        chains.btc.htlc.spent = true;
        chains.btc.htlc.spent_by_claim = true;
        chains.btc.htlc.revealed_preimage = kSecret;
        chains.din.mtp_unix = s.record.offer.t_din_unix - hours_before_t_din * kHour;
        SwapRunner bob(s, kBobKeys, Config(), chains, store);
        bob.Tick(kNow + kHour);
        EXPECT_EQ(chains.din_broadcasts.size(), 1u);
        Transaction tx;
        size_t used = 0;
        EXPECT_TRUE(TransactionSerializer::Deserialize(tx, chains.din_broadcasts.at(0), used));
        return s.record.offer.din_amount_una - tx.vout.at(0).value.GetUna();
    };
    const auto c = Config();
    EXPECT_EQ(fee_at(40), c.din_fee_una);
    EXPECT_EQ(fee_at(2), c.din_fee_urgent_una);
    const auto mid = fee_at(15);
    EXPECT_GT(mid, c.din_fee_una);
    EXPECT_LT(mid, c.din_fee_urgent_una);
}

TEST(SwapRunner, LockTransactionsArePersistedAndPinned) {
    // Alice: her own DIN funding txid, as returned by her wallet.
    {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        SwapRunner alice(MakeSession(Role::DinSeller), kAliceKeys, Config(), chains, store);
        alice.Tick(kNow);
        EXPECT_EQ(store.saved->din_funding_txid, std::string(64, 'f'));
        ASSERT_FALSE(chains.pins.empty());
        EXPECT_EQ(chains.pins.back().first, std::string(64, 'f'));
    }
    // Bob: Alice's DIN lock is pinned BEFORE his BTC leaves, then his own BTC funding.
    {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        auto s = MakeSession(Role::BtcSeller);
        BothLocksSeen(chains, s);
        chains.btc = BtcWatchReport{};
        chains.btc.ok = true;
        chains.btc.mtp_unix = kNow;
        chains.din.htlc.output_confirmations = 40;
        SwapRunner bob(s, kBobKeys, Config(), chains, store);
        bob.Tick(kNow + kHour);
        EXPECT_EQ(bob.session().record.state, SwapState::BtcLockBroadcast);
        const std::string din_txid = DinFunding(s).txid.AsUint256().GetHex();
        EXPECT_EQ(store.saved->din_funding_txid, din_txid);
        EXPECT_EQ(store.saved->btc_funding_txid, std::string(64, 'f'));
        // Order: the DIN pin is saved before the BTC funding call.
        const auto pin_din = std::find(log.lines.begin(), log.lines.end(), "pin");
        const auto fund = std::find_if(log.lines.begin(), log.lines.end(),
                                       [](const std::string& l) { return l.rfind("fund_btc", 0) == 0; });
        ASSERT_NE(fund, log.lines.end());
        EXPECT_LT(pin_din - log.lines.begin(), fund - log.lines.begin());
    }
}

TEST(SwapRunner, AlicesBtcClaimFeeRisesWhileItIsUnconfirmed) {
    auto fee_after = [](uint32_t seconds) {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        auto s = MakeSession(Role::DinSeller);
        s.record.state = SwapState::BtcClaimBroadcast;
        s.record.state_since_unix = kNow;
        BothLocksSeen(chains, s);
        chains.din.htlc.output_confirmations = 40;
        chains.btc.htlc.spent = true;  // our claim, unconfirmed in the mempool
        chains.btc.htlc.spent_by_claim = true;
        chains.btc.htlc.revealed_preimage = kSecret;
        SwapRunner alice(s, kAliceKeys, Config(), chains, store);
        alice.Tick(kNow + seconds);
        EXPECT_EQ(chains.btc_broadcasts.size(), 1u);
        return s.record.offer.btc_amount_sat - ParseBtcTx(chains.btc_broadcasts.at(0)).vout.at(0).value_sat;
    };
    const uint64_t base = Config().btc_fee_sat;
    EXPECT_EQ(fee_after(60), base);
    EXPECT_EQ(fee_after(31 * 60), 2 * base);
    EXPECT_EQ(fee_after(65 * 60), 4 * base);
    // Capped at btc_fee_max_percent of the amount.
    EXPECT_EQ(fee_after(48 * 3600), MakeSession(Role::DinSeller).record.offer.btc_amount_sat *
                                        Config().btc_fee_max_percent / 100);
}

TEST(SwapRunner, AnOutageOfOneChainDoesNotBlockTheOthersUrgentRebroadcasts) {
    auto run = [](Role role, SwapState state, bool din_ok, bool btc_ok, bool claim_seen, bool secret) {
        Log log;
        FakeStore store(log);
        FakeChains chains(log);
        auto s = MakeSession(role);
        s.record.state = state;
        s.record.state_since_unix = kNow;
        s.record.claim_seen = claim_seen;
        if (secret) s.record.secret = kSecret;
        BothLocksSeen(chains, s);
        chains.din.ok = din_ok;
        chains.btc.ok = btc_ok;
        chains.btc.mtp_unix = s.record.offer.t_btc_unix + 3600;
        chains.din.mtp_unix = s.record.offer.t_din_unix + 3600;
        SwapRunner r(s, role == Role::DinSeller ? kAliceKeys : kBobKeys, Config(), chains, store);
        const auto rep = r.Tick(s.record.offer.t_btc_unix + 7200);
        EXPECT_FALSE(rep.observed);
        EXPECT_EQ(r.session().record.state, state) << "no decision without both chains";
        EXPECT_TRUE(log.lines.empty() || log.lines[0].rfind("save", 0) != 0) << "nothing saved";
        return std::make_pair(chains.din_broadcasts.size(), chains.btc_broadcasts.size());
    };
    // Alice's claim was seen (secret public): keep it going even if Dinero is down.
    EXPECT_EQ(run(Role::DinSeller, SwapState::BtcClaimBroadcast, false, true, true, true).second, 1u);
    // ...but never re-send an unseen claim blind.
    EXPECT_EQ(run(Role::DinSeller, SwapState::BtcClaimBroadcast, false, true, false, true).second, 0u);
    // Bob's BTC refund needs only Bitcoin.
    EXPECT_EQ(run(Role::BtcSeller, SwapState::BtcRefundBroadcast, false, true, false, false).second, 1u);
    // Bob's DIN claim (secret known) and Alice's DIN refund need only Dinero.
    EXPECT_EQ(run(Role::BtcSeller, SwapState::DinClaimBroadcast, true, false, false, true).first, 1u);
    EXPECT_EQ(run(Role::DinSeller, SwapState::DinRefundBroadcast, true, false, false, true).first, 1u);
    // Nothing new is started blind.
    const auto none = run(Role::BtcSeller, SwapState::BtcLocked, false, true, false, false);
    EXPECT_EQ(none.first + none.second, 0u);
}

TEST(SwapRunner, SessionRoundTripsThroughTheFileStore) {
    const auto path = (std::filesystem::temp_directory_path() / "swap_runner_test_session.txt").string();
    std::remove(path.c_str());
    auto s = MakeSession(Role::DinSeller);
    s.record.state = SwapState::DinLocked;
    PlaintextFileSwapStore(path).Save(s);
    const auto back = PlaintextFileSwapStore::Load(path);
    EXPECT_EQ(EncodeSession(back), EncodeSession(s));
    EXPECT_EQ(back.record.state, SwapState::DinLocked);
    EXPECT_EQ(back.btc_scan_from_height, 101u);
    EXPECT_EQ(back.din_payout_script, s.din_payout_script);
    EXPECT_FALSE(back.tower_armed);
    s.din_funding_txid = std::string(64, 'a');
    s.btc_funding_txid = std::string(64, 'b');
    s.din_scan_from_height = 77;
    const auto pinned = DecodeSession(EncodeSession(s));
    EXPECT_EQ(pinned.din_funding_txid, s.din_funding_txid);
    EXPECT_EQ(pinned.btc_funding_txid, s.btc_funding_txid);
    EXPECT_EQ(pinned.din_scan_from_height, 77u);
    s.tower_armed = true;
    EXPECT_TRUE(DecodeSession(EncodeSession(s)).tower_armed);
    std::remove(path.c_str());
    EXPECT_THROW(DecodeSession("record=dinswap1rzz\n"), std::invalid_argument);
}

}  // namespace
