#pragma once
// Swap runner (milestone 5 of docs/design/din-btc-atomic-swaps-v1-plan.md):
// the executor around the pure engine. Each Tick() observes both chains,
// asks Step() what to do, persists the result, and only then performs the
// actions (fund, claim, refund) through the wallets and nodes.
//
// Executor contract (engine.h):
//   - no observation, no decision: if either chain is unreachable or
//     inconsistent, the tick does nothing at all;
//   - the new record is saved BEFORE any action runs; if saving fails, no
//     action runs and the in-memory session is unchanged;
//   - a failed action never rolls the state back. The engine re-broadcasts
//     claims and refunds on later ticks and raises an alert for a funding
//     transaction that never appears; it never funds twice.

#include "wallet/swap/btc_watcher.h"
#include "wallet/swap/din_watcher.h"
#include "wallet/swap/engine.h"

#include <memory>
#include <string>
#include <vector>

namespace dinero::swap {

struct TowerPackage;  // tower.h

// Everything one party needs to resume a swap after a restart.
struct SwapSession {
    SwapRecord record;
    uint32_t btc_scan_from_height{};          // BTC tip when the swap was accepted
    uint32_t din_scan_from_height{};          // DIN tip when the swap was accepted
    // The lock transactions, pinned once known: Alice's own DIN funding (from
    // her wallet), Alice's DIN lock as Bob saw it when he funded BTC, and Bob's
    // own BTC funding. Payments to the same scripts never stand in for them.
    std::string din_funding_txid;
    std::string btc_funding_txid;
    // Bob with a tower: his funding, built and signed but NOT yet broadcast
    // (sent once the tower confirms it holds the package). Empty once sent.
    // Bob (CPFP): his DIN claim pays a sweep output of this key; a child moves
    // it to din_payout_script (zero = legacy, the claim pays the wallet).
    Bytes32 din_sweep_pubkey{};
    bool din_swept{false};
    std::string din_claim_txid;    // Bob's first DIN claim (Dinero keeps the first seen)
    uint64_t din_claim_value{0};   // its output value
    std::string btc_funding_raw;
    std::vector<uint8_t> din_payout_script;   // Alice: refund goes here; Bob: claim goes here
    std::vector<uint8_t> btc_payout_script;   // Alice: claim goes here; Bob: refund goes here
    bool tower_armed{false};                  // Bob: the watchtower accepted this swap's package
    std::string tower_package_hash;           // hash of the package delivered (the ack must match it)
    int32_t btc_funding_vout{-1};             // the BTC lock output the package was built for (-1 unknown)
};

// Text form, one "key=value" per line. Holds the secret in the clear (via
// EncodeRecord). Decode throws std::invalid_argument.
std::string EncodeSession(const SwapSession& session);
SwapSession DecodeSession(const std::string& text);

class SwapStore {
public:
    virtual ~SwapStore() = default;
    // Durable when it returns; throws on failure.
    virtual void Save(const SwapSession& session) = 0;
};

// DEVELOPMENT / REGTEST ONLY: writes the session, secret included, unencrypted.
// Write-to-temp + fsync + rename, so a crash leaves the old or the new file.
class PlaintextFileSwapStore : public SwapStore {
public:
    explicit PlaintextFileSwapStore(std::string path);
    void Save(const SwapSession& session) override;
    static SwapSession Load(const std::string& path);  // throws

private:
    std::string path_;
};

// This party's two private keys (32-byte scalars). Alice: DIN refund key and
// BTC claim key. Bob: DIN claim key and BTC refund key.
struct SwapKeys {
    Bytes32 din_secret_key{};
    Bytes32 btc_secret_key{};
    Bytes32 din_sweep_secret_key{};  // Bob: spends his claim's sweep output (CPFP child)
};

struct RunnerConfig {
    uint64_t din_fee_una{100'000};          // DIN claim/refund fee with >= 24 h to T_din
    uint64_t din_fee_urgent_una{1'000'000}; // DIN claim fee within 6 h of T_din (linear between)
    uint64_t btc_fee_sat{1'000};          // first BTC claim/refund fee; doubles every 30 min unconfirmed
    uint32_t btc_fee_max_percent{5};      // cap per BTC claim/refund, of the swap amount
    // Bob's one CPFP bump (Dinero cannot bump twice): only for a claim unmined
    // at least din_bump_after_seconds, and only once it matters — within
    // din_bump_window_seconds of T_din, or unmined for din_bump_stuck_seconds.
    uint32_t din_bump_after_seconds{20 * 60};
    uint32_t din_bump_window_seconds{12 * 60 * 60};
    uint32_t din_bump_stuck_seconds{6 * 60 * 60};
    // Bob's watchtower ladders (only used when a tower is configured).
    bool use_tower{false};
    bool require_tower{false};  // mainnet beta: Bob holds in Accepted (never funds) without a tower
    uint64_t din_tower_start_feerate_una_per_vb{1'000};
    uint64_t btc_tower_start_feerate_sat_per_vb{2};
    uint32_t tower_rungs{8};
    uint32_t tower_max_fee_percent{5};  // per rung, of the swap amount
    std::string din_hrp{"din"};   // "din" / "tdin" / "rdin"
    std::string btc_hrp{"bc"};    // "bc" / "tb" / "bcrt"
    std::string btc_chain{"main"};  // Bitcoin Core getblockchaininfo "chain" this swap must run on
};

struct PreparedBtcFunding {
    std::vector<uint8_t> raw;  // signed, not broadcast
    std::string txid;          // display hex
    uint32_t vout{};
};

// The chains as the runner sees them. Write methods return the txid and throw
// std::runtime_error when the node or wallet refuses.
class SwapChainIo {
public:
    virtual ~SwapChainIo() = default;
    virtual DinWatchReport ObserveDin() = 0;
    virtual BtcWatchReport ObserveBtc() = 0;
    virtual std::string FundDin(const std::string& address, uint64_t amount_una) = 0;
    virtual std::string FundBtc(const std::string& address, uint64_t amount_sat) = 0;
    virtual std::string BroadcastDin(const std::vector<uint8_t>& raw_tx) = 0;
    virtual std::string BroadcastBtc(const std::vector<uint8_t>& raw_tx) = 0;
    // Build and sign Bob's BTC funding without broadcasting it (so the tower
    // can hold a package for its exact outpoint first). Throws if unsupported.
    virtual PreparedBtcFunding PrepareFundBtc(const std::string& address, uint64_t amount_sat);
    // Give a never-sent prepared funding's inputs back to the wallet.
    virtual void ReleasePreparedFunding(const std::vector<uint8_t>& raw) { (void)raw; }
    // Whether a Dinero output is still unspent (nullopt: unknown/unreachable).
    virtual std::optional<bool> DinOutputUnspent(const TxId& txid, uint32_t vout) {
        (void)txid;
        (void)vout;
        return std::nullopt;
    }
    // True while Bob's watchtower confirms it holds exactly this package and is healthy.
    virtual bool TowerAcknowledged(const std::string& swap_id, const std::string& package_hash) {
        (void)swap_id;
        (void)package_hash;
        return false;
    }
    // Hand a TowerPackage (text form) to Bob's watchtower; throws if none is
    // configured or it refuses the package.
    virtual void ArmTower(const std::string& package_text);
    // Watch exactly these lock transactions from now on ("" = keep as is).
    virtual void PinFunding(const std::string& din_txid, const std::string& btc_txid) {
        (void)din_txid;
        (void)btc_txid;
    }
};

// What the chain watchers look for.
struct SwapWatchSpec {
    SwapOffer offer;
    SwapAccept accept;
    uint32_t din_scan_from_height{};
    uint32_t btc_scan_from_height{};
    std::string din_funding_txid;
    std::string btc_funding_txid;
    std::string btc_chain;
};
SwapWatchSpec WatchSpecFor(const SwapSession& session, const RunnerConfig& config);

// Node RPC implementation. `btc` must reach a wallet only for Bob (FundBtc).
// Dinero funding uses wallet.sendtoaddress {address, amount_una}.
class RpcSwapChainIo : public SwapChainIo {
public:
    RpcSwapChainIo(DinRpc din, BtcRpc btc, const SwapSession& session, const RunnerConfig& config);
    RpcSwapChainIo(DinRpc din, BtcRpc btc, SwapWatchSpec spec);
    void PinFunding(const std::string& din_txid, const std::string& btc_txid) override;
    // Optional: where ArmTower() delivers packages (e.g. writes the tower's inbox).
    void SetTowerSink(std::function<void(const std::string&)> sink) { tower_sink_ = std::move(sink); }
    // Optional: whether the tower confirmed a swap's package (see WriteTowerInbox).
    void SetTowerAck(std::function<bool(const std::string&, const std::string&)> ack) { tower_ack_ = std::move(ack); }
    void ArmTower(const std::string& package_text) override;
    PreparedBtcFunding PrepareFundBtc(const std::string& address, uint64_t amount_sat) override;
    void ReleasePreparedFunding(const std::vector<uint8_t>& raw) override;
    bool TowerAcknowledged(const std::string& swap_id, const std::string& package_hash) override;
    std::optional<bool> DinOutputUnspent(const TxId& txid, uint32_t vout) override;
    DinWatchReport ObserveDin() override;
    BtcWatchReport ObserveBtc() override;
    std::string FundDin(const std::string& address, uint64_t amount_una) override;
    std::string FundBtc(const std::string& address, uint64_t amount_sat) override;
    std::string BroadcastDin(const std::vector<uint8_t>& raw_tx) override;
    std::string BroadcastBtc(const std::vector<uint8_t>& raw_tx) override;

private:
    DinRpc din_;
    BtcRpc btc_;
    SwapWatchSpec spec_;
    std::unique_ptr<DinWatcher> din_watcher_;
    std::unique_ptr<BtcWatcher> btc_watcher_;
    void RebuildWatchers();
    std::function<void(const std::string&)> tower_sink_;
    std::function<bool(const std::string&, const std::string&)> tower_ack_;
};

// HTLC addresses for funding.
std::string DinHtlcAddressFor(const SwapRecord& record, const std::string& hrp);
std::string BtcHtlcAddressFor(const SwapRecord& record, const std::string& hrp);  // P2WSH, bech32 v0

// Signed spends, serialized for broadcast. Throw std::invalid_argument when the
// keys do not match the record or the secret is missing or wrong.
std::vector<uint8_t> SignedDinClaim(const SwapSession& s, const SwapKeys& keys,
                                    const FundingOutput& funding, uint64_t fee_una);
std::vector<uint8_t> SignedDinRefund(const SwapSession& s, const SwapKeys& keys,
                                     const FundingOutput& funding, uint64_t fee_una);
// Bob's CPFP child / sweep: spends his claim's sweep output to din_payout_script.
std::vector<uint8_t> SignedDinSweep(const SwapSession& s, const SwapKeys& keys, const FundingOutput& claim_output,
                                    uint64_t fee_una);
std::vector<uint8_t> SignedBtcClaim(const SwapSession& s, const SwapKeys& keys,
                                    const BtcFunding& funding, uint64_t fee_sat);
std::vector<uint8_t> SignedBtcRefund(const SwapSession& s, const SwapKeys& keys,
                                     const BtcFunding& funding, uint64_t fee_sat);

struct TickReport {
    bool observed{false};  // false: a chain was unreachable; nothing decided, saved or done
    SwapState before{};
    SwapState after{};
    std::vector<ActionKind> actions;  // what Step() asked for this tick
    std::vector<std::string> events;  // human-readable log: actions, failures, alerts
};

class SwapRunner {
public:
    // Throws std::invalid_argument if the keys (or Alice's secret) do not match
    // the record for its role.
    SwapRunner(SwapSession session, SwapKeys keys, RunnerConfig config, SwapChainIo& io, SwapStore& store);

    // Throws only if the store cannot save; then nothing was done.
    TickReport Tick(uint32_t wall_clock_unix);

    // Recovery: broadcast this party's own refund now, whatever the state (it
    // never reveals the secret; consensus enforces the lock time, so an early
    // call is refused by the node). Returns the txid; throws if there is
    // nothing to refund or the node refuses.
    std::string ForceRefund(uint32_t wall_clock_unix);

    const SwapSession& session() const { return session_; }

private:
    void Execute(const Action& action, const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                 std::vector<std::string>& events);
    void ArmTower(const DinWatchReport& din, const BtcWatchReport& btc, std::vector<std::string>& events);
    // The BTC lock as observed, else the one the tower was armed for: Bob's
    // claim must always be a package rung (the tower's children spend those).
    std::optional<BtcFunding> ArmedBtcFunding(const BtcWatchReport& btc) const;
    bool ArmTowerWith(const FundingOutput& din_funding, const BtcFunding& btc_funding, std::vector<std::string>& events);
    TowerPackage MyTowerPackage(const FundingOutput& din_funding, const BtcFunding& btc_funding) const;
    void SendPreparedFunding(const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                             std::vector<std::string>& events);
    void ExpirePreparedFunding(uint32_t now, const std::string& why, std::vector<std::string>& events);
    void BumpOrSweepDin(const DinWatchReport& din, uint32_t now, std::vector<std::string>& events);
    uint64_t DinFeeByUrgency(uint32_t din_mtp) const;
    void PinAndSave(const std::string& din_txid, const std::string& btc_txid);
    void SetClaimSeen(bool seen);
    uint64_t BtcFeeNow(uint32_t now) const;
    void SingleChainRebroadcast(const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                                std::vector<std::string>& events);

    SwapSession session_;
    SwapKeys keys_;
    RunnerConfig config_;
    SwapChainIo& io_;
    SwapStore& store_;
};

}  // namespace dinero::swap
