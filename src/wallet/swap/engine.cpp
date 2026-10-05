#include "wallet/swap/engine.h"

#include "crypto/sha256.h"

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {
namespace {

// Bob refuses to lock BTC when the DIN deadline is closer than this (design §6.1).
constexpr uint32_t kBobMinDinDeadlineAhead = 36 * 60 * 60;

bool PreimageMatches(const Bytes32& preimage, const Bytes32& payment_hash) {
    Bytes32 h{};
    crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(h.data());
    return h == payment_hash;
}

struct Stepper {
    SwapRecord rec;
    const Observations& obs;
    std::vector<Action> actions;

    void Go(SwapState state) {
        rec.state = state;
        rec.state_since_unix = obs.wall_clock_unix;
    }
    void Do(ActionKind kind, std::string reason) { actions.push_back({kind, std::move(reason)}); }
    void Alert(std::string reason) { Do(ActionKind::Alert, std::move(reason)); }
    bool FundingOverdue() const {
        return obs.wall_clock_unix > rec.state_since_unix + kFundingAlertAfterSeconds;
    }
    const SwapOffer& offer() const { return rec.offer; }
    // Settled = deep enough that no plausible reorg undoes it. Dinero outcomes
    // need at least the confirmations Bob required before risking his BTC.
    uint32_t DinSettle() const { return std::max(kSettleConfirmations, offer().n_din_confirmations); }
    uint32_t BtcSettle() const { return kSettleConfirmations; }
    bool AliceCutoffOk() const {
        // Both clocks: Bob's refund opens by Bitcoin's median time, so a wall
        // clock running behind the chain must not extend the cut-off.
        return uint64_t(obs.wall_clock_unix) + kAliceClaimCutoffSeconds < offer().t_btc_unix &&
               uint64_t(obs.btc_mtp_unix) + kAliceClaimCutoffSeconds < offer().t_btc_unix;
    }

    // ---- Alice: sells DIN, holds the secret --------------------------------

    void RefundDinWhenOpen() {
        if (obs.din.spent) return;
        if (obs.din_mtp_unix > offer().t_din_unix) {  // final only once T < median time past
            Do(ActionKind::RefundDin, "DIN refund is open");
            Go(SwapState::DinRefundBroadcast);
        }
    }

    void Alice() {
        switch (rec.state) {
        case SwapState::Accepted:
            if (obs.din.output_seen) return Go(SwapState::DinLocked);
            if (obs.wall_clock_unix > offer().expires_unix) return Go(SwapState::Aborted);
            Do(ActionKind::FundDinHtlc, "lock DIN for the swap");
            return Go(SwapState::DinLockBroadcast);

        case SwapState::DinLockBroadcast:
            if (obs.din.output_seen) return Go(SwapState::DinLocked);
            if (FundingOverdue()) Alert("DIN lock not seen on chain; not funding again — check the wallet");
            return;

        case SwapState::DinLocked:
            if (obs.din.spent && !obs.din.spent_by_claim && obs.din.spend_confirmations >= DinSettle()) {
                return Go(SwapState::Refunded);
            }
            // Never claim a BTC lock that is already spent (e.g. Bob refunded while
            // Alice was offline): it cannot succeed and would leak the secret.
            // Reveal only while her own DIN lock is intact and as deep as Bob
            // required: if a reorg removed it, Bob would learn the secret with
            // nothing to claim.
            if (obs.btc.output_seen && !obs.btc.spent &&
                obs.btc.output_confirmations >= offer().n_btc_confirmations && obs.din.output_seen &&
                !obs.din.spent && obs.din.output_value == offer().din_amount_una &&
                obs.din.output_confirmations >= offer().n_din_confirmations) {
                if (obs.btc.output_value != offer().btc_amount_sat) {
                    Alert("BTC lock has the wrong amount; not claiming");
                } else if (AliceCutoffOk()) {
                    Do(ActionKind::ClaimBtc, "BTC lock is deep enough");
                    return Go(SwapState::BtcClaimBroadcast);
                }
            }
            return RefundDinWhenOpen();

        case SwapState::BtcClaimBroadcast:
            if (obs.btc.spent && obs.btc.spent_by_claim) {
                rec.claim_seen = true;  // the secret is public from here on
                if (obs.btc.spend_confirmations >= std::max(offer().n_btc_confirmations, BtcSettle())) {
                    Go(SwapState::Done);
                } else if (obs.btc.spend_confirmations == 0) {
                    // Unconfirmed: re-broadcast so the runner can replace it with a
                    // higher fee before Bob's refund opens.
                    Do(ActionKind::ClaimBtc, "claim unconfirmed; fee bump");
                }
                return;
            }
            if (obs.btc.spent) {  // Bob's refund took the BTC: the secret is public
                Alert("lost the BTC race after revealing the secret; refunding DIN when it opens");
                if (obs.din.spent) {
                    if (obs.din.spent_by_claim) {
                        Alert("Bob claimed the DIN with the revealed secret");
                        if (obs.din.spend_confirmations >= DinSettle()) return Go(SwapState::Lost);
                        return;
                    }
                    if (obs.din.spend_confirmations >= DinSettle()) return Go(SwapState::Refunded);
                    return;
                }
                return RefundDinWhenOpen();
            }
            if (!rec.claim_seen && !AliceCutoffOk()) {
                // The claim never reached the network and the cut-off has passed:
                // revealing now would race Bob's refund. Give up the BTC claim;
                // the DIN refund path stays.
                Alert("BTC claim never seen before the cut-off; not revealing the secret now");
                return Go(SwapState::DinLocked);
            }
            Do(ActionKind::ClaimBtc, "claim not seen; re-broadcasting");
            if (obs.wall_clock_unix >= offer().t_btc_unix) {
                Alert("BTC claim unconfirmed and Bob's refund is open — racing");
            }
            return;

        case SwapState::DinRefundBroadcast:
            if (obs.din.spent) {
                if (obs.din.spent_by_claim) {
                    // Final only once buried: a reorg could still undo it.
                    Alert("Bob claimed the DIN before the refund confirmed");
                    if (obs.din.spend_confirmations >= DinSettle()) return Go(SwapState::Lost);
                    return;
                }
                if (obs.din.spend_confirmations >= DinSettle()) Go(SwapState::Refunded);
                return;
            }
            return Do(ActionKind::RefundDin, "refund not seen; re-broadcasting");

        default:
            return;  // terminal
        }
    }

    // ---- Bob: sells BTC, learns the secret from Alice's claim --------------

    bool LearnSecret() {
        if (!obs.btc.spent || !obs.btc.spent_by_claim) return false;
        if (!obs.btc.revealed_preimage ||
            !PreimageMatches(*obs.btc.revealed_preimage, offer().payment_hash)) {
            Alert("BTC claim seen without a matching secret; not claiming DIN");
            return false;
        }
        rec.secret = obs.btc.revealed_preimage;
        Do(ActionKind::ClaimDin, "secret revealed by Alice's BTC claim");
        Go(SwapState::DinClaimBroadcast);
        return true;
    }

    void Bob() {
        switch (rec.state) {
        case SwapState::Accepted: {
            if (obs.btc.output_seen) {
                Do(ActionKind::ArmTower, "BTC lock on chain");
                return Go(SwapState::BtcLocked);
            }
            // Expiry matters only while Alice has not locked: once her lock exists,
            // Bob waits for its confirmations (the checks below still apply).
            if (!obs.din.output_seen && obs.wall_clock_unix > offer().expires_unix) return Go(SwapState::Aborted);
            // Both clocks: Alice's refund opens by Dinero's median time, so a wall
            // clock running behind the chain must not hide a close deadline.
            if (uint64_t(obs.wall_clock_unix) + kBobMinDinDeadlineAhead > offer().t_din_unix ||
                uint64_t(obs.din_mtp_unix) + kBobMinDinDeadlineAhead > offer().t_din_unix) {
                Alert("DIN deadline is too close; not locking BTC");
                return Go(SwapState::Aborted);
            }
            if (obs.din.spent) {  // locking BTC now would only pay Alice
                Alert("DIN lock already spent; not locking BTC");
                return Go(SwapState::Aborted);
            }
            if (!obs.din.output_seen || obs.din.output_confirmations < offer().n_din_confirmations) return;
            if (obs.din.output_value != offer().din_amount_una) {
                return Alert("DIN lock has the wrong amount; not locking BTC");
            }
            const bool din_stalled = obs.wall_clock_unix > obs.din_mtp_unix + kMaxMtpLagSeconds;
            const bool btc_stalled = obs.wall_clock_unix > obs.btc_mtp_unix + kMaxMtpLagSeconds;
            if (din_stalled || btc_stalled) {
                return Alert("a chain looks stalled (median time lags); waiting before locking BTC");
            }
            Do(ActionKind::FundBtcHtlc, "DIN lock is deep enough");
            return Go(SwapState::BtcLockBroadcast);
        }

        case SwapState::BtcLockBroadcast:
            if (obs.btc.output_seen) {
                Do(ActionKind::ArmTower, "BTC lock on chain");
                return Go(SwapState::BtcLocked);
            }
            if (FundingOverdue()) Alert("BTC lock not seen on chain; not funding again — check the wallet");
            return;

        case SwapState::BtcLocked:
            if (LearnSecret()) return;
            if (obs.btc.spent && !obs.btc.spent_by_claim) {
                if (obs.btc.spend_confirmations >= BtcSettle()) Go(SwapState::Refunded);
                return;
            }
            if (!obs.btc.spent && obs.btc_mtp_unix > offer().t_btc_unix) {  // final once T < median time past
                Do(ActionKind::RefundBtc, "BTC refund is open and the secret never appeared");
                Go(SwapState::BtcRefundBroadcast);
            }
            return;

        case SwapState::BtcRefundBroadcast:
            if (LearnSecret()) return;  // Alice won the race: take the DIN now
            if (obs.btc.spent && !obs.btc.spent_by_claim) {
                if (obs.btc.spend_confirmations >= BtcSettle()) Go(SwapState::Refunded);
                else if (obs.btc.spend_confirmations == 0) Do(ActionKind::RefundBtc, "refund unconfirmed; fee bump");
                return;
            }
            if (!obs.btc.spent) Do(ActionKind::RefundBtc, "refund not seen; re-broadcasting");
            return;

        case SwapState::DinClaimBroadcast:
            if (obs.din.spent) {
                if (!obs.din.spent_by_claim) {
                    // Final only once buried: if a reorg drops the refund, claim again.
                    Alert("Alice's DIN refund beat the claim");
                    if (obs.din.spend_confirmations >= DinSettle()) return Go(SwapState::Lost);
                    return;
                }
                if (obs.din.spend_confirmations >= DinSettle()) Go(SwapState::Done);
                return;
            }
            return Do(ActionKind::ClaimDin, "claim not seen; re-broadcasting");

        default:
            return;  // terminal
        }
    }
};

// ---- Persistence --------------------------------------------------------

constexpr char kRecordPrefix[] = "dinswap1r";
constexpr uint8_t kRecordVersion = 1;
constexpr size_t kChecksumSize = 4;

[[noreturn]] void Refuse(const std::string& why) {
    throw std::invalid_argument("swap record refused: " + why);
}

void PutLE(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutString(std::vector<uint8_t>& out, const std::string& s) {
    PutLE(out, s.size(), 2);
    out.insert(out.end(), s.begin(), s.end());
}

}  // namespace

StepResult Step(const SwapRecord& record, const Observations& now) {
    Stepper s{record, now, {}};
    if (record.role == Role::DinSeller) {
        s.Alice();
    } else {
        s.Bob();
    }
    return {std::move(s.rec), std::move(s.actions)};
}

std::string EncodeRecord(const SwapRecord& r) {
    std::vector<uint8_t> p;
    p.push_back(kRecordVersion);
    p.push_back(static_cast<uint8_t>(r.role));
    p.push_back(static_cast<uint8_t>(r.state));
    PutLE(p, r.state_since_unix, 4);
    p.push_back(static_cast<uint8_t>((r.secret ? 1 : 0) | (r.claim_seen ? 2 : 0)));  // flags
    const Bytes32 secret = r.secret.value_or(Bytes32{});
    p.insert(p.end(), secret.begin(), secret.end());
    PutString(p, EncodeOffer(r.offer));
    PutString(p, EncodeAccept(r.accept));

    Bytes32 sum{};
    crypto::CSHA256().Write(p.data(), p.size()).Finalize(sum.data());
    p.insert(p.end(), sum.begin(), sum.begin() + kChecksumSize);
    static const char* digits = "0123456789abcdef";
    std::string text = kRecordPrefix;
    for (auto b : p) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}

SwapRecord DecodeRecord(const std::string& text) {
    const std::string prefix(kRecordPrefix);
    if (text.compare(0, prefix.size(), prefix) != 0) Refuse("wrong prefix");
    const std::string hex = text.substr(prefix.size());
    if (hex.size() % 2 != 0 || hex.size() < 2 * (1 + 1 + 1 + 4 + 1 + 32 + 4 + kChecksumSize)) {
        Refuse("wrong length");
    }
    std::vector<uint8_t> b;
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) Refuse("not lowercase hex");
        b.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    std::vector<uint8_t> p(b.begin(), b.end() - kChecksumSize);
    Bytes32 sum{};
    crypto::CSHA256().Write(p.data(), p.size()).Finalize(sum.data());
    if (!std::equal(sum.begin(), sum.begin() + kChecksumSize, b.end() - kChecksumSize)) {
        Refuse("checksum mismatch");
    }

    size_t pos = 0;
    auto take = [&](size_t n) {
        if (pos + n > p.size()) Refuse("truncated");
        const size_t at = pos;
        pos += n;
        return at;
    };
    auto le = [&](int n) {
        const size_t at = take(n);
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(p[at + i]) << (8 * i);
        return v;
    };
    auto str = [&] {
        const size_t len = le(2);
        const size_t at = take(len);
        return std::string(p.begin() + at, p.begin() + at + len);
    };

    if (le(1) != kRecordVersion) Refuse("unsupported version");
    SwapRecord r;
    const auto role = le(1);
    if (role > 1) Refuse("unknown role");
    r.role = static_cast<Role>(role);
    const auto state = le(1);
    if (state > static_cast<uint8_t>(SwapState::Lost)) Refuse("unknown state");
    r.state = static_cast<SwapState>(state);
    r.state_since_unix = static_cast<uint32_t>(le(4));
    const auto flags = le(1);  // bit 0: secret present, bit 1: claim seen
    if (flags > 3) Refuse("bad flags");
    const bool has_secret = flags & 1;
    r.claim_seen = (flags & 2) != 0;
    Bytes32 secret{};
    const size_t at = take(32);
    std::copy(p.begin() + at, p.begin() + at + 32, secret.begin());
    if (has_secret) r.secret = secret;
    r.offer = DecodeOffer(str());
    r.accept = DecodeAccept(str());
    if (pos != p.size()) Refuse("trailing bytes");
    if (r.accept.offer_id != OfferId(r.offer)) Refuse("accept does not match the offer");
    if (r.secret && !PreimageMatches(*r.secret, r.offer.payment_hash)) Refuse("secret does not match");
    return r;
}

const char* StateName(SwapState state) {
    switch (state) {
    case SwapState::Accepted: return "accepted";
    case SwapState::DinLockBroadcast: return "din-lock-broadcast";
    case SwapState::DinLocked: return "din-locked";
    case SwapState::BtcLockBroadcast: return "btc-lock-broadcast";
    case SwapState::BtcLocked: return "btc-locked";
    case SwapState::BtcClaimBroadcast: return "btc-claim-broadcast";
    case SwapState::DinClaimBroadcast: return "din-claim-broadcast";
    case SwapState::DinRefundBroadcast: return "din-refund-broadcast";
    case SwapState::BtcRefundBroadcast: return "btc-refund-broadcast";
    case SwapState::Done: return "done";
    case SwapState::Refunded: return "refunded";
    case SwapState::Aborted: return "aborted";
    case SwapState::Lost: return "lost";
    }
    return "unknown";
}

}  // namespace dinero::swap
