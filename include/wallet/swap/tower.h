#pragma once
// Swap watchtower (milestone 5 of docs/design/din-btc-atomic-swaps-v1-plan.md,
// design §6.2): keeps Bob safe while his wallet is offline.
//
// Bob hands the tower a TowerPackage: the swap terms plus PRE-SIGNED
// transactions only — no private key, no secret.
//   - DIN claim rungs: signed claims of the DIN HTLC at rising fees. A tapscript
//     signature does not cover the witness, so the tower inserts Alice's secret
//     the moment it appears in her Bitcoin claim, and broadcasts.
//   - BTC refund rungs: fully signed refunds; valid once Bitcoin's median time
//     reaches T_btc.
// Every rung pays Bob's own payout script; the tower can choose a rung, never
// where the money goes. A package is verified when loaded (outpoints, payout,
// every signature against Bob's keys in the accept) so a bad package is refused
// instead of silently leaving Bob unprotected.
//
// Fees. Bitcoin replaces by fee: each BTC refund rung is re-broadcast while
// unconfirmed and the next rung is tried after `escalate_after_seconds`.
// Dinero nodes do NOT replace by fee by default (mempool.enable_rbf=false;
// verified on regtest), so the first accepted DIN claim is final: the DIN rung
// is chosen by urgency alone — rung 0 while T_din is at least
// `din_relaxed_before_seconds` away (by Dinero's median time), the top rung
// within `din_urgent_before_seconds`, linear in between — never by elapsed time.
//
// The tower keeps no state that matters: it re-derives everything from the
// chains each tick (a restart only resets the escalation step).

#include "wallet/swap/btc_tx.h"
#include "wallet/swap/fee_ladder.h"
#include "wallet/swap/runner.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

// A CPFP child of one claim rung: spends the rung's sweep output (the rung's
// txid ignores its witness, so it can be signed before the secret exists) and
// pays Bob's wallet. Fully signed.
struct DinChildRung {
    Transaction tx;
    uint64_t fee_una{};
};

struct DinClaimRung {
    Transaction tx;                     // unsigned witness; SetDinClaimWitness completes it
    std::array<uint8_t, 64> signature{};  // Bob's BIP340 signature (SIGHASH_DEFAULT)
    uint64_t fee_una{};
    std::vector<DinChildRung> children;   // ascending fee (empty without a sweep key)
};

struct BtcRefundRung {
    BtcTx tx;  // fully signed
    uint64_t fee_sat{};
};

// A Zcash refund rung (docs/design/din-zec-atomic-swaps.md): one, at 4 x the
// conventional fee, nExpiryHeight 0, nLockTime T_zec. The package carries its
// parts; the Zcash module rebuilds the exact transaction to verify and send
// it, so this file needs no Zcash parser.
struct ZecRefundRung {
    uint64_t fee_zat{};
    uint32_t branch_id{};                // the consensus branch it was signed for
    std::vector<uint8_t> payout_script;  // Bob's
    std::vector<uint8_t> signature;      // DER + SIGHASH_ALL, Bob's ZEC refund key
};

struct TowerPackage {
    SwapOffer offer;
    SwapAccept accept;
    uint32_t btc_scan_from_height{};
    uint32_t din_scan_from_height{};
    Bytes32 din_sweep_pubkey{};                // zero: claims pay Bob's wallet directly (no CPFP)
    std::vector<DinClaimRung> din_claims;    // ascending fee
    std::vector<BtcRefundRung> btc_refunds;  // ascending fee (Bitcoin swaps)
    std::optional<BtcFunding> zec_funding;   // Zcash swaps: the ZEC lock (zatoshi)
    std::vector<ZecRefundRung> zec_refunds;  // Zcash swaps
};

// What the Zcash module provides to the tower (installed by ZecSwapChainIo):
// verify the Zcash rungs (throws std::invalid_argument) and serialize rung i.
// Without them a Zcash package is refused.
struct ZecTowerHooks {
    std::function<void(const TowerPackage&)> verify;
    std::function<std::vector<uint8_t>(const TowerPackage&, size_t)> rung_raw;
};
void InstallZecTowerHooks(ZecTowerHooks hooks);

