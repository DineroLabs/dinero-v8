// dinero-swap-tower: Bob's always-on watchtower for DIN <-> BTC swaps
// (docs/design/din-btc-atomic-swaps.md §6.2). Holds only pre-signed
// transactions; never a private key or a secret.
//
//   dinero-swap-tower --inbox DIR --din-rpc HOST:PORT (--din-auth USER:PASS | --din-cookie FILE)
//                     --btc-rpc HOST:PORT --btc-auth USER:PASS
//                     [--din-hrp din|tdin|rdin] [--btc-chain main|testnet4|regtest]
//                     [--interval SECONDS] [--escalate-after SECONDS]
//                     [--expire-unfunded-after SECONDS]
// --expire-unfunded-after (a public tower): a package whose buyer never locked
// his coins is renamed .expired that long after its DIN deadline.
//
// --din-cookie FILE: the Dinero node's .cookie file ("user:password"). It is
// read again whenever it changes, because the node writes a new one each time
// it starts; a tower given the cookie's contents once would go blind then.
//
// Without Bitcoin Core (a tower built with DINERO_SWAP_LIGHT_BTC; see
// docs/design/din-btc-swaps-without-bitcoin-core.md): instead of --btc-rpc and
// --btc-auth,
//                     --btc-backend light --btc-servers URL[,URL…]
//                     [--btc-peers HOST[,HOST…]|none] [--btc-peer-port N]
//                     [--btc-headers FILE] [--btc-min-servers N]
// Bitcoin headers and broadcasts then go over the Bitcoin network and lookups
// to the servers, checked against the tower's own header chain.
//
// Arming: Bob's wallet writes <DIR>/<id>.pkg (WriteTowerInbox). Each package
// is verified on load; a bad one is renamed .rejected with the reason logged.
// When a swap is settled on chain the package is renamed .done. A restart
// re-reads the inbox and re-derives everything from the chains.
#include "wallet/swap/tower.h"

#include "rpc_client.h"
#ifdef DINERO_SWAP_LIGHT_BTC
#include "wallet/swap/btc_light_backend.h"
#include "wallet/swap/btc_light_chain_io.h"
#endif

#include <chrono>
#include <csignal>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

using namespace dinero;
using namespace dinero::swap;

namespace {

volatile std::sig_atomic_t g_stop = 0;

std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> MakeRpc(
    std::shared_ptr<rpc::RpcClient> client) {
    return [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        auto r = client->call(m, p);
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
            if (m == "sendrawtransaction") {
                std::cerr << "tower: " << m << " refused: "
                          << (r && r->isMember("error") ? (*r)["error"].toStyledString() : client->get_last_error());
            }
            return std::nullopt;
        }
        return (*r)["result"];
    };
}

std::shared_ptr<rpc::RpcClient> Client(const std::string& hostport, const std::string& auth) {
    const auto c = hostport.rfind(':'), a = auth.find(':');
    if (c == std::string::npos || a == std::string::npos) throw std::invalid_argument("expected HOST:PORT and USER:PASS");
    return std::make_shared<rpc::RpcClient>(hostport.substr(0, c), uint16_t(std::stoi(hostport.substr(c + 1))),
                                            auth.substr(0, a), auth.substr(a + 1));
}

// The Dinero node's RPC with the credentials in its cookie file, followed as
// the file changes. No file, or a bad one: "unreachable", and tried again.
std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> MakeCookieRpc(
    const std::string& hostport, const std::string& cookie_file) {
    struct State {
        std::string cookie;
        std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> rpc;
    };
    auto state = std::make_shared<State>();
    return [state, hostport, cookie_file](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        std::string cookie;
        if (std::ifstream in(cookie_file); in) std::getline(in, cookie);
        while (!cookie.empty() && (cookie.back() == '\r' || cookie.back() == ' ')) cookie.pop_back();
        if (cookie.find(':') == std::string::npos) return std::nullopt;
        if (cookie != state->cookie || !state->rpc) {
            state->rpc = MakeRpc(Client(hostport, cookie));
            state->cookie = cookie;
        }
        return state->rpc(m, p);
    };
}

