#pragma once
// The Zcash counter chain for the swap runner (milestone 5 of
// docs/design/din-zec-atomic-swaps.md): a SwapChainIo whose counter leg is a
// Zebra node and the swap engine's own Zcash keys.
//
// The runner is chain-neutral (runner.h): this class overrides the counter
// hooks. Its BtcWatchReport / BtcFunding carry the Zcash lock (zatoshi).
//   - Funding: Zebra has no wallet. Bob's lock is always prepared from his
//     per-swap deposit, signed with the deposit key, and sent after the
//     tower's ack (or at once without a tower). It never expires, so a lock
//     waiting for the ack stays valid; the upgrade guard refuses to prepare
//     one whose window reaches a network upgrade.
//   - Claims and wallet refunds pay a fixed 2 x the ZIP 317 conventional fee
//     (no RBF on Zcash) and are signed for the next block's branch id with
//     expiry tip + 40; each re-broadcast signs again, so an expired one is
//     replaced.
//   - Tower: one refund rung at 4 x the conventional fee, expiry 0, signed
//     for the branch persisted in the session when the tower was armed.
//
// Linked only into test tools until rust/zcash_swap folds into
// rust/orchard_backend (v8.2.1): runner.cpp and tower.cpp do not include it.

#include "wallet/swap/runner.h"
#include "wallet/swap/zec_watcher.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dinero::swap {

struct ZecIoConfig {
    std::string expected_chain{"main"};  // Zebra's chain name: "main", or "test" (testnet and regtest)
    ZecNetwork network{ZecNetwork::Main};
    Bytes32 deposit_secret_key{};        // Bob: the per-swap deposit key; zero for Alice
    std::vector<uint8_t> change_script;  // Bob: where a deposit's excess goes (his ZEC payout)
    uint32_t deposit_confirmations{kZecDepositConfirmations};
    uint32_t funding_fee_multiplier{2};
    uint64_t spend_fee_zat{2 * 10'000};  // claim and wallet refund: 2 x conventional
    uint64_t rung_fee_zat{4 * 10'000};   // tower refund rung: 4 x conventional (ZIP 317's weight cap)
    uint32_t spend_expiry_blocks{40};
    uint32_t upgrade_margin_seconds{24 * 60 * 60};  // the window checked is T_zec + this
    // sendrawtransaction to Zebra: returns the txid, throws std::runtime_error
    // carrying Zebra's message (the may-have-left check reads it).
    std::function<std::string(const std::string& raw_hex)> broadcast;
    std::function<uint32_t()> clock;     // wall clock for the upgrade guard (default: time())
};

class ZecSwapChainIo : public RpcSwapChainIo {
public:
    ZecSwapChainIo(DinRpc din, ZecRpc zec, SwapWatchSpec spec, ZecIoConfig config);

    BtcWatchReport ObserveBtc() override;
    std::string FundBtc(const std::string& address, uint64_t amount_zat) override;
    PreparedBtcFunding PrepareFundBtc(const std::string& address, uint64_t amount_zat) override;
    void ReleasePreparedFunding(const std::vector<uint8_t>& raw) override;
    std::optional<bool> PreparedFundingUnsent(const std::vector<uint8_t>& raw) override;
    std::string BroadcastBtc(const std::vector<uint8_t>& raw_tx) override;
    void PinFunding(const std::string& din_txid, const std::string& zec_txid) override;

    CounterChainRules CounterRules() const override;
    std::string CounterHtlcAddress(const SwapRecord& record, const RunnerConfig& config) const override;
    uint64_t CounterSpendFee(const SwapSession& s, const RunnerConfig& config, uint32_t now) const override;
    std::vector<uint8_t> SignCounterClaim(const SwapSession& s, const SwapKeys& keys, const BtcFunding& funding,
                                          uint64_t fee) override;
    std::vector<uint8_t> SignCounterRefund(const SwapSession& s, const SwapKeys& keys, const BtcFunding& funding,
                                           uint64_t fee) override;
    bool CounterFundsByPreparing() const override { return true; }
    bool CounterBroadcastMayHaveLeft(const std::string& error) const override;
    void AddCounterTowerRungs(TowerPackage& package, const SwapSession& s, const SwapKeys& keys,
                              const BtcFunding& funding, const BtcFeeLadderPolicy& policy) const override;

    const std::optional<ZecChainInfo>& last_chain() const { return chain_; }
    // Reads the chain state (tip, branch id) the signer uses; ObserveBtc does this too.
    void ObserveBtcChain() { ChainNow(); }

private:
    ZecChainInfo ChainNow();
    ZecTx Spend(const SwapSession& s, const SwapKeys& keys, const BtcFunding& funding, uint64_t fee, bool claim);
    void RebuildZecWatcher();

    ZecRpc zec_;
    SwapWatchSpec zspec_;
    ZecIoConfig config_;
    std::unique_ptr<ZecWatcher> watcher_;
    std::optional<ZecChainInfo> chain_;  // from the last observation
};

ZecNetwork ZecNetworkFor(SwapNetwork network);

// Zebra names both testnet and regtest "test", so the chain name alone cannot
// pair a Dinero network with its Zcash network: the genesis block hash can.
std::string ZecGenesisFor(SwapNetwork network);

// `rpc`, except that getblockchaininfo answers nothing unless the node's block 0
// is `genesis_hash`: every observer then treats the node as unreachable.
ZecRpc GuardZecGenesis(ZecRpc rpc, std::string genesis_hash);

}  // namespace dinero::swap
