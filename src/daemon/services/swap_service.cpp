#include "daemon/services/swap_service.h"

#include "consensus/chainparams.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "rpc/rpc_registry.h"
#include "rpc_client.h"
#include "wallet/key_origin.h"
#include "wallet/swap/tower.h"
#include "wallet/wallet_manager.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>

namespace dinero {
namespace {

std::atomic<SwapService*> g_active{nullptr};

bool IsWrite(const std::string& m) {
    return m == "sendrawtransaction" || m == "wallet.sendtoaddress" || m == "wallet.getnewaddress";
}

// The node's own RPC handlers, called in-process with the same context the
// HTTP server builds (http_rpc_server.cpp), so the code paths are the same.
swap::DinRpc InProcessRpc(DaemonContext* ctx) {
    return [ctx](const std::string& method, const Json::Value& params) -> std::optional<Json::Value> {
        try {
            ::RpcHandler* handler = g_rpcRegistry.lookup(method);
            if (!handler) return std::nullopt;
            ExecutionContext ec;
            ec.client_id = "swap";
            ec.daemon = ctx;
            ec.logger = (method.rfind("wallet.", 0) == 0 && ctx->wallet_logger) ? ctx->wallet_logger
                                                                                : ctx->logger_interface;
            ec.mempool = nullptr;
            ec.utxo_view = nullptr;
            if (auto mp = std::dynamic_pointer_cast<dinero::MempoolService>(ctx->mempool)) {
                ec.mempool_v2 = const_cast<dinero::Mempool*>(&mp->mempool());
            }
            ec.wallet_manager = ctx->wallet ? &ctx->wallet->get() : nullptr;
            Json::Value result = (*handler)(ec, params);
            // Handlers report failures in-band; keep them for writes (the runner
            // shows the node's reason), treat them as "no answer" for reads.
            if (result.isObject() && result.isMember("error") && !result["error"].isNull() && !IsWrite(method)) {
                return std::nullopt;
            }
            return result;
        } catch (const std::exception&) {
            return std::nullopt;  // the HTTP server would turn this into an error reply
        }
    };
}

swap::SwapNetwork NetworkOf(const ChainParams& p) {
    if (p.name == "mainnet") return swap::SwapNetwork::Mainnet;
    if (p.name == "testnet") return swap::SwapNetwork::Testnet;
    return swap::SwapNetwork::Regtest;
}

}  // namespace

SwapService* SwapService::Active() { return g_active.load(); }

bool SwapService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    auto config = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    if (!config || !config->GetBool("swap.enable", false)) return true;  // disabled: nothing at all

