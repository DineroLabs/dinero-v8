#pragma once
// The swap runner's Bitcoin side without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.4).
//
// An RpcSwapChainIo whose Bitcoin leg is the node's light backend: it observes
// the lock through BtcLightWatcher and broadcasts through BtcLightBackend. The
// Dinero leg, the signing, the fees and the tower rungs are the base class's.
//
// Selling DIN for BTC needs nothing else. Buying DIN pays BTC in: with a
// deposit key the lock is built from the buyer's deposit address
// (btc_deposit.h) and sent by the runner; without one, FundBtc and
// PrepareFundBtc refuse.

#include "wallet/swap/btc_light_backend.h"
#include "wallet/swap/btc_light_watcher.h"
#include "wallet/swap/runner.h"

#include <functional>
#include <memory>

namespace dinero::swap {

class LightBtcChainIo : public RpcSwapChainIo {
public:
    // The backend must outlive this object. `clock` defaults to the wall clock.
    // `deposit_key`: the buyer's per-swap deposit key (zero: this side does not
    // pay BTC). `max_funding_fee_sat`: the most the lock's network fee may be.
    LightBtcChainIo(DinRpc din, BtcLightBackend& backend, const SwapSession& session, const RunnerConfig& config,
                    std::function<uint32_t()> clock = {}, Bytes32 deposit_key = {}, uint64_t max_funding_fee_sat = 0);

    // For a watcher that has no session, only what to watch: the watchtower.
    LightBtcChainIo(DinRpc din, BtcLightBackend& backend, SwapWatchSpec spec, std::function<uint32_t()> clock = {});

    BtcWatchReport ObserveBtc() override;
    std::string BroadcastBtc(const std::vector<uint8_t>& raw_tx) override;
    std::string FundBtc(const std::string& address, uint64_t amount_sat) override;
    PreparedBtcFunding PrepareFundBtc(const std::string& address, uint64_t amount_sat) override;
    std::optional<bool> PreparedFundingUnsent(const std::vector<uint8_t>& raw) override;
    void ReleasePreparedFunding(const std::vector<uint8_t>&) override {}  // nothing is reserved anywhere
    bool CounterFundsByPreparing() const override { return deposit_key_ != Bytes32{}; }
    void PinFunding(const std::string& din_txid, const std::string& btc_txid) override;
    // Only a refusal by every source that answered means the transaction did
    // not leave (BtcLightBackend::Broadcast).
    bool CounterBroadcastMayHaveLeft(const std::string& error) const override;

    // Why the last Bitcoin observation was not usable ("" when it was).
    const std::string& last_problem() const { return watcher_->last_problem(); }

private:
    void RebuildLightWatcher();

    BtcLightBackend& backend_;
    SwapWatchSpec light_spec_;
    std::function<uint32_t()> clock_;
    std::unique_ptr<BtcLightWatcher> watcher_;
    Bytes32 deposit_key_{};
    uint64_t max_funding_fee_sat_{0};
    std::vector<uint8_t> change_script_;  // the buyer's refund address: a deposit's excess goes there
};

}  // namespace dinero::swap
