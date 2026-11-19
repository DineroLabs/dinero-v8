#pragma once
// The node's light view of Bitcoin, shared by all its swaps
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3).
//
// Owns the header chain (and its file), the list of public servers, and the
// Bitcoin peers headers come from. It does two things for the swaps:
//   - Sync: bring the header chain up to date, from Bitcoin peers first and
//     from the servers when no peer can be reached (port 8333 blocked). Either
//     way every header passes the chain's own checks.
//   - Broadcast: hand a transaction to every server and directly to peers, and
//     say whether anybody took it.
//
// Threads. Sync, Broadcast, chain() and servers() belong to the swap service's
// thread, the only one that touches the header chain. tip_height() and
// problem() may be called from any thread.
//
// The network itself is injected (BtcLightTransport), so the logic here is
// tested without one.

#include "wallet/swap/btc_esplora.h"
#include "wallet/swap/btc_header_chain.h"
#include "wallet/swap/btc_p2p_client.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dinero::swap {

struct BtcLightBackendConfig {
    BtcChainParams params;
    std::string header_file;               // where the header chain is kept ("": nowhere)
    std::vector<std::string> server_urls;  // Esplora base addresses, at least one
    std::vector<std::string> peer_hosts;   // DNS seeds or addresses (empty: servers only)
    uint16_t peer_port{0};                 // 0: the network's default
    size_t min_servers{1};                 // servers that must answer for a watcher's report to count
    size_t peers_per_round{2};             // peers asked in one Sync or Broadcast
    uint32_t min_sync_interval_seconds{45};
    uint32_t max_server_headers_per_sync{144};  // one day of blocks through the slow path
};

struct BtcLightTransport {
    BtcHttp http;
    // Host names to endpoints (ResolveBtcPeers).
    std::function<std::vector<BtcPeerEndpoint>(const std::vector<std::string>& hosts, uint16_t port, size_t limit)> resolve;
    // One conversation with one peer (RunBtcPeer).
    std::function<BtcPeerOutcome(const BtcPeerEndpoint&, const BtcP2pNetwork&, BtcHeaderChain&,
                                 const std::vector<std::vector<uint8_t>>& txs)>
        talk;
};

class BtcLightBackend {
public:
    // Throws std::invalid_argument for a configuration that cannot work (no
    // server, a server address that is not https, an unknown network).
    BtcLightBackend(BtcLightBackendConfig config, BtcLightTransport transport);

    // Brings the header chain up to date; at most once per
    // min_sync_interval_seconds unless `force`. Never throws.
    void Sync(uint32_t now_unix, bool force = false);

    // Sends `raw_tx` to every server and to peers. Returns its id (the usual
    // hex) if a server accepted it or already had it, or a peer took it.
    // Throws std::runtime_error otherwise; the message starts with
    // "Bitcoin transaction refused" when every source that answered refused
    // it and none might still relay it, and with "Bitcoin transaction may not
    // have been sent" when that cannot be told.
    std::string Broadcast(const std::vector<uint8_t>& raw_tx);

    const BtcHeaderChain& chain() const { return chain_; }
    std::vector<EsploraServer*> servers();
    const BtcLightBackendConfig& config() const { return config_; }

    uint32_t tip_height() const { return tip_height_.load(); }
    // Why the last Sync could not reach anyone ("" when it could).
    std::string problem() const;

private:
    size_t SyncFromPeers(const std::vector<std::vector<uint8_t>>& txs, std::vector<Bytes32>* delivered);
    bool SyncFromServers(uint32_t now_unix);
    void AfterChange(uint32_t tip_before);

    BtcLightBackendConfig config_;
    BtcLightTransport transport_;
    BtcP2pNetwork net_;
    BtcHeaderChain chain_;
    std::vector<std::unique_ptr<EsploraServer>> servers_;
    std::vector<BtcPeerEndpoint> peers_;
    size_t next_peer_{0};
    uint32_t last_sync_{0};
    bool synced_once_{false};
    std::atomic<uint32_t> tip_height_{0};
    mutable std::mutex problem_mu_;
    std::string problem_;
};

// The real network: libcurl for HTTPS and TCP for peers.
BtcLightTransport MakeBtcLightTransport();

}  // namespace dinero::swap
