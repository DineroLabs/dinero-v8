// Drives the light Bitcoin HTTPS transport and server client for real
// (tests/wallet/swap_btc_http_test.sh, and by hand against public servers):
//   swap_btc_http_tool get <url> <max body bytes> <timeout seconds> [ca bundle]
//       -> "status <n> bytes <m>"
//   swap_btc_http_tool prove <network> <server base url> <header file> <txid>
//       -> "proven height <h> confirmations <c> outputs <n>" or "refused <why>"
//   swap_btc_http_tool tip <server base url>            -> "tip <height>"
//   swap_btc_http_tool fee <server base url> <target>   -> "fee <sat/vB>"
//   swap_btc_http_tool deposit <network> <server urls, comma separated> <header file> <script hex>
//       -> what the deposit watcher sees at that script, proven and for recovery, and how many
//          requests each took

#include "wallet/swap/btc_esplora.h"
#include "wallet/swap/btc_header_store.h"
#include "wallet/swap/btc_http_curl.h"
#include "wallet/swap/btc_light_deposit.h"

#include <chrono>
#include <memory>
#include <sstream>

#include <ctime>
#include <iostream>
#include <string>

using namespace dinero::swap;

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    try {
        if (mode == "get" && argc >= 5) {
            BtcHttpOptions options;
            options.max_body_bytes = std::stoull(argv[3]);
            options.total_timeout_seconds = std::stol(argv[4]);
            options.connect_timeout_seconds = options.total_timeout_seconds;
            if (argc >= 6) options.ca_bundle_path = argv[5];
            const auto r = MakeBtcHttp(options).get(argv[2]);
            std::cout << "status " << r.status << " bytes " << r.body.size() << "\n";
            return 0;
        }
        if (mode == "tip" && argc >= 3) {
            EsploraServer server(argv[2], MakeBtcHttp());
            const auto tip = server.TipHeight();
            std::cout << (tip ? "tip " + std::to_string(*tip) : std::string("error")) << "\n";
            return tip ? 0 : 1;
        }
        if (mode == "fee" && argc >= 4) {
            EsploraServer server(argv[2], MakeBtcHttp());
            const auto fee = server.FeeRate(static_cast<uint32_t>(std::stoul(argv[3])));
            std::cout << (fee ? "fee " + std::to_string(*fee) : std::string("error")) << "\n";
            return fee ? 0 : 1;
        }
        if (mode == "deposit" && argc >= 6) {
            const std::string network = argv[2];
            BtcHeaderChain chain(network == "main" ? BtcChainParams::Mainnet()
                                 : network == "testnet4" ? BtcChainParams::Testnet4() : BtcChainParams::Regtest());
            LoadBtcHeaderChain(argv[4], chain, static_cast<uint32_t>(std::time(nullptr)));
            size_t requests = 0, failures = 0;
            BtcHttp http = MakeBtcHttp();
            const auto real_get = http.get;
            http.get = [&](const std::string& url) {
                ++requests;
                const auto r = real_get(url);
                if (r.status == 0 || r.status == 429 || r.status >= 500) ++failures;
                return r;
            };
            std::vector<std::unique_ptr<EsploraServer>> owned;
            std::vector<EsploraServer*> servers;
            std::stringstream urls(argv[3]);
            for (std::string url; std::getline(urls, url, ',');) {
                owned.push_back(std::make_unique<EsploraServer>(url, http));
                servers.push_back(owned.back().get());
            }
            std::vector<uint8_t> script;
            const std::string hex = argv[5];
            for (size_t i = 0; i + 1 < hex.size(); i += 2) script.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            BtcDepositWatcher watcher(chain, servers, script);
            auto run = [&](const char* name, bool recovery) {
                requests = failures = 0;
                const auto start = std::chrono::steady_clock::now();
                const BtcDepositView v = recovery ? watcher.ObserveForRecovery() : watcher.Observe();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::cout << name << " ok " << v.ok << " coins " << v.coins.size() << " seen_sat " << v.seen_sat << " confirmed_sat "
                          << v.confirmed_sat << " requests " << requests << " failed " << failures << " ms " << ms << "\n";
            };
            std::cout << "chain tip " << chain.TipHeight() << "\n";
            run("proven", false);
            run("proven-again", false);
            run("recovery", true);
            return 0;
        }
        if (mode == "prove" && argc >= 6) {
            const std::string network = argv[2];
            BtcHeaderChain chain(network == "main" ? BtcChainParams::Mainnet()
                                 : network == "testnet4" ? BtcChainParams::Testnet4() : BtcChainParams::Regtest());
            LoadBtcHeaderChain(argv[4], chain, static_cast<uint32_t>(std::time(nullptr)));
            const auto txid = BtcHashFromHex(argv[5]);
            if (!txid) {
                std::cout << "refused not a transaction id\n";
                return 1;
            }
            EsploraServer server(argv[3], MakeBtcHttp());
            std::string why;
            const auto proven = FetchProvenTx(server, chain, *txid, why);
            if (!proven) {
                std::cout << "refused " << why << "\n";
                return 1;
            }
            std::cout << "proven height " << proven->height << " confirmations " << proven->confirmations << " outputs "
                      << proven->tx.vout.size() << "\n";
            return 0;
        }
    } catch (const std::exception& e) {
        std::cout << "error " << e.what() << "\n";
        return 1;
    }
    std::cerr << "usage: swap_btc_http_tool get|tip|fee|prove ...\n";
    return 2;
}
