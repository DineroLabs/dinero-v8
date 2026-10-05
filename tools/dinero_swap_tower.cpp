// dinero-swap-tower: Bob's always-on watchtower for DIN <-> BTC swaps
// (docs/design/din-btc-atomic-swaps.md §6.2). Holds only pre-signed
// transactions; never a private key or a secret.
//
//   dinero-swap-tower --inbox DIR --din-rpc HOST:PORT --din-auth USER:PASS
//                     --btc-rpc HOST:PORT --btc-auth USER:PASS
//                     [--din-hrp din|tdin|rdin] [--btc-chain main|test|regtest]
//                     [--interval SECONDS] [--escalate-after SECONDS]
//
// Arming: Bob's wallet writes <DIR>/<id>.pkg (WriteTowerInbox). Each package
// is verified on load; a bad one is renamed .rejected with the reason logged.
// When a swap is settled on chain the package is renamed .done. A restart
// re-reads the inbox and re-derives everything from the chains.
#include "wallet/swap/tower.h"

#include "rpc_client.h"

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

struct Watched {
    std::unique_ptr<RpcSwapChainIo> io;
    std::unique_ptr<Watchtower> tower;
};

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt{
        {"--din-hrp", "din"}, {"--btc-chain", "main"}, {"--interval", "30"}, {"--escalate-after", "1800"}};
    for (int i = 1; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    for (const char* k : {"--inbox", "--din-rpc", "--din-auth", "--btc-rpc", "--btc-auth"}) {
        if (!opt.count(k)) {
            std::cerr << "usage: see tools/dinero_swap_tower.cpp (missing " << k << ")\n";
            return 2;
        }
    }
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });
    const auto din = MakeRpc(Client(opt["--din-rpc"], opt["--din-auth"]));
    const auto btc = MakeRpc(Client(opt["--btc-rpc"], opt["--btc-auth"]));
    TowerConfig config;
    config.escalate_after_seconds = static_cast<uint32_t>(std::stoul(opt["--escalate-after"]));
    const auto interval = std::chrono::seconds(std::stoul(opt["--interval"]));
    const std::string inbox = opt["--inbox"];
    std::map<std::string, Watched> watched;  // package path -> tower

    std::cout << "tower: watching " << inbox << std::endl;
    while (!g_stop) {
        // Load new packages.
        if (DIR* d = opendir(inbox.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string name = e->d_name;
                if (name.size() < 5 || name.compare(name.size() - 4, 4, ".pkg") != 0) continue;
                const std::string path = inbox + "/" + name;
                if (watched.count(path)) continue;
                std::ifstream in(path);
                std::stringstream text;
                text << in.rdbuf();
                try {
                    auto package = DecodeTowerPackage(text.str());
                    Watched w;
                    w.io = std::make_unique<RpcSwapChainIo>(din, btc, TowerWatchSpec(package, opt["--btc-chain"]));
                    w.tower = std::make_unique<Watchtower>(std::move(package), config, *w.io);
                    watched.emplace(path, std::move(w));
                    std::cout << "tower: armed " << name << std::endl;
                } catch (const std::exception& ex) {
                    std::cout << "tower: REJECTED " << name << ": " << ex.what() << std::endl;
                    std::rename(path.c_str(), (path + ".rejected").c_str());
                }
            }
            closedir(d);
        }
        // Tick every armed swap.
        for (auto it = watched.begin(); it != watched.end();) {
            const auto r = it->second.tower->Tick(static_cast<uint32_t>(std::time(nullptr)));
            for (const auto& ev : r.events) std::cout << "tower: " << it->first << ": " << ev << std::endl;
            if (r.finished) {
                std::rename(it->first.c_str(), (it->first + ".done").c_str());
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
