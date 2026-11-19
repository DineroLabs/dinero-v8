#pragma once
// Bitcoin peer-to-peer protocol for swaps without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.1).
//
// Two jobs only: fetch block headers from Bitcoin peers into the node's own
// header chain, and hand the network a transaction (above all the claim that
// reveals a swap's secret) with no server in between.
//
// This file is the protocol, not the socket: BtcPeerSession is fed the bytes a
// peer sent and returns the bytes to send back. No I/O, no clock, no threads,
// so every rule can be tested against any byte sequence.
//
// A peer is untrusted. The session
//   - buffers only the few messages it uses (version, verack, ping, headers,
//     getdata), each under a small size limit; every other message is skipped
//     as it streams past, without being stored;
//   - closes on a wrong network magic, an oversized frame, a bad checksum, a
//     malformed message, or a header its chain refuses;
//   - never sends a transaction it was not asked to queue.
// What a peer says about headers is checked by BtcHeaderChain, not here.

#include "wallet/swap/btc_header_chain.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

inline constexpr uint32_t kBtcP2pMaxFrame = 4'000'000;       // Bitcoin's own limit for any message
inline constexpr uint32_t kBtcP2pMaxHeaders = 2000;          // per `headers` message
inline constexpr int32_t kBtcP2pProtocolVersion = 70015;
inline constexpr size_t kBtcP2pMaxQueuedTxs = 16;

struct BtcP2pNetwork {
    std::array<uint8_t, 4> magic{};
    uint16_t default_port{};
    static BtcP2pNetwork Mainnet();
    static BtcP2pNetwork Testnet4();
    static BtcP2pNetwork Regtest();
    // By BtcChainParams::name ("main", "testnet4", "regtest"); throws std::invalid_argument otherwise.
    static BtcP2pNetwork For(const std::string& chain_name);
};

// One framed message: 24-byte header + payload.
std::vector<uint8_t> BtcP2pMessage(const BtcP2pNetwork& net, const std::string& command,
                                   const std::vector<uint8_t>& payload);

class BtcPeerSession {
public:
    // `nonce` identifies this connection (random per session). The chain is the
    // caller's; the session calls Accept on it.
    BtcPeerSession(BtcP2pNetwork net, BtcHeaderChain& chain, uint64_t nonce);

    // The first bytes to send: our `version`.
    std::vector<uint8_t> Start(uint32_t now_unix);

    struct Step {
        std::vector<uint8_t> send;         // bytes to write to the peer (may be empty)
        size_t headers_accepted{0};        // new headers this step
        std::vector<Bytes32> txs_sent;     // transactions the peer asked for and was given (txid, wire order)
    };
    // Feed bytes received from the peer. After a fault the session is closed:
    // further input is ignored and the caller should drop the connection.
    Step OnReceived(const uint8_t* data, size_t size, uint32_t now_unix);

    // Ask for headers after our tip. Empty before the handshake is complete,
    // while a request is outstanding, or once closed.
    std::vector<uint8_t> RequestHeaders();

    // Announce a transaction; the peer fetches it with `getdata`. Empty (and
    // nothing queued) before the handshake, if the bytes are not a
    // transaction, or if too many are queued already.
    std::vector<uint8_t> AnnounceTx(const std::vector<uint8_t>& raw_tx);

    bool handshake_complete() const { return got_version_ && got_verack_; }
    // The peer answered a headers request with fewer than a full batch: it has
    // nothing after our tip.
    bool headers_synced() const { return headers_synced_; }
    bool awaiting_headers() const { return awaiting_headers_; }
    int32_t peer_start_height() const { return peer_start_height_; }
    uint64_t peer_services() const { return peer_services_; }
    // Bytes of a partly received message held in memory right now.
    size_t buffered_bytes() const { return header_.size() + payload_.size(); }
    // Why the session closed; nullopt while it is healthy.
    const std::optional<std::string>& fault() const { return fault_; }

private:
    void Close(std::string why);
    void OnMessage(const std::string& command, const std::vector<uint8_t>& payload, uint32_t now_unix, Step& step);
    void OnHeaders(const std::vector<uint8_t>& payload, uint32_t now_unix, Step& step);
    void OnGetData(const std::vector<uint8_t>& payload, Step& step);
    void Send(Step& step, const std::string& command, const std::vector<uint8_t>& payload);

    BtcP2pNetwork net_;
    BtcHeaderChain& chain_;
    uint64_t nonce_;

    // Framing: a 24-byte header, then `remaining_` payload bytes that are either
    // collected (wanted_) or skipped.
    std::vector<uint8_t> header_;
    std::vector<uint8_t> payload_;
    std::string command_;
    uint32_t remaining_{0};
    bool in_payload_{false};
    bool wanted_{false};
    std::array<uint8_t, 4> checksum_{};

    bool started_{false};
    bool got_version_{false};
    bool got_verack_{false};
    bool awaiting_headers_{false};
    bool headers_synced_{false};
    int32_t peer_start_height_{0};
    uint64_t peer_services_{0};
    std::optional<std::string> fault_;

    struct QueuedTx {
        Bytes32 txid{};
        std::vector<uint8_t> raw;
    };
    std::vector<QueuedTx> queued_;
};

}  // namespace dinero::swap
