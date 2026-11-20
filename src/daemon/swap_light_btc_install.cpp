// Bitcoin without Bitcoin Core for dinerod's swaps
// (docs/design/din-btc-swaps-without-bitcoin-core.md). Compiled into dinerod
// only with DINERO_SWAP_LIGHT_BTC: it registers SwapService's light-Bitcoin
// installer at start-up, so the default dinerod contains none of this code and
// does not link libcurl.
//
// Config (with swap.enable=1):
//   swap.btc_backend=light        use this instead of swap.btc_rpc
//   swap.btc_servers=URL,URL      Esplora servers (https; defaults per network)
//   swap.btc_peers=HOST,HOST      Bitcoin peers or DNS seeds for headers and
//                                 broadcast (defaults per network; "none": servers only)
//   swap.btc_peer_port=N          (the network's default)
//   swap.btc_min_servers=N        servers that must answer (default 2 on mainnet, else 1)
//   swap.btc_sync_seconds=N       least time between header syncs (default 45)
//
//   swap.tower_url=https://HOST   a watchtower armed over the network, for a
//                                 buyer of DIN (instead of swap.tower_inbox)
//
//   swap.btc_funding_fee_sat=N    the most a lock's network fee may be; part of
//                                 every deposit asked for (default 3000)
//
// With it the node sells DIN for BTC, and buys DIN by paying BTC to a per-swap
// deposit address from any wallet or exchange.
#include "daemon/services/config_service.h"
#include "daemon/services/swap_service.h"
#include "wallet/swap/btc_light_backend.h"
#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/btc_light_chain_io.h"
#include "wallet/swap/btc_light_deposit.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/btc_http_curl.h"
#include "wallet/swap/tower_http.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace dinero {
namespace {

std::vector<std::string> List(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream in(text);
    for (std::string item; std::getline(in, item, ',');) {
        const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

std::function<void(uint32_t)> InstallLightBitcoin(swap::SwapManager& manager, const ConfigService& config,
                                                  swap::SwapNetwork dinero_network, swap::DinRpc din) {
    // The Bitcoin network, which a rehearsal may set apart from Dinero's.
    const swap::SwapNetwork network = SwapService::BitcoinNetworkFor(config, dinero_network);
    swap::BtcLightBackendConfig c;
    std::string default_servers, default_peers;
    switch (network) {
    case swap::SwapNetwork::Mainnet:
        c.params = swap::BtcChainParams::Mainnet();
        default_servers = "https://mempool.space/api,https://blockstream.info/api";
        default_peers = "seed.bitcoin.sipa.be,dnsseed.bluematt.me,seed.btc.petertodd.net,seed.bitcoin.sprovoost.nl";
        c.min_servers = 2;
        break;
    case swap::SwapNetwork::Testnet:
        c.params = swap::BtcChainParams::Testnet4();
        default_servers = "https://mempool.space/testnet4/api";
        default_peers = "seed.testnet4.bitcoin.sprovoost.nl,seed.testnet4.wiz.biz";
        break;
    case swap::SwapNetwork::Regtest:
        c.params = swap::BtcChainParams::Regtest();
        break;
    }
    c.server_urls = List(config.GetString("swap.btc_servers", default_servers));
    const std::string peers = config.GetString("swap.btc_peers", default_peers);
    if (peers != "none") c.peer_hosts = List(peers);
    c.peer_port = static_cast<uint16_t>(config.GetInt("swap.btc_peer_port", 0));
    c.min_servers = static_cast<size_t>(config.GetInt("swap.btc_min_servers", static_cast<int>(c.min_servers)));
    c.min_sync_interval_seconds = static_cast<uint32_t>(std::max(1, config.GetInt("swap.btc_sync_seconds", 45)));
    std::error_code ignored;
    std::filesystem::create_directories(config.DataDir() + "/swaps", ignored);
    c.header_file = config.DataDir() + "/swaps/btc_headers.bin";

    std::shared_ptr<swap::BtcLightBackend> backend;
    try {
        backend = std::make_shared<swap::BtcLightBackend>(c, swap::MakeBtcLightTransport());
    } catch (const std::exception& e) {
        SwapService::Refuse(e.what());
        return nullptr;
    }

    swap::LightBtcSupport support;
    const uint64_t funding_fee = static_cast<uint64_t>(std::max(500, config.GetInt("swap.btc_funding_fee_sat", 3000)));
    const std::string hrp = network == swap::SwapNetwork::Mainnet ? "bc" : network == swap::SwapNetwork::Testnet ? "tb" : "bcrt";
    support.funding_fee_sat = funding_fee;
    support.make_io = [backend, din, funding_fee](const swap::SwapSession& session, const swap::RunnerConfig& rc,
                                                  const swap::Bytes32& deposit_key) {
        return std::unique_ptr<swap::RpcSwapChainIo>(
            new swap::LightBtcChainIo(din, *backend, session, rc, {}, deposit_key, funding_fee));
    };
    // One watcher per deposit address, kept: what it has proven is not fetched again.
    struct Deposits {
        std::mutex mu;
        std::map<swap::Bytes32, std::unique_ptr<swap::BtcDepositWatcher>> watchers;
    };
    auto deposits = std::make_shared<Deposits>();
    auto observe = [backend, deposits](const swap::Bytes32& key) {
        std::lock_guard<std::mutex> one(deposits->mu);
        const auto chain_lock = backend->Lock();
        auto& watcher = deposits->watchers[key];
        if (!watcher) {
            watcher = std::make_unique<swap::BtcDepositWatcher>(
                backend->chain(), backend->servers(), swap::BtcP2wpkhScript(swap::detail::CompressedOf(key)));
        }
        const swap::BtcDepositView view = watcher->Observe();
        if (!view.ok) throw std::runtime_error("cannot reach any Bitcoin server to read the deposit");
        return view;
    };
    // For giving coins back: also what cannot be proven (BtcDepositWatcher::ObserveForRecovery).
    auto observe_for_recovery = [backend, deposits](const swap::Bytes32& key) {
        std::lock_guard<std::mutex> one(deposits->mu);
        const auto chain_lock = backend->Lock();
        swap::BtcDepositWatcher watcher(backend->chain(), backend->servers(),
                                        swap::BtcP2wpkhScript(swap::detail::CompressedOf(key)));
        const swap::BtcDepositView view = watcher.ObserveForRecovery();
        if (!view.ok) throw std::runtime_error("cannot reach any Bitcoin server to read the deposit");
        return view;
    };
    // Watch-only balances of addresses the user's swaps paid to: one watcher
    // per script, kept, so what it has proven is not fetched again.
    struct Watched {
        std::mutex mu;
        std::map<std::vector<uint8_t>, std::unique_ptr<swap::BtcDepositWatcher>> watchers;
    };
    auto watched = std::make_shared<Watched>();
    support.script_activity = [backend, watched](const std::vector<uint8_t>& script) {
        std::lock_guard<std::mutex> one(watched->mu);
        const auto chain_lock = backend->Lock();
        auto& watcher = watched->watchers[script];
        if (!watcher) watcher = std::make_unique<swap::BtcDepositWatcher>(backend->chain(), backend->servers(), script);
        const swap::BtcDepositView view = watcher->Observe();
        if (!view.ok) throw std::runtime_error("cannot reach any Bitcoin server");
        return swap::BtcScriptActivity{view.seen_sat, view.ever_paid};
    };
    support.script_balance = [activity = support.script_activity](const std::vector<uint8_t>& script) {
        return activity(script).sat;
    };
    support.recoverable_sat = [observe_for_recovery](const swap::Bytes32& key) { return observe_for_recovery(key).seen_sat; };
    support.deposit_address = [hrp](const swap::Bytes32& key) {
        return swap::BtcP2wpkhAddress(swap::detail::CompressedOf(key), hrp);
    };
    support.deposit_status = [observe](const swap::Bytes32& key) {
        const swap::BtcDepositView view = observe(key);
        swap::ZecDepositStatus status;
        status.seen_zat = view.seen_sat;
        status.confirmed_zat = view.confirmed_sat;
        status.confirmations = view.newest_confirmations;
        return status;
    };
    support.sweep_deposit = [backend, observe_for_recovery, funding_fee](const swap::Bytes32& key,
                                                                         const std::vector<uint8_t>& to) {
        const swap::BtcDepositView view = observe_for_recovery(key);
        const auto feerate = backend->FeeRate(6);
        if (!feerate) throw std::runtime_error("cannot reach any Bitcoin server for the network fee");
        const auto sweep = swap::BuildBtcDepositSweep(view.coins, key, to, *feerate, funding_fee);
        return backend->Broadcast(sweep.raw);
    };
    const uint32_t checkpoint = c.params.checkpoint_height;
    support.tip_height = [backend, checkpoint] {
        const uint32_t tip = backend->tip_height();
        const std::string problem = backend->problem();
        if (tip == checkpoint && !problem.empty()) throw std::runtime_error("Bitcoin headers are not synced yet: " + problem);
        return tip;
    };
    manager.SetLightBitcoin(std::move(support));
    std::cout << "[Swap] Bitcoin without Bitcoin Core: " << c.server_urls.size() << " server(s), "
              << (c.peer_hosts.empty() ? std::string("no peers (servers only)") : std::to_string(c.peer_hosts.size()) + " peer seed(s)")
              << "; it sells DIN for BTC and buys DIN from a BTC deposit address" << std::endl;
    return [backend](uint32_t now) { backend->Sync(now); };
}

// swap.tower_url: a watchtower armed over the network (tower_http.h). Lives
// here because it needs the HTTPS client, which only this dinerod links.
bool InstallTowerUrl(swap::SwapManager& manager, const ConfigService& config) {
    const std::string url = config.GetString("swap.tower_url", "");
    if (const std::string problem = swap::TowerUrlProblem(url); !problem.empty()) {
        return SwapService::Refuse("swap.tower_url: " + problem);
    }
    swap::BtcHttpOptions options;
    options.max_body_bytes = 4096;  // the tower's answers are a line of JSON
    const swap::BtcHttp http = swap::MakeBtcHttp(options);
    manager.SetTowerSink(swap::MakeHttpTowerSink(url, http));
    manager.SetTowerAck(swap::MakeHttpTowerAck(url, http));
    return true;
}

const bool g_registered = [] {
    SwapService::SetLightBtcInstaller(InstallLightBitcoin);
    SwapService::SetTowerUrlInstaller(InstallTowerUrl);
    return true;
}();

}  // namespace
}  // namespace dinero
