#include "wallet/swap/btc_light_backend.h"

#include "wallet/swap/btc_header_store.h"
#include "wallet/swap/btc_raw_tx.h"

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {
namespace {

bool SaysAlreadyKnown(const std::string& error) {
    std::string lower;
    for (char c : error) lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    // Bitcoin Core's words for a transaction it has: in the mempool, or already in a block.
    return lower.find("already") != std::string::npos || lower.find("txn-already-known") != std::string::npos;
}

}  // namespace

BtcLightBackend::BtcLightBackend(BtcLightBackendConfig config, BtcLightTransport transport)
    : config_(std::move(config)), transport_(std::move(transport)), net_(BtcP2pNetwork::For(config_.params.name)),
      chain_(config_.params) {
    if (config_.server_urls.empty()) throw std::invalid_argument("light Bitcoin needs at least one server (swap.btc_servers)");
    if (config_.min_servers == 0 || config_.min_servers > config_.server_urls.size()) {
        throw std::invalid_argument("light Bitcoin: fewer servers configured than must answer");
    }
    for (const std::string& url : config_.server_urls) servers_.push_back(std::make_unique<EsploraServer>(url, transport_.http));
    if (!config_.peer_hosts.empty() && (!transport_.resolve || !transport_.talk)) {
        throw std::invalid_argument("light Bitcoin: peers configured without a peer transport");
    }
    if (config_.peer_port == 0) config_.peer_port = net_.default_port;
    tip_height_ = chain_.TipHeight();
}

std::vector<EsploraServer*> BtcLightBackend::servers() {
    std::vector<EsploraServer*> out;
    for (auto& s : servers_) out.push_back(s.get());
    return out;
}

std::string BtcLightBackend::problem() const {
    std::lock_guard<std::mutex> lock(problem_mu_);
    return problem_;
}

BtcHeaderChain BtcLightBackend::Copy() const {
    std::lock_guard<std::mutex> lock(chain_mu_);
    return chain_;
}

// Takes what a Sync or Broadcast learned into the shared chain: every header
// again through the chain's checks (those it already has are skipped).
void BtcLightBackend::Merge(const BtcHeaderChain& learned, uint32_t now_unix) {
    std::lock_guard<std::mutex> lock(chain_mu_);
    const Bytes32 before = chain_.TipHash();
    chain_.Adopt(learned, now_unix);
    tip_height_ = chain_.TipHeight();
    if (chain_.TipHash() == before || config_.header_file.empty()) return;
    try {
        SaveBtcHeaderChain(config_.header_file, chain_);
    } catch (const std::exception&) {
        // The file is a cache: the headers are fetched again after a restart.
    }
}

// Talks to up to peers_per_round peers; returns how many conversations ended
// with our chain as far as that peer's. Peers that fail are forgotten.
size_t BtcLightBackend::SyncFromPeers(BtcHeaderChain& chain, const std::vector<std::vector<uint8_t>>& txs,
                                      std::vector<Bytes32>* delivered) {
    if (config_.peer_hosts.empty()) return 0;
    if (peers_.size() < config_.peers_per_round) {
        peers_ = transport_.resolve(config_.peer_hosts, config_.peer_port, 32);
        next_peer_ = 0;
    }
    size_t good = 0;
    for (size_t round = 0; round < config_.peers_per_round && !peers_.empty(); ++round) {
        if (next_peer_ >= peers_.size()) next_peer_ = 0;
        const BtcPeerOutcome out = transport_.talk(peers_[next_peer_], net_, chain, txs);
        if (delivered) {
            for (const Bytes32& txid : out.txs_sent) {
                if (std::find(delivered->begin(), delivered->end(), txid) == delivered->end()) delivered->push_back(txid);
            }
        }
        if (out.headers_synced) {
            ++good;
            ++next_peer_;  // the next round asks someone else
        } else {
            peers_.erase(peers_.begin() + next_peer_);  // unreachable, misbehaving or on another chain
        }
    }
    return good;
}

// The slow path: headers one at a time from the servers. Returns whether any
// server answered with a tip.
bool BtcLightBackend::SyncFromServers(BtcHeaderChain& chain, uint32_t now_unix) {
    bool answered = false;
    // One bound for the whole round, however many servers there are.
    const uint32_t limit = chain.TipHeight() + config_.max_server_headers_per_sync;
    for (auto& server : servers_) {
        const auto tip = server->TipHeight();
        if (!tip) continue;
        answered = true;
        if (*tip <= chain.TipHeight() || chain.TipHeight() >= limit) continue;
        const uint32_t last = std::min(*tip, limit);
        // Walk back from our tip + 1 until a header connects (the server may be
        // on a branch that forks a few blocks below our tip), then forward.
        std::vector<BtcBlockHeader> pending;
        uint32_t height = chain.TipHeight() + 1;
        bool connected = false;
        for (int back = 0; back < 12 && height > chain.params().checkpoint_height; ++back, --height) {
            const auto hash = server->BlockHashAt(height);
            const auto header = hash ? server->Header(*hash) : std::nullopt;
            if (!header) break;
            const auto result = chain.Accept(*header, now_unix);
            if (result.status == BtcHeaderChain::Status::Orphan) {
                pending.push_back(*header);
                continue;
            }
            connected = result.status != BtcHeaderChain::Status::Invalid;
            break;
        }
        if (!connected) continue;  // this server's chain does not meet ours, or it sent an invalid header
        bool ok = true;
        for (auto it = pending.rbegin(); it != pending.rend() && ok; ++it) {
            ok = chain.Accept(*it, now_unix).status != BtcHeaderChain::Status::Invalid;
        }
        for (uint32_t h = chain.TipHeight() + 1; ok && h <= last; ++h) {
            const auto hash = server->BlockHashAt(h);
            const auto header = hash ? server->Header(*hash) : std::nullopt;
            if (!header) break;
            const auto status = chain.Accept(*header, now_unix).status;
            ok = status == BtcHeaderChain::Status::Accepted || status == BtcHeaderChain::Status::Duplicate;
        }
    }
    return answered;
}

void BtcLightBackend::Sync(uint32_t now_unix, bool force) {
    std::lock_guard<std::mutex> network(network_mu_);
    if (!synced_once_ && !config_.header_file.empty()) {
        std::lock_guard<std::mutex> lock(chain_mu_);
        LoadBtcHeaderChain(config_.header_file, chain_, now_unix);  // what a previous run already verified
        tip_height_ = chain_.TipHeight();
    }
    if (synced_once_ && !force && now_unix - last_sync_ < config_.min_sync_interval_seconds) return;
    synced_once_ = true;
    last_sync_ = now_unix;
    BtcHeaderChain working = Copy();
    std::string problem;
    try {
        const size_t peers = SyncFromPeers(working, {}, nullptr);
        if (peers == 0 && !SyncFromServers(working, now_unix)) {
            problem = config_.peer_hosts.empty() ? "cannot reach any Bitcoin server"
                                                 : "cannot reach any Bitcoin peer or server";
        }
    } catch (const std::exception& e) {
        problem = std::string("Bitcoin header sync failed: ") + e.what();
    }
    Merge(working, now_unix);
    std::lock_guard<std::mutex> lock(problem_mu_);
    problem_ = problem;
}

std::optional<double> BtcLightBackend::FeeRate(uint32_t target_blocks) {
    std::vector<double> rates;
    for (auto& server : servers_) {
        if (const auto rate = server->FeeRate(target_blocks)) rates.push_back(*rate);
    }
    if (rates.empty()) return std::nullopt;
    // The median: one server cannot push the fee up or down on its own (of
    // two, the higher, so a transaction is not left stuck).
    std::sort(rates.begin(), rates.end());
    return std::max(1.0, rates[rates.size() / 2]);
}

std::string BtcLightBackend::Broadcast(const std::vector<uint8_t>& raw_tx) {
    const auto tx = ParseBtcRawTx(raw_tx);
    if (!tx) throw std::runtime_error("Bitcoin transaction refused: not a transaction");
    const std::string id = BtcHashToHex(tx->txid);

    bool taken = false;       // somebody has it
    bool unsure = false;      // somebody may have it: no answer, or an answer we cannot read
    std::string refusal;
    for (auto& server : servers_) {
        const EsploraBroadcast r = server->Broadcast(raw_tx);
        if (r.accepted || SaysAlreadyKnown(r.error)) {
            taken = true;
        } else if (server->last_error() == EsploraError::NotFound) {
            if (refusal.empty()) refusal = r.error;  // a definite "no" (HTTP 4xx)
        } else {
            unsure = true;
        }
    }
    std::vector<Bytes32> delivered;
    {
        std::lock_guard<std::mutex> network(network_mu_);
        BtcHeaderChain working = Copy();
        try {
            SyncFromPeers(working, {raw_tx}, &delivered);
        } catch (const std::exception&) {
            unsure = true;
        }
        Merge(working, working.Tip().time + kBtcMaxFutureBlockTime);  // headers a peer just gave: already time-checked there
    }
    // A peer that fetched it and answered afterwards has looked at it. It may
    // still have rejected it, so this is "sent", to be confirmed by seeing it.
    if (std::find(delivered.begin(), delivered.end(), tx->txid) != delivered.end()) taken = true;

    if (taken) return id;
    if (!unsure && !refusal.empty()) throw std::runtime_error("Bitcoin transaction refused: " + refusal);
    throw std::runtime_error("Bitcoin transaction may not have been sent: no server or peer confirmed taking it" +
                             (refusal.empty() ? std::string() : " (" + refusal + ")"));
}

}  // namespace dinero::swap
