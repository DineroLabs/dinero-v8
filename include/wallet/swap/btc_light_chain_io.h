#pragma once
// The swap runner's Bitcoin side without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.4).
//
// An RpcSwapChainIo whose Bitcoin leg is the node's light backend: it observes
// the lock through BtcLightWatcher and broadcasts through BtcLightBackend. The
// Dinero leg, the signing, the fees and the tower rungs are the base class's.
//
// Selling DIN for BTC needs nothing else. Paying BTC in (buying DIN) needs a
// Bitcoin wallet, which a light node does not have yet: FundBtc and
// PrepareFundBtc refuse, and SwapManager refuses to accept such an offer.

#include "wallet/swap/btc_light_backend.h"
#include "wallet/swap/btc_light_watcher.h"
#include "wallet/swap/runner.h"

#include <functional>
#include <memory>

namespace dinero::swap {

class LightBtcChainIo : public RpcSwapChainIo {
public:
    // The backend must outlive this object. `clock` defaults to the wall clock.
    LightBtcChainIo(DinRpc din, BtcLightBackend& backend, const SwapSession& session, const RunnerConfig& config,
                    std::function<uint32_t()> clock = {});

    BtcWatchReport ObserveBtc() override;
    std::string BroadcastBtc(const std::vector<uint8_t>& raw_tx) override;
    std::string FundBtc(const std::string& address, uint64_t amount_sat) override;
    PreparedBtcFunding PrepareFundBtc(const std::string& address, uint64_t amount_sat) override;
    std::optional<bool> PreparedFundingUnsent(const std::vector<uint8_t>& raw) override;
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
};

}  // namespace dinero::swap
