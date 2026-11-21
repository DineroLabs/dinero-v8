// Drives the light Bitcoin P2P client against a real node (tests/wallet/
// swap_btc_p2p_regtest.sh):
//   swap_btc_p2p_tool sync <network> <host> <port> <header file>
//       -> "tip <height> <hash> accepted <n> synced <0|1>"
//   swap_btc_p2p_tool send <network> <host> <port> <header file> <raw tx hex>
//       -> "sent <txid>" if the peer fetched it
// Exit code 0 only if the conversation ended as planned.

#include "wallet/swap/btc_header_store.h"
#include "wallet/swap/btc_p2p_client.h"
#include "wallet/swap/btc_raw_tx.h"

#include <ctime>
#include <iostream>
#include <string>

using namespace dinero::swap;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: swap_btc_p2p_tool sync|send <network> <host> <port> <header file> [raw tx hex]\n";
        return 2;
    }
    const std::string mode = argv[1], network = argv[2], host = argv[3], file = argv[5];
    const auto port = static_cast<uint16_t>(std::stoul(argv[4]));
    const BtcChainParams params = network == "main" ? BtcChainParams::Mainnet()
                                  : network == "testnet4" ? BtcChainParams::Testnet4()
                                                          : BtcChainParams::Regtest();
    BtcHeaderChain chain(params);
    const auto now = static_cast<uint32_t>(std::time(nullptr));
    LoadBtcHeaderChain(file, chain, now);

    std::vector<std::vector<uint8_t>> txs;
    if (mode == "send") {
        if (argc < 7) return 2;
        const std::string hex = argv[6];
        std::vector<uint8_t> raw;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) raw.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        txs.push_back(raw);
    }
    const auto peers = ResolveBtcPeers({host}, port, 1);
    if (peers.empty()) {
        std::cout << "error cannot resolve " << host << "\n";
        return 1;
    }
    BtcPeerOptions options;
    options.total_timeout_seconds = 60;
    const BtcPeerOutcome out = RunBtcPeer(peers[0], BtcP2pNetwork::For(params.name), chain, txs, options);
    SaveBtcHeaderChain(file, chain);
    std::cout << "tip " << chain.TipHeight() << " " << BtcHashToHex(chain.TipHash()) << " accepted " << out.headers_accepted
              << " synced " << out.headers_synced << "\n";
    for (const auto& txid : out.txs_sent) std::cout << "sent " << BtcHashToHex(txid) << "\n";
    if (!out.error.empty()) {
        std::cout << "error " << out.error << "\n";
        return 1;
    }
    return 0;
}
