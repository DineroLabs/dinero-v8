// Drives the light Bitcoin HTTPS transport and server client for real
// (tests/wallet/swap_btc_http_test.sh, and by hand against public servers):
//   swap_btc_http_tool get <url> <max body bytes> <timeout seconds> [ca bundle]
//       -> "status <n> bytes <m>"
//   swap_btc_http_tool prove <network> <server base url> <header file> <txid>
//       -> "proven height <h> confirmations <c> outputs <n>" or "refused <why>"
//   swap_btc_http_tool tip <server base url>            -> "tip <height>"
//   swap_btc_http_tool fee <server base url> <target>   -> "fee <sat/vB>"

#include "wallet/swap/btc_esplora.h"
#include "wallet/swap/btc_header_store.h"
#include "wallet/swap/btc_http_curl.h"

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