// The outpoints the package's rungs spend are the locks the tower watches,
// exactly (payments to the same scripts are ignored).
SwapWatchSpec TowerWatchSpec(const TowerPackage& package, const std::string& btc_chain);

// "dinswap1t" text form, one "key=value" per line. Decode throws std::invalid_argument.
std::string EncodeTowerPackage(const TowerPackage& package);
TowerPackage DecodeTowerPackage(const std::string& text);

struct BtcFeeLadderPolicy {
    uint64_t start_feerate_sat_per_vb{2};
    uint32_t max_rungs{8};     // each next rung doubles the feerate
    uint64_t max_fee_sat{};    // absolute cap per rung
    uint64_t min_payout_sat{};
};

// Bob signs both ladders. Throws std::invalid_argument unless `s` is Bob's
// session, the keys are his, and at least one rung of each fits the policy.
TowerPackage BuildTowerPackage(const SwapSession& s, const SwapKeys& keys, const FundingOutput& din_funding,
                               const BtcFunding& btc_funding, const FeeLadderPolicy& din_policy,
                               const BtcFeeLadderPolicy& btc_policy);
// The two halves of BuildTowerPackage (neither verifies the package): the
// terms and DIN claim rungs, then the Bitcoin refund rungs.
TowerPackage BuildTowerPackageDin(const SwapSession& s, const SwapKeys& keys, const FundingOutput& din_funding,
                                  const FeeLadderPolicy& din_policy);
void AddBtcRefundRungs(TowerPackage& p, const SwapSession& s, const SwapKeys& keys, const BtcFunding& btc_funding,
                       const BtcFeeLadderPolicy& btc_policy);

// Throws std::invalid_argument naming the first problem: wrong outpoint, a rung
// paying anywhere but Bob's payout, fees not strictly rising, a refund whose
// locktime is not T_btc, or any signature that does not verify.
void VerifyTowerPackage(const TowerPackage& package);

// Arming channel: the package is written atomically (temp + fsync + rename) as
// <inbox>/<offer id prefix>.pkg. The inbox should be a directory only Bob's
// user can write (0700); the tower verifies every package anyway.
std::string WriteTowerInbox(const std::string& inbox_dir, const std::string& package_text);  // returns path

// The tower's side of arming: while a package's tower is healthy (both chains
// observed, the package's DIN lock on chain) the daemon refreshes
// <inbox>/<id>.armed with that package's hash and the time. Bob funds only on
// a fresh ack for exactly the package he delivered.
std::string TowerPackageHash(const std::string& package_text);  // SHA-256 hex
void MarkTowerArmed(const std::string& inbox_dir, const std::string& swap_id, const std::string& package_hash,
                    uint32_t now_unix);
bool TowerAckFresh(const std::string& inbox_dir, const std::string& swap_id, const std::string& package_hash,
                   uint32_t now_unix, uint32_t max_age_seconds = 120);

// ---- A tower that anyone may arm over the network ---------------------------
// (tools/dinero_swap_tower_intake.cpp; design: section 14 of
// docs/design/din-btc-swaps-without-bitcoin-core.md.) The intake hands the
// tower's own inbox packages from strangers, so:
//   - a package is fully verified before it is kept;
//   - it is kept as <inbox>/<hash>.pkg, its SHA-256: nobody can replace or
//     block another person's package, whatever swap it names;
//   - size and number are capped.
struct TowerIntakeLimits {
    size_t max_package_bytes{256 * 1024};
    size_t max_packages{20000};            // .pkg files in the inbox
    std::optional<SwapNetwork> network;    // packages of another network are refused
};
enum class TowerIntakeStatus {
    Stored,       // new, verified, on disk
    AlreadyHeld,  // the same package again: nothing written
    TooLarge,
    Invalid,      // `reason` says why
    Full,         // the inbox holds max_packages
    Failed,       // could not write
};
struct TowerIntakeResult {
    TowerIntakeStatus status{TowerIntakeStatus::Failed};
    std::string hash;    // of the text, for Stored and AlreadyHeld
    std::string reason;  // for Invalid and Failed
};
TowerIntakeResult TowerIntakeStore(const std::string& inbox_dir, const std::string& package_text,
                                   const TowerIntakeLimits& limits);
