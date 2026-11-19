#include "wallet/swap/btc_light_chain_io.h"

#include <ctime>
#include <stdexcept>

namespace dinero::swap {
namespace {

const char* kNoWallet =
    "this node uses Bitcoin without Bitcoin Core and cannot pay BTC yet: it can sell DIN for BTC, not buy DIN with BTC";

}  // namespace

LightBtcChainIo::LightBtcChainIo(DinRpc din, BtcLightBackend& backend, const SwapSession& session,
                                 const RunnerConfig& config, std::function<uint32_t()> clock)
    : RpcSwapChainIo(std::move(din),
                     [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; },  // no Bitcoin Core
                     session, config),
      backend_(backend), light_spec_(WatchSpecFor(session, config)), clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return static_cast<uint32_t>(std::time(nullptr)); };
    RebuildLightWatcher();
}

void LightBtcChainIo::RebuildLightWatcher() {
    BtcWatchTarget target;
    target.terms = MakeBtcTerms(light_spec_.offer, light_spec_.accept);
    target.scan_from_height = light_spec_.btc_scan_from_height;
    target.expected_amount_sat = light_spec_.offer.btc_amount_sat;
    target.expected_funding_txid = light_spec_.btc_funding_txid;
    target.expected_chain = light_spec_.btc_chain;
    BtcLightWatchConfig config;
    config.min_servers = backend_.config().min_servers;
    const auto lock = backend_.Lock();
    watcher_ = std::make_unique<BtcLightWatcher>(backend_.chain(), backend_.servers(), target, config);
}

void LightBtcChainIo::PinFunding(const std::string& din_txid, const std::string& btc_txid) {
    RpcSwapChainIo::PinFunding(din_txid, btc_txid);  // the Dinero watcher
    if (!btc_txid.empty() && btc_txid != light_spec_.btc_funding_txid) {
        light_spec_.btc_funding_txid = btc_txid;
        RebuildLightWatcher();
    }
}

BtcWatchReport LightBtcChainIo::ObserveBtc() {
    const auto lock = backend_.Lock();  // the header chain does not change under one observation
    return watcher_->Observe(clock_());
}

std::string LightBtcChainIo::BroadcastBtc(const std::vector<uint8_t>& raw_tx) { return backend_.Broadcast(raw_tx); }

std::string LightBtcChainIo::FundBtc(const std::string&, uint64_t) { throw std::runtime_error(kNoWallet); }

PreparedBtcFunding LightBtcChainIo::PrepareFundBtc(const std::string&, uint64_t) { throw std::runtime_error(kNoWallet); }

std::optional<bool> LightBtcChainIo::PreparedFundingUnsent(const std::vector<uint8_t>&) { return std::nullopt; }

bool LightBtcChainIo::CounterBroadcastMayHaveLeft(const std::string& error) const {
    return error.rfind("Bitcoin transaction refused", 0) != 0;
}

}  // namespace dinero::swap
