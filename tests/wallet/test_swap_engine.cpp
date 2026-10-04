// Swap engine: every path of design §6.1 driven by scripted chain observations,
// plus persistence and crash/restart idempotence at every state.
#include "wallet/swap/engine.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <stdexcept>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;
constexpr uint32_t kHour = 3600;

std::array<uint8_t, 32> Secret(uint8_t s) { std::array<uint8_t, 32> a{}; a.back() = s; return a; }

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

const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();

Bytes32 HashOf(const Bytes32& s) {
    Bytes32 h{};
    crypto::CSHA256().Write(s.data(), s.size()).Finalize(h.data());
    return h;
}

SwapRecord MakeRecord(Role role) {
    SwapRecord r;
    r.role = role;
    auto& o = r.offer;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 1'000 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    o.payment_hash = HashOf(kSecret);
    o.din_refund_pubkey = XOnly(3);
    o.btc_claim_pubkey = Compressed(4);
    o.expires_unix = kNow + 2 * kHour;
    o.t_btc_unix = kNow + 26 * kHour;
    o.t_din_unix = kNow + 50 * kHour;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 2;
    r.accept.offer_id = OfferId(o);
    r.accept.din_claim_pubkey = XOnly(5);
    r.accept.btc_refund_pubkey = Compressed(6);
    r.state = SwapState::Accepted;
    r.state_since_unix = kNow;
    if (role == Role::DinSeller) r.secret = kSecret;
    return r;
}

Observations At(uint32_t wall) {
    Observations o;
    o.wall_clock_unix = wall;
    o.din_mtp_unix = wall;
    o.btc_mtp_unix = wall;
    return o;
}

HtlcObservation Locked(uint64_t value, uint32_t confs) {
    HtlcObservation h;
    h.output_seen = true;
    h.output_value = value;
    h.output_confirmations = confs;
    return h;
}

HtlcObservation Spent(HtlcObservation h, bool by_claim, uint32_t confs,
                      std::optional<Bytes32> preimage = std::nullopt) {
    h.spent = true;
    h.spent_by_claim = by_claim;
    h.spend_confirmations = confs;
    h.revealed_preimage = preimage;
    return h;
}

bool Has(const StepResult& r, ActionKind k) {
    return std::any_of(r.actions.begin(), r.actions.end(), [&](const Action& a) { return a.kind == k; });
}
size_t Count(const StepResult& r, ActionKind k) {
    return std::count_if(r.actions.begin(), r.actions.end(), [&](const Action& a) { return a.kind == k; });
}
bool MovesFunds(const StepResult& r) {
    return std::any_of(r.actions.begin(), r.actions.end(), [](const Action& a) {
        return a.kind != ActionKind::Alert && a.kind != ActionKind::ArmTower;
    });
}

constexpr uint64_t kDin = 1'000 * 100'000'000ULL;
constexpr uint64_t kBtc = 1'000'000;

}  // namespace

// --- Alice (DIN seller) ----------------------------------------------------

TEST(SwapEngine, AliceHappyPath) {
    auto r = MakeRecord(Role::DinSeller);
    auto obs = At(kNow);
    auto s = Step(r, obs);
    EXPECT_EQ(Count(s, ActionKind::FundDinHtlc), 1U);
    EXPECT_EQ(s.record.state, SwapState::DinLockBroadcast);

    auto again = Step(s.record, obs);
    EXPECT_FALSE(Has(again, ActionKind::FundDinHtlc)) << "funding is requested exactly once";

    obs.din = Locked(kDin, 1);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::DinLocked);

    obs.btc = Locked(kBtc, 1);  // below N_btc
    s = Step(s.record, obs);
    EXPECT_FALSE(Has(s, ActionKind::ClaimBtc));

    obs.btc = Locked(kBtc, 2);
    s = Step(s.record, obs);
    EXPECT_TRUE(Has(s, ActionKind::ClaimBtc));
    EXPECT_EQ(s.record.state, SwapState::BtcClaimBroadcast);

    obs.btc = Spent(obs.btc, /*by_claim=*/true, /*confs=*/2, kSecret);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::Done);
}

TEST(SwapEngine, AliceRefusesWrongBtcAmount) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLocked;
    auto obs = At(kNow + kHour);
    obs.din = Locked(kDin, 40);
    obs.btc = Locked(kBtc - 1, 5);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::ClaimBtc));
    EXPECT_TRUE(Has(s, ActionKind::Alert));
}

