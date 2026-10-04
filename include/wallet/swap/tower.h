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
// Fees: each rung is re-broadcast while unconfirmed and the next rung is tried
// after `escalate_after_seconds`; when T_din is close the DIN claim jumps to the
// top rung. Bitcoin replaces by fee. Dinero nodes do NOT by default
// (mempool.enable_rbf=false): there the first accepted rung stays, so DIN
// rungs start at a fee meant to confirm without replacement.
//
// The tower keeps no state that matters: it re-derives everything from the
// chains each tick (a restart only resets the escalation step).

#include "wallet/swap/btc_tx.h"
#include "wallet/swap/fee_ladder.h"
#include "wallet/swap/runner.h"

#include <string>
#include <vector>

namespace dinero::swap {

struct DinClaimRung {
    Transaction tx;                     // unsigned witness; SetDinClaimWitness completes it
    std::array<uint8_t, 64> signature{};  // Bob's BIP340 signature (SIGHASH_DEFAULT)
    uint64_t fee_una{};
};

struct BtcRefundRung {
    BtcTx tx;  // fully signed
    uint64_t fee_sat{};
};

struct TowerPackage {
    SwapOffer offer;
    SwapAccept accept;
    uint32_t btc_scan_from_height{};
    std::vector<DinClaimRung> din_claims;    // ascending fee
    std::vector<BtcRefundRung> btc_refunds;  // ascending fee
};

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

// Throws std::invalid_argument naming the first problem: wrong outpoint, a rung
// paying anywhere but Bob's payout, fees not strictly rising, a refund whose
// locktime is not T_btc, or any signature that does not verify.
void VerifyTowerPackage(const TowerPackage& package);

// Arming channel: the package is written atomically (temp + fsync + rename) as
// <inbox>/<offer id prefix>.pkg. The inbox should be a directory only Bob's
// user can write (0700); the tower verifies every package anyway.
std::string WriteTowerInbox(const std::string& inbox_dir, const std::string& package_text);  // returns path

struct TowerConfig {
    uint32_t escalate_after_seconds{30 * 60};
    uint32_t din_urgent_before_seconds{6 * 60 * 60};  // top DIN rung this close to T_din
};

struct TowerReport {
    bool observed{false};
    bool finished{false};  // Bob's outcome is settled on chain (claimed DIN or refunded BTC)
    std::vector<std::string> events;
};

class Watchtower {
public:
    // Verifies the package (throws std::invalid_argument).
    Watchtower(TowerPackage package, TowerConfig config, SwapChainIo& io);
    TowerReport Tick(uint32_t wall_clock_unix);

private:
    struct Escalation {
        size_t rung{0};
        uint32_t since{0};  // wall clock of the first broadcast at this rung
        bool started{false};
    };
    size_t NextRung(Escalation& e, size_t rungs, uint32_t now, bool urgent) const;

    TowerPackage package_;
    TowerConfig config_;
    SwapChainIo& io_;
    Escalation din_claim_;
    Escalation btc_refund_;
};

}  // namespace dinero::swap
