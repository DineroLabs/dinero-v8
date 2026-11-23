// The real network for BtcLightBackend. Kept apart from the backend so that
// the backend (and everything that tests it) links neither libcurl nor sockets.
#include "wallet/swap/btc_http_curl.h"
#include "wallet/swap/btc_light_backend.h"

namespace dinero::swap {

BtcLightTransport MakeBtcLightTransport() {
    BtcLightTransport t;
    t.http = MakeBtcHttp();
    t.resolve = ResolveBtcPeers;
    t.talk = [](const BtcPeerEndpoint& peer, const BtcP2pNetwork& net, BtcHeaderChain& chain,
                const std::vector<std::vector<uint8_t>>& txs) {
        BtcPeerOptions options;
        options.connect_timeout_seconds = 6;
        options.idle_timeout_seconds = 15;
        options.total_timeout_seconds = 60;
        options.linger_seconds = 8;
        return RunBtcPeer(peer, net, chain, txs, options);
    };
    return t;
}

}  // namespace dinero::swap