TEST(SwapEngine, AliceNeverClaimsPastTheCutoffAndRefundsDinLater) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLocked;
    auto obs = At(r.offer.t_btc_unix - kAliceClaimCutoffSeconds);  // exactly at the cut-off
    obs.din = Locked(kDin, 40);
    obs.btc = Locked(kBtc, 5);
    auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::ClaimBtc)) << "too close to Bob's refund";

    obs = At(r.offer.t_din_unix - 1);
    obs.din = Locked(kDin, 400);
    s = Step(s.record, obs);
    EXPECT_FALSE(Has(s, ActionKind::RefundDin)) << "refund not open yet";

    obs.din_mtp_unix = r.offer.t_din_unix;
    obs.wall_clock_unix = r.offer.t_din_unix + 600;
    s = Step(s.record, obs);
    EXPECT_TRUE(Has(s, ActionKind::RefundDin));
    EXPECT_EQ(s.record.state, SwapState::DinRefundBroadcast);

    obs.din = Spent(obs.din, /*by_claim=*/false, 1);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::Refunded);
}

TEST(SwapEngine, AliceRebroadcastsADroppedClaim) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::BtcClaimBroadcast;
    auto obs = At(kNow + 3 * kHour);
    obs.din = Locked(kDin, 60);
    obs.btc = Locked(kBtc, 5);  // claim not seen (dropped from mempool / reorged)
    const auto s = Step(r, obs);
    EXPECT_TRUE(Has(s, ActionKind::ClaimBtc));
    EXPECT_EQ(s.record.state, SwapState::BtcClaimBroadcast);
}

TEST(SwapEngine, AliceLosingTheRaceRefundsDinAndAlerts) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::BtcClaimBroadcast;
    auto obs = At(r.offer.t_btc_unix + kHour);
    obs.din = Locked(kDin, 600);
    obs.btc = Spent(Locked(kBtc, 50), /*by_claim=*/false, 1);  // Bob's refund won
    auto s = Step(r, obs);
    EXPECT_TRUE(Has(s, ActionKind::Alert));
    EXPECT_FALSE(Has(s, ActionKind::RefundDin)) << "DIN refund not open yet";

    obs = At(r.offer.t_din_unix + 60);
    obs.din = Locked(kDin, 900);
    obs.btc = Spent(Locked(kBtc, 50), false, 10);
    s = Step(s.record, obs);
    EXPECT_TRUE(Has(s, ActionKind::RefundDin));

    obs.din = Spent(obs.din, /*by_claim=*/true, 1, kSecret);  // Bob used the leaked secret first
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::Lost);
    EXPECT_TRUE(Has(s, ActionKind::Alert));
}

TEST(SwapEngine, MissingFundingAlertsInsteadOfFundingAgain) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLockBroadcast;
    r.state_since_unix = kNow;
    const auto s = Step(r, At(kNow + kFundingAlertAfterSeconds + 1));
    EXPECT_FALSE(Has(s, ActionKind::FundDinHtlc));
    EXPECT_TRUE(Has(s, ActionKind::Alert));
}

// --- Bob (BTC seller) ------------------------------------------------------

TEST(SwapEngine, BobHappyPath) {
    auto r = MakeRecord(Role::BtcSeller);
    auto obs = At(kNow);
    auto s = Step(r, obs);
    EXPECT_FALSE(MovesFunds(s)) << "nothing until Alice's DIN lock is deep";

    obs.din = Locked(kDin, 29);
    s = Step(s.record, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));

    obs.din = Locked(kDin, 30);
    s = Step(s.record, obs);
    EXPECT_EQ(Count(s, ActionKind::FundBtcHtlc), 1U);
    EXPECT_EQ(s.record.state, SwapState::BtcLockBroadcast);
    EXPECT_FALSE(Has(Step(s.record, obs), ActionKind::FundBtcHtlc)) << "funding requested once";

    obs.btc = Locked(kBtc, 1);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::BtcLocked);
    EXPECT_TRUE(Has(s, ActionKind::ArmTower));

    obs.btc = Spent(obs.btc, /*by_claim=*/true, /*confs=*/0, kSecret);  // mempool reveal
    s = Step(s.record, obs);
    EXPECT_TRUE(Has(s, ActionKind::ClaimDin));
    EXPECT_EQ(s.record.state, SwapState::DinClaimBroadcast);
    ASSERT_TRUE(s.record.secret.has_value());
    EXPECT_EQ(*s.record.secret, kSecret);

    obs.din = Spent(obs.din, /*by_claim=*/true, 1, kSecret);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::Done);
}