    const std::string btc = config->GetString("swap.btc_rpc", "");
    const auto colon = btc.rfind(':');
    if (colon == std::string::npos) {
        std::cerr << "[Swap] swap.enable=1 needs swap.btc_rpc=HOST:PORT (Bitcoin Core RPC)" << std::endl;
        return false;
    }
    auto client = std::make_shared<rpc::RpcClient>(btc.substr(0, colon), uint16_t(std::stoi(btc.substr(colon + 1))),
                                                   config->GetString("swap.btc_rpc_user", ""),
                                                   config->GetString("swap.btc_rpc_pass", ""));
    swap::BtcRpc btc_rpc = [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        const auto r = client->call(m, p);
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) return std::nullopt;
        return (*r)["result"];
    };

    const ChainParams& params = dinero::Params();
    swap::SwapManagerConfig mc;
    mc.dir = config->DataDir() + "/swaps";
    mc.network = NetworkOf(params);
    mc.runner.din_hrp = params.hrp;
    mc.runner.btc_hrp = mc.network == swap::SwapNetwork::Mainnet ? "bc"
                      : mc.network == swap::SwapNetwork::Testnet ? "tb" : "bcrt";
    mc.runner.btc_chain = mc.network == swap::SwapNetwork::Mainnet ? "main"
                        : mc.network == swap::SwapNetwork::Testnet ? "test" : "regtest";
    mc.runner.din_fee_una = uint64_t(config->GetInt("swap.din_fee_una", int(mc.runner.din_fee_una)));
    mc.runner.din_fee_urgent_una = uint64_t(config->GetInt("swap.din_fee_urgent_una", int(mc.runner.din_fee_urgent_una)));
    mc.runner.btc_fee_sat = uint64_t(config->GetInt("swap.btc_fee_sat", int(mc.runner.btc_fee_sat)));
    tick_seconds_ = uint32_t(std::max(1, config->GetInt("swap.tick_seconds", 30)));
    // Mainnet beta: explicit opt-in and per-swap caps (swap_manager.h BetaPolicy).
    auto limit = [&](const char* key) -> std::optional<uint64_t> {
        const std::string v = config->GetString(key, "0");
        if (v.empty() || v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        return std::stoull(v);
    };
    const auto max_btc = limit("swap.max_btc_sat"), max_din = limit("swap.max_din_una");
    if (!max_btc || !max_din) {
        std::cerr << "[Swap] swap.max_btc_sat / swap.max_din_una must be whole numbers" << std::endl;
        return false;
    }
    const auto beta = swap::BetaPolicy(mc.network, config->GetBool("swap.mainnet_beta", false), *max_btc, *max_din);
    if (beta.refusal) {
        std::cerr << "[Swap] " << *beta.refusal << std::endl;
        return false;
    }
    mc.max_btc_sat = beta.max_btc_sat;
    mc.max_din_una = beta.max_din_una;

    // Swap keys from the wallet seed at m/SWAP'/... (see swap_manager.h).
    swap::KeyDeriver derive = [&ctx](const std::vector<uint32_t>& path) -> std::optional<swap::Bytes32> {
        if (!ctx.wallet || !ctx.wallet->hasActiveWallet()) return std::nullopt;
        auto& wm = ctx.wallet->get();
        if (wm.isWalletLocked()) return std::nullopt;
        wallet::KeyOriginInfo origin;
        for (uint32_t c : path) origin.path.push_back(c | wallet::KeyOriginInfo::HARDENED_BIT);
        const auto key = wm.DerivePrivateKey(origin);
        if (!key || key->size() != 32) return std::nullopt;
        swap::Bytes32 out{};
        std::copy(key->begin(), key->end(), out.begin());
        return out;
    };
    manager_ = std::make_unique<swap::SwapManager>(mc, derive, InProcessRpc(&ctx), btc_rpc);
    const std::string inbox = config->GetString("swap.tower_inbox", "");
    if (!inbox.empty()) {
        manager_->SetTowerSink([inbox](const std::string& package) { swap::WriteTowerInbox(inbox, package); });
    }
    std::cout << "[Swap] enabled: dir=" << mc.dir << " btc_rpc=" << btc << " tick=" << tick_seconds_ << "s"
              << (mc.max_btc_sat ? " max_btc_sat=" + std::to_string(mc.max_btc_sat) + " max_din_una=" +
                                       std::to_string(mc.max_din_una)
                                 : std::string())
              << (inbox.empty() ? "" : " tower_inbox=" + inbox) << std::endl;
    return true;
}

bool SwapService::Start() {
    if (!manager_) return true;
    stopping_ = false;
    g_active = this;
    thread_ = std::thread(&SwapService::Loop, this);
    return true;
}

void SwapService::Stop() {
    if (!manager_) return;
    g_active = nullptr;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SwapService::Loop() {
    while (true) {
        try {
            manager_->TickAll(static_cast<uint32_t>(std::time(nullptr)));
        } catch (const std::exception& e) {
            std::cerr << "[Swap] tick failed: " << e.what() << std::endl;
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        if (wake_.wait_for(lock, std::chrono::seconds(tick_seconds_), [this] { return stopping_; })) return;
    }
}

}  // namespace dinero
