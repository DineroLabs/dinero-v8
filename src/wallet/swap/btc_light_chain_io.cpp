#include "wallet/swap/btc_light_chain_io.h"

#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/btc_light_deposit.h"
#include "wallet/swap/swap_crypto.h"

#include <ctime>
#include <stdexcept>

namespace dinero::swap {
namespace {

const char* kNoWallet =
    "this node uses Bitcoin without Bitcoin Core and cannot pay BTC yet: it can sell DIN for BTC, not buy DIN with BTC";

}  // namespace

LightBtcChainIo::LightBtcChainIo(DinRpc din, BtcLightBackend& backend, const SwapSession& session,
                                 const RunnerConfig& config, std::function<uint32_t()> clock, Bytes32 deposit_key,
                                 uint64_t max_funding_fee_sat)
    : RpcSwapChainIo(std::move(din),
                     [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; },  // no Bitcoin Core
                     session, config),
      backend_(backend), light_spec_(WatchSpecFor(session, config)), clock_(std::move(clock)), deposit_key_(deposit_key),
      max_funding_fee_sat_(max_funding_fee_sat), change_script_(session.btc_payout_script) {
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

// The lock, built from the buyer's deposit: nothing is sent here.
PreparedBtcFunding LightBtcChainIo::PrepareFundBtc(const std::string&, uint64_t amount_sat) {
    if (!CounterFundsByPreparing()) throw std::runtime_error(kNoWallet);
    const auto pubkey = detail::CompressedOf(deposit_key_);
    BtcDepositView view;
    {
        const auto lock = backend_.Lock();
        view = BtcDepositWatcher(backend_.chain(), backend_.servers(), BtcP2wpkhScript(pubkey)).Observe();
    }
    if (!view.ok) throw std::runtime_error("cannot reach any Bitcoin server to read the deposit");
    const auto feerate = backend_.FeeRate(2);
    if (!feerate) throw std::runtime_error("cannot reach any Bitcoin server for the network fee");
    const auto lock_script = BtcP2wshScriptPubKey(BuildBtcHtlcWitnessScript(MakeBtcTerms(light_spec_.offer, light_spec_.accept)));
    const BtcDepositSpend plan =
        BuildBtcLockFromDeposit(view.deep_coins, deposit_key_, lock_script, amount_sat, change_script_, *feerate, max_funding_fee_sat_);
    PreparedBtcFunding prepared;
    prepared.raw = plan.raw;
    prepared.txid = BtcHashToHex(plan.txid);
    prepared.vout = 0;
    return prepared;
}

// Unsent means every coin it spends is still unspent. A server that can show
// a spender settles it; "unspent" from all that answer is believed (a wrong
// answer only delays a cancel the network would refuse anyway).
std::optional<bool> LightBtcChainIo::PreparedFundingUnsent(const std::vector<uint8_t>& raw) {
    const auto tx = ParseBtcRawTx(raw);
    if (!tx) return std::nullopt;
    bool answered = false;
    for (const auto& in : tx->vin) {
        for (EsploraServer* server : backend_.servers()) {
            const auto out = server->Outspend(in.prev_txid, in.prev_vout);
            if (!out) continue;
            answered = true;
            if (out->spent) return false;
        }
    }
    return answered ? std::optional<bool>(true) : std::nullopt;
}

bool LightBtcChainIo::CounterBroadcastMayHaveLeft(const std::string& error) const {
    return error.rfind("Bitcoin transaction refused", 0) != 0;
}

}  // namespace dinero::swap
