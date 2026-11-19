#include "wallet/swap/btc_p2p.h"

#include "crypto/sha256.h"
#include "wallet/swap/btc_raw_tx.h"

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {
namespace {

std::array<uint8_t, 4> Checksum(const uint8_t* data, size_t size) {
    uint8_t once[32], twice[32];
    crypto::CSHA256().Write(data, size).Finalize(once);
    crypto::CSHA256().Write(once, 32).Finalize(twice);
    return {twice[0], twice[1], twice[2], twice[3]};
}

void PutLe(std::vector<uint8_t>& out, uint64_t v, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint64_t GetLe(const uint8_t* p, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// Bitcoin's CompactSize at `pos`; false if truncated or not minimally encoded.
bool ReadCompact(const std::vector<uint8_t>& data, size_t& pos, uint64_t& value) {
    if (pos >= data.size()) return false;
    const uint8_t first = data[pos++];
    if (first < 0xfd) {
        value = first;
        return true;
    }
    const size_t width = first == 0xfd ? 2 : first == 0xfe ? 4 : 8;
    if (data.size() - pos < width) return false;
    value = GetLe(data.data() + pos, width);
    pos += width;
    return value >= (width == 2 ? 0xfdULL : width == 4 ? 0x10000ULL : 0x100000000ULL);
}

// The largest payload we will hold for a message we use; 0: not one of ours.
uint32_t WantedLimit(const std::string& command) {
    if (command == "version") return 2000;
    if (command == "verack") return 0xffffffff;  // must be empty: checked when handled
    if (command == "ping") return 64;
    if (command == "pong") return 64;
    if (command == "headers") return 9 + kBtcP2pMaxHeaders * 81;
    if (command == "getdata") return 9 + 1000 * 36;
    return 0;
}

constexpr uint32_t kInvTx = 1;
constexpr uint32_t kInvWitnessTx = 0x40000001;

}  // namespace

BtcP2pNetwork BtcP2pNetwork::Mainnet() { return {{0xf9, 0xbe, 0xb4, 0xd9}, 8333}; }
BtcP2pNetwork BtcP2pNetwork::Testnet4() { return {{0x1c, 0x16, 0x3f, 0x28}, 48333}; }
BtcP2pNetwork BtcP2pNetwork::Regtest() { return {{0xfa, 0xbf, 0xb5, 0xda}, 18444}; }

BtcP2pNetwork BtcP2pNetwork::For(const std::string& chain_name) {
    if (chain_name == "main") return Mainnet();
    if (chain_name == "testnet4") return Testnet4();
    if (chain_name == "regtest") return Regtest();
    throw std::invalid_argument("no Bitcoin P2P network named " + chain_name);
}

std::vector<uint8_t> BtcP2pMessage(const BtcP2pNetwork& net, const std::string& command,
                                   const std::vector<uint8_t>& payload) {
    if (command.empty() || command.size() > 12) throw std::invalid_argument("bad Bitcoin P2P command");
    if (payload.size() > kBtcP2pMaxFrame) throw std::invalid_argument("Bitcoin P2P payload too large");
    std::vector<uint8_t> out(net.magic.begin(), net.magic.end());
    out.insert(out.end(), command.begin(), command.end());
    out.insert(out.end(), 12 - command.size(), 0);
    PutLe(out, payload.size(), 4);
    const auto sum = Checksum(payload.data(), payload.size());
    out.insert(out.end(), sum.begin(), sum.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

BtcPeerSession::BtcPeerSession(BtcP2pNetwork net, BtcHeaderChain& chain, uint64_t nonce)
    : net_(net), chain_(chain), nonce_(nonce) {}

std::vector<uint8_t> BtcPeerSession::Start(uint32_t now_unix) {
    if (started_) return {};
    started_ = true;
    std::vector<uint8_t> p;
    PutLe(p, static_cast<uint32_t>(kBtcP2pProtocolVersion), 4);
    PutLe(p, 0, 8);         // services: none (we serve nothing)
    PutLe(p, now_unix, 8);
    p.insert(p.end(), 26, 0);  // receiver address: unused
    p.insert(p.end(), 26, 0);  // sender address: unused
    PutLe(p, nonce_, 8);
    p.push_back(0);         // empty user agent
    PutLe(p, chain_.TipHeight(), 4);
    p.push_back(0);         // relay: do not send us other people's transactions
    return BtcP2pMessage(net_, "version", p);
}

void BtcPeerSession::Close(std::string why) {
    if (!fault_) fault_ = std::move(why);
    header_.clear();
    payload_.clear();
    queued_.clear();
    receipts_.clear();
    awaiting_headers_ = false;
}

void BtcPeerSession::Send(Step& step, const std::string& command, const std::vector<uint8_t>& payload) {
    const auto message = BtcP2pMessage(net_, command, payload);
    step.send.insert(step.send.end(), message.begin(), message.end());
}

BtcPeerSession::Step BtcPeerSession::OnReceived(const uint8_t* data, size_t size, uint32_t now_unix) {
    Step step;
    size_t pos = 0;
    while (pos < size && !fault_) {
        if (!in_payload_) {
            const size_t take = std::min(size - pos, 24 - header_.size());
            header_.insert(header_.end(), data + pos, data + pos + take);
            pos += take;
            if (header_.size() < 24) break;
            if (!std::equal(net_.magic.begin(), net_.magic.end(), header_.begin())) {
                Close("wrong network magic");
                break;
            }
            // The command: letters and digits, then zero padding to twelve bytes.
            size_t length = 0;
            while (length < 12 && header_[4 + length] != 0) ++length;
            bool clean = length > 0;
            for (size_t i = 0; i < 12; ++i) {
                const uint8_t c = header_[4 + i];
                clean = clean && (i < length ? (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') : c == 0);
            }
            if (!clean) {
                Close("malformed command");
                break;
            }
            command_.assign(header_.begin() + 4, header_.begin() + 4 + length);
            remaining_ = static_cast<uint32_t>(GetLe(header_.data() + 16, 4));
            std::copy(header_.begin() + 20, header_.begin() + 24, checksum_.begin());
            header_.clear();
            if (remaining_ > kBtcP2pMaxFrame) {
                Close("oversized message");
                break;
            }
            const uint32_t limit = WantedLimit(command_);
            wanted_ = limit != 0;
            if (wanted_ && remaining_ > (command_ == "verack" ? 0 : limit)) {
                Close("oversized " + command_);
                break;
            }
            payload_.clear();
            in_payload_ = true;
        }
        const size_t take = std::min<size_t>(size - pos, remaining_);
        if (wanted_) payload_.insert(payload_.end(), data + pos, data + pos + take);
        pos += take;
        remaining_ -= static_cast<uint32_t>(take);
        if (remaining_ != 0) break;
        in_payload_ = false;
        if (!wanted_) continue;  // skipped unseen: its checksum cannot be checked and does not matter
        if (Checksum(payload_.data(), payload_.size()) != checksum_) {
            Close("bad checksum");
            break;
        }
        OnMessage(command_, payload_, now_unix, step);
        payload_.clear();
    }
    if (fault_) {
        step.send.clear();  // nothing more is said to a peer we are dropping
    }
    return step;
}

void BtcPeerSession::OnMessage(const std::string& command, const std::vector<uint8_t>& payload, uint32_t now_unix,
                               Step& step) {
    if (command == "version") {
        if (got_version_) return Close("second version message");
        if (payload.size() < 46) return Close("malformed version");
        const auto version = static_cast<int32_t>(GetLe(payload.data(), 4));
        if (version < 70001) return Close("peer protocol too old");
        peer_services_ = GetLe(payload.data() + 4, 8);
        // nonce at 72..80, then the user agent and the start height.
        if (payload.size() >= 80 && GetLe(payload.data() + 72, 8) == nonce_) return Close("connected to ourselves");
        size_t pos = 80;
        uint64_t agent = 0;
        if (payload.size() > 80 && ReadCompact(payload, pos, agent) && agent <= payload.size() - pos &&
            payload.size() - pos - agent >= 4) {
            peer_start_height_ = static_cast<int32_t>(GetLe(payload.data() + pos + agent, 4));
        }
        got_version_ = true;
        Send(step, "verack", {});
        return;
    }
    if (!got_version_) return Close(command + " before version");
    if (command == "verack") {
        got_verack_ = true;
        return;
    }
    if (command == "ping") {
        if (payload.size() == 8) Send(step, "pong", payload);  // pre-BIP31 pings carry nothing and expect nothing
        return;
    }
    if (!handshake_complete()) return Close(command + " before the handshake finished");
    if (command == "headers") return OnHeaders(payload, now_unix, step);
    if (command == "getdata") return OnGetData(payload, step);
    if (command == "pong") return OnPong(payload, step);
}

void BtcPeerSession::OnHeaders(const std::vector<uint8_t>& payload, uint32_t now_unix, Step& step) {
    size_t pos = 0;
    uint64_t count = 0;
    if (!ReadCompact(payload, pos, count) || count > kBtcP2pMaxHeaders || payload.size() - pos != count * 81) {
        return Close("malformed headers");
    }
    // Peers also announce new blocks with unrequested `headers`; those are welcome.
    const bool requested = awaiting_headers_;
    awaiting_headers_ = false;
    for (uint64_t i = 0; i < count; ++i, pos += 81) {
        if (payload[pos + 80] != 0) return Close("malformed headers");  // the transaction count is always zero
        const auto header = ParseBtcBlockHeader({payload.begin() + pos, payload.begin() + pos + 80});
        const auto result = chain_.Accept(*header, now_unix);
        switch (result.status) {
            case BtcHeaderChain::Status::Accepted:
                ++step.headers_accepted;
                break;
            case BtcHeaderChain::Status::Duplicate:
                break;
            case BtcHeaderChain::Status::Orphan:
                // An announcement we cannot connect yet is normal; an answer to
                // our own request that does not connect is a peer on another chain.
                if (requested) return Close("headers do not connect to our chain");
                return;
            case BtcHeaderChain::Status::Invalid:
                return Close("invalid header: " + result.reason);
        }
    }
    if (requested) headers_synced_ = count < kBtcP2pMaxHeaders;
}

void BtcPeerSession::OnGetData(const std::vector<uint8_t>& payload, Step& step) {
    size_t pos = 0;
    uint64_t count = 0;
    if (!ReadCompact(payload, pos, count) || count > 1000 || payload.size() - pos != count * 36) {
        return Close("malformed getdata");
    }
    for (uint64_t i = 0; i < count; ++i, pos += 36) {
        const auto type = static_cast<uint32_t>(GetLe(payload.data() + pos, 4));
        if (type != kInvTx && type != kInvWitnessTx) continue;
        for (const QueuedTx& tx : queued_) {
            if (!std::equal(tx.txid.begin(), tx.txid.end(), payload.begin() + pos + 4)) continue;
            if (receipts_.size() >= 4 * kBtcP2pMaxQueuedTxs) continue;  // a peer asking over and over gets no more
            Send(step, "tx", tx.raw);
            // The receipt: the peer answers this ping only after it has handled the transaction.
            const uint64_t ping = nonce_ ^ (0x9e3779b97f4a7c15ULL * ++next_ping_);
            std::vector<uint8_t> payload8;
            PutLe(payload8, ping, 8);
            Send(step, "ping", payload8);
            receipts_.emplace_back(ping, tx.txid);
        }
    }
}

void BtcPeerSession::OnPong(const std::vector<uint8_t>& payload, Step& step) {
    if (payload.size() != 8) return;
    const uint64_t ping = GetLe(payload.data(), 8);
    for (auto it = receipts_.begin(); it != receipts_.end(); ++it) {
        if (it->first != ping) continue;
        step.txs_sent.push_back(it->second);
        receipts_.erase(it);
        return;
    }
}

std::vector<uint8_t> BtcPeerSession::RequestHeaders() {
    if (fault_ || !handshake_complete() || awaiting_headers_) return {};
    std::vector<uint8_t> p;
    PutLe(p, static_cast<uint32_t>(kBtcP2pProtocolVersion), 4);
    const auto locator = chain_.Locator();
    p.push_back(static_cast<uint8_t>(locator.size()));  // at most a few dozen entries
    for (const Bytes32& hash : locator) p.insert(p.end(), hash.begin(), hash.end());
    p.insert(p.end(), 32, 0);  // no stop hash: as many as the peer will give
    awaiting_headers_ = true;
    headers_synced_ = false;
    return BtcP2pMessage(net_, "getheaders", p);
}

std::vector<uint8_t> BtcPeerSession::AnnounceTx(const std::vector<uint8_t>& raw_tx) {
    if (fault_ || !handshake_complete()) return {};
    const auto tx = ParseBtcRawTx(raw_tx);
    if (!tx) return {};
    const bool known = std::any_of(queued_.begin(), queued_.end(), [&](const QueuedTx& q) { return q.txid == tx->txid; });
    if (!known) {
        if (queued_.size() >= kBtcP2pMaxQueuedTxs) return {};
        queued_.push_back({tx->txid, raw_tx});
    }
    std::vector<uint8_t> p{1};
    PutLe(p, kInvTx, 4);
    p.insert(p.end(), tx->txid.begin(), tx->txid.end());
    return BtcP2pMessage(net_, "inv", p);
}

}  // namespace dinero::swap