struct Watched {
    std::unique_ptr<RpcSwapChainIo> io;
    std::unique_ptr<Watchtower> tower;
    std::string id;    // file name without .pkg
    std::string hash;  // TowerPackageHash of the package text
    TowerPackage package;
};

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt{
        {"--din-hrp", "din"}, {"--btc-chain", "main"}, {"--interval", "30"}, {"--escalate-after", "1800"}};
    for (int i = 1; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    const bool light = opt.count("--btc-backend") && opt["--btc-backend"] == "light";
    if (opt.count("--btc-backend") && !light && opt["--btc-backend"] != "core") {
        std::cerr << "tower: --btc-backend must be core or light\n";
        return 2;
    }
    std::vector<std::string> required{"--inbox", "--din-rpc"};
    if (!opt.count("--din-cookie")) required.push_back("--din-auth");
    if (light) required.push_back("--btc-servers");
    else required.insert(required.end(), {"--btc-rpc", "--btc-auth"});
    for (const std::string& k : required) {
        if (!opt.count(k)) {
            std::cerr << "usage: see tools/dinero_swap_tower.cpp (missing " << k << ")\n";
            return 2;
        }
    }
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });
    const auto din = opt.count("--din-cookie") ? MakeCookieRpc(opt["--din-rpc"], opt["--din-cookie"])
                                               : MakeRpc(Client(opt["--din-rpc"], opt["--din-auth"]));
    std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> btc;
#ifdef DINERO_SWAP_LIGHT_BTC
    std::unique_ptr<BtcLightBackend> backend;