// 64 lower-case hex digits: the only thing the intake ever puts in a file name.
bool IsTowerPackageHash(const std::string& text);
// Seconds since the tower last confirmed it is guarding package `hash`, if
// that is recent enough for the buyer to rely on (TowerAckFresh's rule).
std::optional<uint32_t> TowerIntakeArmedAge(const std::string& inbox_dir, const std::string& hash, uint32_t now_unix,
                                            uint32_t max_age_seconds = 120);

struct TowerConfig {
    uint32_t escalate_after_seconds{30 * 60};
    uint32_t din_relaxed_before_seconds{24 * 60 * 60};  // lowest DIN rung this far from T_din
    uint32_t din_urgent_before_seconds{6 * 60 * 60};    // top DIN rung this close to T_din
    uint32_t settle_confirmations{6};  // keep watching (and re-broadcast after a reorg) until this deep
    // The one CPFP bump (Dinero cannot bump twice): the claim unmined at least
    // din_bump_after_seconds, and within din_bump_window_seconds of T_din or
    // unmined for din_bump_stuck_seconds; then the largest child.
    uint32_t din_bump_after_seconds{20 * 60};
    uint32_t din_bump_window_seconds{12 * 60 * 60};
    uint32_t din_bump_stuck_seconds{6 * 60 * 60};
};

// Index into `count` fee levels by how close T_din is (Dinero has no RBF:
// a level is chosen once). Shared by the tower and Bob's runner so both send
// the same claim.
size_t IndexByUrgency(size_t count, uint32_t t_din, uint32_t din_mtp, const TowerConfig& config);

struct TowerReport {
    bool observed{false};
    bool healthy{false};                    // both chains observed and the package's DIN lock on chain
    bool counter_lock_seen{false};          // the buyer's BTC (or ZEC) lock is on its chain
    std::optional<Bytes32> learned_secret;  // set once, when first learned (the daemon keeps it)
    bool finished{false};  // Bob's outcome is settled on chain (claimed DIN or refunded BTC)
    std::vector<std::string> events;
};

class Watchtower {
public:
    // Verifies the package (throws std::invalid_argument).
    Watchtower(TowerPackage package, TowerConfig config, SwapChainIo& io);
    TowerReport Tick(uint32_t wall_clock_unix);
    // A secret learned before a restart (kept by the daemon): lets the DIN
    // duty continue while Bitcoin is unreachable. Throws if it does not match.
    void SetKnownSecret(const Bytes32& secret);

private:
    struct Escalation {
        size_t rung{0};
        uint32_t since{0};  // wall clock of the first broadcast at this rung
        bool started{false};
    };
    size_t NextRung(Escalation& e, size_t rungs, uint32_t now) const;
    size_t DinRungByUrgency(uint32_t din_mtp) const;

    TowerPackage package_;
    TowerConfig config_;
    SwapChainIo& io_;
    Escalation btc_refund_;
    std::optional<uint32_t> duty_since_;  // first tick of the DIN claim duty in this process
    std::optional<Bytes32> secret_;
    void DinClaimDuty(const DinWatchReport& din, uint32_t now, TowerReport& report);
    void BtcRefundDuty(const BtcWatchReport& btc, uint32_t now, TowerReport& report);
};

// Nothing left to guard: both chains were observed, the buyer never locked
// his coins, and the DIN deadline passed `grace_seconds` ago (he cannot lock
// after it). A public tower drops such packages; they would pile up for ever.
bool TowerPackageAbandoned(const TowerPackage& package, const TowerReport& report, uint32_t now_unix,
                           uint32_t grace_seconds);

// One daemon cycle for one armed swap: Tick, then refresh the ack (while
// healthy) and keep a newly learned secret in the inbox. Never throws: a
// failed tick or write is an ALERT event, because the daemon ticks every swap
// in one loop and one swap's disk or chain trouble must not stop the others.
// Without a fresh ack Bob does not fund, so a failed ack write is safe.
TowerReport TickAndRecord(Watchtower& tower, const std::string& inbox_dir, const std::string& swap_id,
                          const std::string& package_hash, uint32_t now_unix);

}  // namespace dinero::swap