TEST(SwapEngine, BobChecksBeforeLocking) {
    auto r = MakeRecord(Role::BtcSeller);
    auto obs = At(kNow);
    obs.din = Locked(kDin - 1, 50);
    auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc)) << "wrong DIN amount";
    EXPECT_TRUE(Has(s, ActionKind::Alert));

    obs.din = Locked(kDin, 50);
    obs.din_mtp_unix = kNow - kMaxMtpLagSeconds - 1;  // Dinero looks stalled
    s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));

    obs = At(kNow);
    obs.din = Locked(kDin, 50);
    obs.btc_mtp_unix = kNow - kMaxMtpLagSeconds - 1;  // Bitcoin looks stalled
    s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));

    obs = At(r.offer.expires_unix + 1);  // offer expired before the lock was deep
    obs.din = Locked(kDin, 50);
    s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));
    EXPECT_EQ(s.record.state, SwapState::Aborted);
}

TEST(SwapEngine, BobRefundsWhenTheSecretNeverAppears) {
    auto r = MakeRecord(Role::BtcSeller);
    r.state = SwapState::BtcLocked;
    auto obs = At(r.offer.t_btc_unix + kHour);
    obs.btc_mtp_unix = r.offer.t_btc_unix - 1;
    obs.din = Locked(kDin, 500);
    obs.btc = Locked(kBtc, 100);
    auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::RefundBtc)) << "Bitcoin MTP has not reached T_btc";

    obs.btc_mtp_unix = r.offer.t_btc_unix;
    s = Step(r, obs);
    EXPECT_TRUE(Has(s, ActionKind::RefundBtc));
    EXPECT_EQ(s.record.state, SwapState::BtcRefundBroadcast);

    obs.btc = Spent(obs.btc, /*by_claim=*/false, 1);
    s = Step(s.record, obs);
    EXPECT_EQ(s.record.state, SwapState::Refunded);
}

TEST(SwapEngine, BobClaimsDinWhenAliceWinsTheRefundRace) {
    auto r = MakeRecord(Role::BtcSeller);
    r.state = SwapState::BtcRefundBroadcast;
    auto obs = At(r.offer.t_btc_unix + kHour);
    obs.din = Locked(kDin, 500);
    obs.btc = Spent(Locked(kBtc, 100), /*by_claim=*/true, 0, kSecret);
    const auto s = Step(r, obs);
    EXPECT_TRUE(Has(s, ActionKind::ClaimDin));
    EXPECT_EQ(s.record.state, SwapState::DinClaimBroadcast);
}

TEST(SwapEngine, BobRebroadcastsOrReportsALostDinClaim) {
    auto r = MakeRecord(Role::BtcSeller);
    r.state = SwapState::DinClaimBroadcast;
    r.secret = kSecret;
    auto obs = At(kNow + 5 * kHour);
    obs.din = Locked(kDin, 300);  // claim not seen
    obs.btc = Spent(Locked(kBtc, 10), true, 3, kSecret);
    auto s = Step(r, obs);
    EXPECT_TRUE(Has(s, ActionKind::ClaimDin));

    obs.din = Spent(obs.din, /*by_claim=*/false, 1);  // Alice's refund beat Bob's claim
    s = Step(r, obs);
    EXPECT_EQ(s.record.state, SwapState::Lost);
    EXPECT_TRUE(Has(s, ActionKind::Alert));
}

TEST(SwapEngine, BobIgnoresAPreimageThatDoesNotMatch) {
    auto r = MakeRecord(Role::BtcSeller);
    r.state = SwapState::BtcLocked;
    auto obs = At(kNow + 3 * kHour);
    obs.din = Locked(kDin, 300);
    Bytes32 wrong = kSecret;
    wrong[0] ^= 1;
    obs.btc = Spent(Locked(kBtc, 10), true, 0, wrong);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::ClaimDin));
    EXPECT_TRUE(Has(s, ActionKind::Alert));
    EXPECT_FALSE(s.record.secret.has_value());
}

// --- Persistence and crash/restart -----------------------------------------

TEST(SwapEngine, RecordsRoundTripAndRejectCorruption) {
    for (Role role : {Role::DinSeller, Role::BtcSeller}) {
        for (uint8_t st = 0; st <= static_cast<uint8_t>(SwapState::Lost); ++st) {
            auto r = MakeRecord(role);
            r.state = static_cast<SwapState>(st);
            r.state_since_unix = kNow + st;
            const auto text = EncodeRecord(r);
            const auto back = DecodeRecord(text);
            EXPECT_EQ(EncodeRecord(back), text) << StateName(r.state);
            EXPECT_EQ(back.state, r.state);
            EXPECT_EQ(back.secret.has_value(), r.secret.has_value());
        }
    }
    auto text = EncodeRecord(MakeRecord(Role::DinSeller));
    text[text.size() / 2] = text[text.size() / 2] == 'a' ? 'b' : 'a';
    EXPECT_THROW(DecodeRecord(text), std::invalid_argument);
}