#endif
    if (light) {
#ifdef DINERO_SWAP_LIGHT_BTC
        auto list = [](const std::string& text) {
            std::vector<std::string> out;
            std::stringstream in(text);
            for (std::string item; std::getline(in, item, ',');) {
                if (!item.empty()) out.push_back(item);
            }
            return out;
        };
        try {
            BtcLightBackendConfig c;
            const std::string chain = opt["--btc-chain"];
            c.params = chain == "main" ? BtcChainParams::Mainnet() : chain == "testnet4" ? BtcChainParams::Testnet4()
                                                                                          : BtcChainParams::Regtest();
            if (chain != c.params.name) throw std::invalid_argument("no light Bitcoin network named " + chain);
            c.server_urls = list(opt["--btc-servers"]);
            const std::string peers = opt.count("--btc-peers") ? opt["--btc-peers"]
                                      : chain == "main" ? "seed.bitcoin.sipa.be,dnsseed.bluematt.me,seed.btc.petertodd.net"
                                      : chain == "testnet4" ? "seed.testnet4.bitcoin.sprovoost.nl,seed.testnet4.wiz.biz" : "none";
            if (peers != "none") c.peer_hosts = list(peers);
            if (opt.count("--btc-peer-port")) c.peer_port = static_cast<uint16_t>(std::stoul(opt["--btc-peer-port"]));
            c.min_servers = opt.count("--btc-min-servers") ? std::stoul(opt["--btc-min-servers"]) : (chain == "main" ? 2 : 1);
            c.header_file = opt.count("--btc-headers") ? opt["--btc-headers"] : opt["--inbox"] + "/btc_headers.bin";
            c.min_sync_interval_seconds = static_cast<uint32_t>(std::max<unsigned long>(1, std::stoul(opt["--interval"])));
            backend = std::make_unique<BtcLightBackend>(c, MakeBtcLightTransport());
        } catch (const std::exception& e) {
            std::cerr << "tower: " << e.what() << "\n";
            return 2;
        }
        std::cout << "tower: Bitcoin without Bitcoin Core (" << backend->config().server_urls.size() << " server(s))" << std::endl;
#else
        std::cerr << "tower: --btc-backend light needs a tower built with DINERO_SWAP_LIGHT_BTC\n";
        return 2;
#endif
    } else {
        btc = MakeRpc(Client(opt["--btc-rpc"], opt["--btc-auth"]));
    }
    TowerConfig config;
    config.escalate_after_seconds = static_cast<uint32_t>(std::stoul(opt["--escalate-after"]));
    const auto interval = std::chrono::seconds(std::stoul(opt["--interval"]));
    const uint32_t expire_unfunded_after =
        opt.count("--expire-unfunded-after") ? static_cast<uint32_t>(std::stoul(opt["--expire-unfunded-after"])) : 0;
    const std::string inbox = opt["--inbox"];
    std::map<std::string, Watched> watched;  // package path -> tower

    std::cout << "tower: watching " << inbox << std::endl;
    while (!g_stop) {
#ifdef DINERO_SWAP_LIGHT_BTC
        // Our own view of Bitcoin first: every swap below is judged against it.
        if (backend) {
            backend->Sync(static_cast<uint32_t>(std::time(nullptr)));
            if (const std::string problem = backend->problem(); !problem.empty()) {
                std::cout << "tower: Bitcoin: " << problem << std::endl;
            }
        }
#endif
        // Load new packages.
        if (DIR* d = opendir(inbox.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string name = e->d_name;
                if (name.size() < 5 || name.compare(name.size() - 4, 4, ".pkg") != 0) continue;
                const std::string path = inbox + "/" + name;
                std::ifstream in(path);
                std::stringstream text;
                text << in.rdbuf();
                // A re-armed swap rewrites its package: load the new one (the ack
                // Bob waits for names the new hash).
                if (auto it = watched.find(path); it != watched.end()) {
                    if (it->second.hash == TowerPackageHash(text.str())) continue;
                    std::cout << "tower: " << name << " changed; reloading" << std::endl;
                    watched.erase(it);
                }
                try {
                    auto package = DecodeTowerPackage(text.str());
                    Watched w;
#ifdef DINERO_SWAP_LIGHT_BTC
                    if (backend) {
                        w.io = std::make_unique<LightBtcChainIo>(din, *backend, TowerWatchSpec(package, opt["--btc-chain"]));
                    } else
#endif
                    {
                        w.io = std::make_unique<RpcSwapChainIo>(din, btc, TowerWatchSpec(package, opt["--btc-chain"]));
                    }
                    w.package = package;
                    w.tower = std::make_unique<Watchtower>(std::move(package), config, *w.io);
                    w.id = name.substr(0, name.size() - 4);
                    w.hash = TowerPackageHash(text.str());
                    // A secret learned before a restart (public on Bitcoin already).
                    if (std::ifstream sf(inbox + "/" + w.id + ".secret"); sf) {
                        std::string hex;
                        sf >> hex;
                        if (hex.size() == 64) {
                            Bytes32 sec{};
                            for (size_t i = 0; i < 32; ++i) sec[i] = uint8_t(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
                            w.tower->SetKnownSecret(sec);
                        }
                    }
                    watched.emplace(path, std::move(w));
                    std::cout << "tower: loaded " << name << std::endl;
                } catch (const std::exception& ex) {
                    std::cout << "tower: REJECTED " << name << ": " << ex.what() << std::endl;
                    std::rename(path.c_str(), (path + ".rejected").c_str());
                }
            }
            closedir(d);
        }
        // Tick every armed swap.
        for (auto it = watched.begin(); it != watched.end();) {
            const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
            const TowerReport r = TickAndRecord(*it->second.tower, inbox, it->second.id, it->second.hash, now);
            for (const auto& ev : r.events) std::cout << "tower: " << it->first << ": " << ev << std::endl;
            if (r.finished) {
                std::rename(it->first.c_str(), (it->first + ".done").c_str());
                it = watched.erase(it);
            } else if (expire_unfunded_after &&
                       TowerPackageAbandoned(it->second.package, r, now, expire_unfunded_after)) {
                std::cout << "tower: " << it->first << ": the buyer never locked; dropped after its DIN deadline" << std::endl;
                std::rename(it->first.c_str(), (it->first + ".expired").c_str());
                it = watched.erase(it);
            } else {
                ++it;
            }
        }
        for (auto waited = std::chrono::seconds(0); waited < interval && !g_stop; waited += std::chrono::seconds(1)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::cout << "tower: stopped" << std::endl;
    return 0;
}
