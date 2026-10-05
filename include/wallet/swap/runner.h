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
    std::vector<uint8_t> din_payout_script;   // Alice: refund goes here; Bob: claim goes here
    std::vector<uint8_t> btc_payout_script;   // Alice: claim goes here; Bob: refund goes here
    bool tower_armed{false};                  // Bob: the watchtower accepted this swap's package
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
};

struct RunnerConfig {
    uint64_t din_fee_una{100'000};          // DIN claim/refund fee with >= 24 h to T_din
    uint64_t din_fee_urgent_una{1'000'000}; // DIN claim fee within 6 h of T_din (linear between)
    uint64_t btc_fee_sat{1'000};
    // Bob's watchtower ladders (only used when a tower is configured).
    bool use_tower{false};
    uint64_t din_tower_start_feerate_una_per_vb{1'000};
    uint64_t btc_tower_start_feerate_sat_per_vb{2};
    uint32_t tower_rungs{8};
    uint32_t tower_max_fee_percent{5};  // per rung, of the swap amount
    std::string din_hrp{"din"};   // "din" / "tdin" / "rdin"
    std::string btc_hrp{"bc"};    // "bc" / "tb" / "bcrt"
    std::string btc_chain{"main"};  // Bitcoin Core getblockchaininfo "chain" this swap must run on
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
    void ArmTower(const std::string& package_text) override;
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

    const SwapSession& session() const { return session_; }

private:
    void Execute(const Action& action, const DinWatchReport& din, const BtcWatchReport& btc,
                 std::vector<std::string>& events);
    void ArmTower(const DinWatchReport& din, const BtcWatchReport& btc, std::vector<std::string>& events);
    void PinAndSave(const std::string& din_txid, const std::string& btc_txid);

    SwapSession session_;
    SwapKeys keys_;
    RunnerConfig config_;
    SwapChainIo& io_;
    SwapStore& store_;
};

}  // namespace dinero::swap