TEST(SwapEngine, RestartAtAnyStateGivesTheSameDecision) {
    // Replay Bob's happy path; at each step "crash", reload the record from its
    // persisted form and confirm the decision is identical.
    auto r = MakeRecord(Role::BtcSeller);
    std::vector<Observations> script;
    auto obs = At(kNow);
    script.push_back(obs);
    obs.din = Locked(kDin, 30); script.push_back(obs);
    obs.btc = Locked(kBtc, 1); script.push_back(obs);
    obs.btc = Spent(obs.btc, true, 0, kSecret); script.push_back(obs);
    obs.din = Spent(obs.din, true, 1, kSecret); script.push_back(obs);
    for (const auto& o : script) {
        const auto live = Step(r, o);
        const auto reloaded = Step(DecodeRecord(EncodeRecord(r)), o);
        EXPECT_EQ(EncodeRecord(live.record), EncodeRecord(reloaded.record));
        ASSERT_EQ(live.actions.size(), reloaded.actions.size());
        for (size_t i = 0; i < live.actions.size(); ++i) {
            EXPECT_EQ(live.actions[i].kind, reloaded.actions[i].kind);
        }
        r = live.record;
    }
    EXPECT_EQ(r.state, SwapState::Done);
}

TEST(SwapEngine, AliceRefundWaitsForDinMedianTimeNotTheWallClock) {
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLocked;
    auto obs = At(r.offer.t_din_unix + 3 * kHour);  // wall clock is past the deadline
    obs.din_mtp_unix = r.offer.t_din_unix - 1;       // but consensus is not
    obs.din = Locked(kDin, 900);
    EXPECT_FALSE(Has(Step(r, obs), ActionKind::RefundDin))
        << "a refund broadcast before MTP reaches T_din is invalid";
}

TEST(SwapEngine, BobAbortsWhenTheDinDeadlineIsUnder36HoursAway) {
    auto r = MakeRecord(Role::BtcSeller);
    r.offer.expires_unix = r.offer.t_din_unix - 30 * kHour;  // still open at the check below
    auto obs = At(r.offer.t_din_unix - 35 * kHour);
    obs.din = Locked(kDin, 50);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));
    EXPECT_EQ(s.record.state, SwapState::Aborted);
}

TEST(SwapEngine, ARecordWhoseSecretDoesNotMatchIsRejected) {
    auto r = MakeRecord(Role::DinSeller);
    Bytes32 wrong = kSecret;
    wrong[31] ^= 1;
    r.secret = wrong;  // corrupted on disk: Alice's claim would be invalid
    EXPECT_THROW(DecodeRecord(EncodeRecord(r)), std::invalid_argument);
}

TEST(SwapEngine, AliceNeverClaimsAnAlreadySpentBtcLock) {
    // Alice was offline; Bob's refund already took the BTC. Claiming now
    // cannot succeed and would leak the secret for nothing.
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLocked;
    auto obs = At(kNow + 3 * kHour);
    obs.din = Locked(kDin, 300);
    obs.btc = Spent(Locked(kBtc, 50), /*by_claim=*/false, 5);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::ClaimBtc));
}

TEST(SwapEngine, BobNeverLocksAgainstAnAlreadySpentDinLock) {
    // Alice already refunded (or anyone spent) the DIN lock: BTC locked now
    // could only be taken by Alice with her secret.
    auto r = MakeRecord(Role::BtcSeller);
    auto obs = At(kNow + kHour);
    obs.din = Spent(Locked(kDin, 40), /*by_claim=*/false, 3);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));
}

TEST(SwapEngine, BobJudgesTheDinDeadlineByChainTimeToo) {
    // Bob's clock is behind the chain: by his wall clock T_din is 49 h away,
    // but Dinero's median time is 20 h from it, so Alice's refund opens soon.
    auto r = MakeRecord(Role::BtcSeller);
    auto obs = At(kNow + kHour);
    obs.din_mtp_unix = r.offer.t_din_unix - 20 * kHour;
    obs.din = Locked(kDin, 40);
    const auto s = Step(r, obs);
    EXPECT_FALSE(Has(s, ActionKind::FundBtcHtlc));
}

TEST(SwapEngine, AliceJudgesHerClaimCutoffByBitcoinChainTimeToo) {
    // Alice's clock is behind: Bitcoin's median time is 3 h from T_btc, inside
    // her 6 h cut-off, so revealing the secret now risks losing the race.
    auto r = MakeRecord(Role::DinSeller);
    r.state = SwapState::DinLocked;
    auto obs = At(kNow + kHour);
    obs.btc_mtp_unix = r.offer.t_btc_unix - 3 * kHour;
    obs.din = Locked(kDin, 40);
    obs.btc = Locked(kBtc, 5);
    EXPECT_FALSE(Has(Step(r, obs), ActionKind::ClaimBtc));
}
