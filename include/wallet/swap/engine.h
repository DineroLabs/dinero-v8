#pragma once
// Swap engine (milestone 3 of docs/design/din-btc-atomic-swaps-v1-plan.md).
//
// A PURE decision function: given the persisted SwapRecord and what is
// currently observed on both chains, Step() returns the next record and the
// actions to perform. It never trusts its own memory over the chains — every
// tick re-derives from observations — so a crash or restart cannot confuse it.
//
// Executor contract (wallet side, not in this file):
//   1. persist StepResult.record BEFORE performing any action;
//   2. then perform the actions (all are idempotent re-broadcasts except the
//      two funding actions, which Step() emits only once, from a single state).
// If a funding transaction never appears on chain, Step() raises an alert
// instead of funding again — double funding is impossible by construction.
//
// Safety rules implemented here (design §6.1):
//   Alice (DIN seller): claims BTC only before T_btc - 6 h by BOTH her wall
//   clock and Bitcoin's MTP, and never once the BTC lock is spent; after a
//   lost race or the cut-off, refunds DIN once Dinero MTP reaches T_din.
//   Bob (BTC seller): locks BTC only after N_din confirmations of the correct,
//   unspent DIN lock and only if the DIN deadline is >= 36 h away by BOTH his
//   wall clock and Dinero's MTP, the offer has not expired and neither chain's
//   MTP lags wall clock by more than 2 h; claims
//   DIN the moment the preimage appears (mempool or block); refunds BTC once
//   Bitcoin MTP reaches T_btc and the preimage is still unseen.

#include "wallet/swap/offer.h"

#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

enum class Role : uint8_t { DinSeller = 0, BtcSeller = 1 };  // Alice, Bob

enum class SwapState : uint8_t {
    Accepted = 0,          // offer + accept matched, nothing on chain yet
    DinLockBroadcast = 1,  // Alice asked her wallet to fund the DIN HTLC
    DinLocked = 2,         // DIN HTLC observed
    BtcLockBroadcast = 3,  // Bob asked his wallet to fund the BTC HTLC
    BtcLocked = 4,         // BTC HTLC observed (Bob)
    BtcClaimBroadcast = 5, // Alice broadcast her BTC claim (secret revealed)
    DinClaimBroadcast = 6, // Bob broadcast his DIN claim
    DinRefundBroadcast = 7,
    BtcRefundBroadcast = 8,
    Done = 9,              // swap completed for this party
    Refunded = 10,         // this party got its own coins back
    Aborted = 11,          // nothing was locked by this party
    Lost = 12,             // counterparty took both sides; needs attention
};

struct SwapRecord {
    Role role{Role::DinSeller};
    SwapOffer offer;
    SwapAccept accept;
    SwapState state{SwapState::Accepted};
    std::optional<Bytes32> secret;  // Alice: her secret. Bob: learned from Alice's BTC claim.
    uint32_t state_since_unix{};    // wall clock when the current state began
    bool claim_seen{false};         // Alice: her BTC claim was observed (the secret is public)
};

// What a chain watcher reports for one HTLC.
struct HtlcObservation {
    bool output_seen{false};
    uint32_t output_confirmations{0};
    uint64_t output_value{0};        // una (DIN) or sat (BTC)
    bool spent{false};               // spend seen in mempool or a block
    bool spent_by_claim{false};      // claim path (else refund path)
    uint32_t spend_confirmations{0}; // 0 = mempool only
    std::optional<Bytes32> revealed_preimage;  // from a claim witness
};

struct Observations {
    uint32_t wall_clock_unix{};
    uint32_t din_mtp_unix{};
    uint32_t btc_mtp_unix{};
    HtlcObservation din;
    HtlcObservation btc;
};

enum class ActionKind : uint8_t {
    FundDinHtlc,  // Alice's wallet funds the DIN HTLC (emitted once)
    FundBtcHtlc,  // Bob's wallet funds the BTC HTLC (emitted once)
    ArmTower,     // Bob hands pre-signed ladders to the watchtower
    ClaimBtc,     // Alice: broadcast/re-broadcast BTC claim (reveals secret)
    ClaimDin,     // Bob: broadcast/re-broadcast DIN claim with the secret
    RefundDin,    // Alice
    RefundBtc,    // Bob
    Alert,        // tell the user; never moves funds
};

struct Action {
    ActionKind kind;
    std::string reason;
};

struct StepResult {
    SwapRecord record;
    std::vector<Action> actions;
};

// Alice's claim cut-off and Bob's abort thresholds (design §6.1).
inline constexpr uint32_t kAliceClaimCutoffSeconds = 6 * 60 * 60;
inline constexpr uint32_t kMaxMtpLagSeconds = 2 * 60 * 60;
inline constexpr uint32_t kFundingAlertAfterSeconds = 2 * 60 * 60;
// Bob never commits BTC when Alice's DIN refund opens sooner than this.
inline constexpr uint32_t kBobMinDinDeadlineAheadSeconds = 36 * 60 * 60;
// A party is Done/Refunded only once its outcome is this deep; until then it
// keeps watching and re-broadcasts if a reorg drops the transaction.
inline constexpr uint32_t kSettleConfirmations = 6;

StepResult Step(const SwapRecord& record, const Observations& now);

// Persisted form: "dinswap1r" + hex(payload || 4-byte SHA-256 checksum).
// Holds the secret in the clear: the wallet store that saves it must encrypt it.
std::string EncodeRecord(const SwapRecord& record);
SwapRecord DecodeRecord(const std::string& text);  // throws std::invalid_argument

const char* StateName(SwapState state);

}  // namespace dinero::swap
