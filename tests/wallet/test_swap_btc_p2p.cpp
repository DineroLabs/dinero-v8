// The Bitcoin peer protocol for swaps without Bitcoin Core, against a scripted
// peer: handshake, header sync, handing over a transaction, and every way a
// hostile peer can misbehave. No sockets here (see the regtest test for those).

#include "wallet/swap/btc_p2p.h"
#include "wallet/swap/btc_raw_tx.h"

#include "crypto/sha256.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>

using namespace dinero::swap;

namespace {

constexpr uint32_t kNow = 1'900'000'000;
const BtcP2pNetwork kNet = BtcP2pNetwork::Regtest();

using Bytes = std::vector<uint8_t>;

void Le(Bytes& out, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint64_t Le(const Bytes& b, size_t pos, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[pos + i]) << (8 * i);
    return v;
}

struct Message {
    std::string command;
    Bytes payload;
};

// Splits a byte stream we sent into messages, checking the framing on the way.
std::vector<Message> Parse(const Bytes& stream) {
    std::vector<Message> out;
    size_t pos = 0;
    while (pos < stream.size()) {
        EXPECT_GE(stream.size() - pos, 24u);
        EXPECT_TRUE(std::equal(kNet.magic.begin(), kNet.magic.end(), stream.begin() + pos));
        Message m;
        for (size_t i = 0; i < 12 && stream[pos + 4 + i]; ++i) m.command.push_back(static_cast<char>(stream[pos + 4 + i]));
        const size_t length = Le(stream, pos + 16, 4);
        m.payload.assign(stream.begin() + pos + 24, stream.begin() + pos + 24 + length);
        const Bytes again = BtcP2pMessage(kNet, m.command, m.payload);
        EXPECT_TRUE(std::equal(again.begin(), again.end(), stream.begin() + pos)) << m.command;  // checksum included
        out.push_back(m);
        pos += 24 + length;
    }
    return out;
}

Bytes PeerVersion(int32_t version = 70016, uint64_t nonce = 99, int32_t start_height = 4321) {
    Bytes p;
    Le(p, static_cast<uint32_t>(version), 4);
    Le(p, 0x409, 8);  // NETWORK | WITNESS | NETWORK_LIMITED
    Le(p, kNow, 8);
    p.insert(p.end(), 52, 0);
    Le(p, nonce, 8);
    const std::string agent = "/Satoshi:28.0.0/";
    p.push_back(static_cast<uint8_t>(agent.size()));
    p.insert(p.end(), agent.begin(), agent.end());
    Le(p, static_cast<uint32_t>(start_height), 4);
    p.push_back(1);
    return BtcP2pMessage(kNet, "version", p);
}

BtcBlockHeader Mine(const BtcBlockHeader& parent, uint32_t salt = 0) {
    BtcBlockHeader h;
    h.version = 0x20000000;
    h.prev_hash = BtcBlockHash(parent);
    h.merkle_root[0] = static_cast<uint8_t>(salt);
    h.merkle_root[1] = static_cast<uint8_t>(salt >> 8);
    h.time = parent.time + 600;
    h.bits = parent.bits;
    for (h.nonce = 0; !BtcCheckProofOfWork(h, 0x207fffff); ++h.nonce) {
    }
    return h;
}

Bytes HeadersMessage(const std::vector<BtcBlockHeader>& headers, uint8_t tx_count = 0) {
    Bytes p;
    if (headers.size() < 0xfd) {
        p.push_back(static_cast<uint8_t>(headers.size()));
    } else {
        p.push_back(0xfd);
        Le(p, static_cast<uint64_t>(headers.size()), size_t{2});
    }
    for (const auto& h : headers) {
        const auto raw = SerializeBtcBlockHeader(h);
        p.insert(p.end(), raw.begin(), raw.end());
        p.push_back(tx_count);
    }
    return BtcP2pMessage(kNet, "headers", p);
}

// A session past the handshake.
struct Connected {
    BtcHeaderChain chain{BtcChainParams::Regtest()};
    BtcPeerSession session{kNet, chain, 7};
    Connected() {
        session.Start(kNow);
        Feed(PeerVersion());
        Feed(BtcP2pMessage(kNet, "verack", {}));
        EXPECT_TRUE(session.handshake_complete());
    }
    BtcPeerSession::Step Feed(const Bytes& bytes) { return session.OnReceived(bytes.data(), bytes.size(), kNow); }
};

// A peer that knows a chain and answers `getheaders` the way Bitcoin Core does.
struct FakePeer {
    std::vector<BtcBlockHeader> chain;  // chain[0] is the regtest genesis
    explicit FakePeer(size_t length) {
        chain.push_back(BtcChainParams::Regtest().checkpoint);
        while (chain.size() <= length) chain.push_back(Mine(chain.back()));
    }
    Bytes Answer(const Message& getheaders) const {
        EXPECT_EQ(getheaders.command, "getheaders");
        const size_t count = getheaders.payload[4];
        EXPECT_EQ(getheaders.payload.size(), 4 + 1 + 32 * count + 32);
        size_t from = 0;  // the first locator hash we know
        for (size_t i = 0; i < count; ++i) {
            Bytes32 hash;
            std::copy_n(getheaders.payload.begin() + 5 + 32 * i, 32, hash.begin());
            bool found = false;
            for (size_t h = 0; h < chain.size(); ++h) {
                if (BtcBlockHash(chain[h]) == hash) {
                    from = h;
                    found = true;
                }
            }
            if (found) break;
        }
        std::vector<BtcBlockHeader> batch;
        for (size_t h = from + 1; h < chain.size() && batch.size() < kBtcP2pMaxHeaders; ++h) batch.push_back(chain[h]);
        return HeadersMessage(batch);
    }
};

Bytes LegacyTx(uint8_t salt) {
    Bytes tx{1, 0, 0, 0, 1};
    tx.insert(tx.end(), 32, salt);
    tx.insert(tx.end(), {0, 0, 0, 0, 2, 0x51, salt, 0xff, 0xff, 0xff, 0xff, 1, 9, 0, 0, 0, 0, 0, 0, 0, 2, 0x51, 0x52, 0, 0, 0, 0});
    return tx;
}

Bytes GetData(uint32_t type, const Bytes32& hash) {
    Bytes p{1};
    Le(p, type, 4);
    p.insert(p.end(), hash.begin(), hash.end());
    return BtcP2pMessage(kNet, "getdata", p);
}

}  // namespace

TEST(BtcP2p, Handshake) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    BtcPeerSession session(kNet, chain, 0x1122334455667788ULL);
    const auto hello = Parse(session.Start(kNow));
    ASSERT_EQ(hello.size(), 1u);
    EXPECT_EQ(hello[0].command, "version");
    const Bytes& v = hello[0].payload;
    ASSERT_EQ(v.size(), 86u);
    EXPECT_EQ(Le(v, 0, 4), 70015u);
    EXPECT_EQ(Le(v, 4, 8), 0u);                          // we offer no services
    EXPECT_EQ(Le(v, 12, 8), kNow);
    EXPECT_EQ(Le(v, 72, 8), 0x1122334455667788ULL);
    EXPECT_EQ(v[80], 0);                                 // no user agent: nothing to fingerprint
    EXPECT_EQ(Le(v, 81, 4), 0u);                         // our height
    EXPECT_EQ(v[85], 0);                                 // do not relay transactions to us
    EXPECT_TRUE(session.Start(kNow).empty());            // said once

    EXPECT_TRUE(session.RequestHeaders().empty());       // nothing before the handshake
    const Bytes version = PeerVersion();
    auto step = session.OnReceived(version.data(), version.size(), kNow);
    const auto reply = Parse(step.send);
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply[0].command, "verack");
    EXPECT_TRUE(reply[0].payload.empty());
    EXPECT_FALSE(session.handshake_complete());
    EXPECT_EQ(session.peer_start_height(), 4321);
    EXPECT_EQ(session.peer_services(), 0x409u);

    const Bytes verack = BtcP2pMessage(kNet, "verack", {});
    step = session.OnReceived(verack.data(), verack.size(), kNow);
    EXPECT_TRUE(step.send.empty());
    EXPECT_TRUE(session.handshake_complete());
    EXPECT_FALSE(session.fault().has_value());
}

// TCP delivers bytes in any pieces: one at a time must mean the same.
TEST(BtcP2p, ByteAtATime) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    BtcPeerSession session(kNet, chain, 7);
    session.Start(kNow);
    Bytes stream = PeerVersion();
    const Bytes wtxid = BtcP2pMessage(kNet, "wtxidrelay", {});  // newer peers say this before verack
    const Bytes verack = BtcP2pMessage(kNet, "verack", {});
    Bytes ping_payload{1, 2, 3, 4, 5, 6, 7, 8};
    const Bytes ping = BtcP2pMessage(kNet, "ping", ping_payload);
    for (const Bytes* part : {&wtxid, &verack, &ping}) stream.insert(stream.end(), part->begin(), part->end());
    Bytes sent;
    for (uint8_t byte : stream) {
        const auto step = session.OnReceived(&byte, 1, kNow);
        sent.insert(sent.end(), step.send.begin(), step.send.end());
    }
    EXPECT_TRUE(session.handshake_complete());
    const auto messages = Parse(sent);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].command, "verack");
    EXPECT_EQ(messages[1].command, "pong");
    EXPECT_EQ(messages[1].payload, ping_payload);
}

TEST(BtcP2p, SyncsHeadersInBatches) {
    const FakePeer peer(4500);
    Connected c;
    size_t rounds = 0, accepted = 0;
    while (!c.session.headers_synced()) {
        const auto request = Parse(c.session.RequestHeaders());
        ASSERT_EQ(request.size(), 1u);
        EXPECT_TRUE(c.session.awaiting_headers());
        EXPECT_TRUE(c.session.RequestHeaders().empty());  // one request at a time
        const auto step = c.Feed(peer.Answer(request[0]));
        ASSERT_FALSE(c.session.fault().has_value()) << *c.session.fault();
        accepted += step.headers_accepted;
        ASSERT_LT(++rounds, 10u);
    }
    EXPECT_EQ(rounds, 3u);  // 2000 + 2000 + 500
    EXPECT_EQ(accepted, 4500u);
    EXPECT_EQ(c.chain.TipHeight(), 4500u);
    EXPECT_EQ(c.chain.TipHash(), BtcBlockHash(peer.chain.back()));

    // Asking again when nothing is new: an empty answer, still in sync.
    const auto request = Parse(c.session.RequestHeaders());
    const auto step = c.Feed(peer.Answer(request[0]));
    EXPECT_EQ(step.headers_accepted, 0u);
    EXPECT_TRUE(c.session.headers_synced());
}

TEST(BtcP2p, NewBlockAnnouncementsAreTakenToo) {
    const FakePeer peer(3);
    Connected c;
    auto step = c.Feed(HeadersMessage({peer.chain[1]}));  // unrequested
    EXPECT_EQ(step.headers_accepted, 1u);
    step = c.Feed(HeadersMessage({peer.chain[3]}));       // skips one: cannot connect yet, not a fault
    EXPECT_EQ(step.headers_accepted, 0u);
    EXPECT_FALSE(c.session.fault().has_value());
    EXPECT_EQ(c.chain.TipHeight(), 1u);
}

// What we do not use is never stored, however large, and does not disturb what follows.
TEST(BtcP2p, UnusedMessagesStreamPast) {
    Connected c;
    Bytes big(24);
    std::copy(kNet.magic.begin(), kNet.magic.end(), big.begin());
    std::memcpy(big.data() + 4, "inv", 3);
    const uint32_t length = kBtcP2pMaxFrame;
    for (int i = 0; i < 4; ++i) big[16 + i] = static_cast<uint8_t>(length >> (8 * i));
    big[20] = 0xde;  // a checksum that cannot be right: nobody looks
    auto step = c.Feed(big);
    const Bytes chunk(65536, 0xab);
    for (uint32_t fed = 0; fed < length;) {
        const size_t n = std::min<size_t>(chunk.size(), length - fed);
        step = c.session.OnReceived(chunk.data(), n, kNow);
        fed += static_cast<uint32_t>(n);
        ASSERT_FALSE(c.session.fault().has_value());
        ASSERT_TRUE(step.send.empty());
        ASSERT_EQ(c.session.buffered_bytes(), 0u);  // four megabytes went by; none were kept
    }
    Bytes ping_payload(8, 0x5a);
    step = c.Feed(BtcP2pMessage(kNet, "ping", ping_payload));
    const auto reply = Parse(step.send);
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply[0].command, "pong");
    for (const char* command : {"addr", "sendcmpct", "feefilter", "sendheaders", "alert", "block", "tx"}) {
        EXPECT_TRUE(c.Feed(BtcP2pMessage(kNet, command, Bytes(100, 1))).send.empty());
        EXPECT_FALSE(c.session.fault().has_value()) << command;
    }
}

TEST(BtcP2p, HandsOverATransactionOnlyWhenAsked) {
    const Bytes tx = LegacyTx(0x31);
    const Bytes32 txid = ParseBtcRawTx(tx)->txid;

    BtcHeaderChain chain(BtcChainParams::Regtest());
    BtcPeerSession early(kNet, chain, 7);
    early.Start(kNow);
    EXPECT_TRUE(early.AnnounceTx(tx).empty());  // not before the handshake

    Connected c;
    EXPECT_TRUE(c.session.AnnounceTx({1, 2, 3}).empty());  // not a transaction
    const auto inv = Parse(c.session.AnnounceTx(tx));
    ASSERT_EQ(inv.size(), 1u);
    EXPECT_EQ(inv[0].command, "inv");
    ASSERT_EQ(inv[0].payload.size(), 37u);
    EXPECT_EQ(inv[0].payload[0], 1);
    EXPECT_EQ(Le(inv[0].payload, 1, 4), 1u);  // MSG_TX
    EXPECT_TRUE(std::equal(txid.begin(), txid.end(), inv[0].payload.begin() + 5));

    // The announcement alone sends nothing more; the peer has to ask.
    Bytes32 other = txid;
    other[0] ^= 1;
    EXPECT_TRUE(c.Feed(GetData(1, other)).send.empty());       // another transaction
    EXPECT_TRUE(c.Feed(GetData(2, txid)).send.empty());        // asked for as a block
    for (uint32_t type : {1u, 0x40000001u}) {                  // plain and witness requests
        const auto step = c.Feed(GetData(type, txid));
        const auto sent = Parse(step.send);
        ASSERT_EQ(sent.size(), 1u);
        EXPECT_EQ(sent[0].command, "tx");
        EXPECT_EQ(sent[0].payload, tx);                        // byte for byte what we were given
        EXPECT_EQ(step.txs_sent, std::vector<Bytes32>{txid});
    }
    // Announcing the same one again does not queue it twice; the queue is bounded.
    EXPECT_FALSE(c.session.AnnounceTx(tx).empty());
    for (uint8_t i = 1; i < kBtcP2pMaxQueuedTxs; ++i) EXPECT_FALSE(c.session.AnnounceTx(LegacyTx(0x40 + i)).empty()) << int(i);
    EXPECT_TRUE(c.session.AnnounceTx(LegacyTx(0x90)).empty());
    EXPECT_EQ(Parse(c.Feed(GetData(1, txid)).send).size(), 1u);
}

TEST(BtcP2p, ClosesOnAnythingMalformed) {
    const FakePeer peer(5);
    auto fault_after = [&](const Bytes& bytes, bool request_first = false) -> std::string {
        Connected c;
        if (request_first) c.session.RequestHeaders();
        const auto step = c.Feed(bytes);
        if (!c.session.fault()) return "";
        EXPECT_TRUE(step.send.empty());  // nothing is said to a peer being dropped
        // Closed for good: nothing is processed, nothing is said, nothing can be asked.
        EXPECT_TRUE(c.Feed(BtcP2pMessage(kNet, "ping", Bytes(8, 1))).send.empty());
        EXPECT_TRUE(c.session.RequestHeaders().empty());
        EXPECT_TRUE(c.session.AnnounceTx(LegacyTx(1)).empty());
        return *c.session.fault();
    };

    Bytes mainnet = BtcP2pMessage(BtcP2pNetwork::Mainnet(), "ping", Bytes(8, 1));
    EXPECT_EQ(fault_after(mainnet), "wrong network magic");

    Bytes oversized = BtcP2pMessage(kNet, "inv", {});
    const uint32_t too_long = kBtcP2pMaxFrame + 1;
    for (int i = 0; i < 4; ++i) oversized[16 + i] = static_cast<uint8_t>(too_long >> (8 * i));
    EXPECT_EQ(fault_after(oversized), "oversized message");

    Bytes many_headers = BtcP2pMessage(kNet, "headers", {});
    const uint32_t over = 9 + kBtcP2pMaxHeaders * 81 + 1;
    for (int i = 0; i < 4; ++i) many_headers[16 + i] = static_cast<uint8_t>(over >> (8 * i));
    EXPECT_EQ(fault_after(many_headers), "oversized headers");

    Bytes bad_sum = HeadersMessage({peer.chain[1]});
    bad_sum[20] ^= 1;
    EXPECT_EQ(fault_after(bad_sum), "bad checksum");

    Bytes bad_command = BtcP2pMessage(kNet, "ping", Bytes(8, 1));
    bad_command[5] = 'I';  // commands are lower case
    EXPECT_EQ(fault_after(bad_command), "malformed command");
    bad_command[5] = 0;    // a hole in the name
    EXPECT_EQ(fault_after(bad_command), "malformed command");

    EXPECT_EQ(fault_after(HeadersMessage({peer.chain[1]}, /*tx_count=*/1)), "malformed headers");
    Bytes short_count{2};
    const auto one = SerializeBtcBlockHeader(peer.chain[1]);
    short_count.insert(short_count.end(), one.begin(), one.end());
    short_count.push_back(0);
    EXPECT_EQ(fault_after(BtcP2pMessage(kNet, "headers", short_count)), "malformed headers");
    Bytes padded_count{0xfd, 1, 0};
    padded_count.insert(padded_count.end(), one.begin(), one.end());
    padded_count.push_back(0);
    EXPECT_EQ(fault_after(BtcP2pMessage(kNet, "headers", padded_count)), "malformed headers");

    BtcBlockHeader forged = peer.chain[1];
    while (BtcCheckProofOfWork(forged, 0x207fffff)) ++forged.nonce;
    EXPECT_EQ(fault_after(HeadersMessage({forged})), "invalid header: proof of work");

    // An answer to our own request that does not connect: the peer is on another chain.
    EXPECT_EQ(fault_after(HeadersMessage({peer.chain[3]}), /*request_first=*/true), "headers do not connect to our chain");

    EXPECT_EQ(fault_after(PeerVersion()), "second version message");
    EXPECT_EQ(fault_after(BtcP2pMessage(kNet, "verack", {0})), "oversized verack");
    EXPECT_EQ(fault_after(BtcP2pMessage(kNet, "getdata", {1, 2, 3})), "malformed getdata");
    Bytes trailing = GetData(1, Bytes32{});
    trailing = BtcP2pMessage(kNet, "getdata", [&] {
        Bytes p(trailing.begin() + 24, trailing.end());
        p.push_back(0);  // one byte more than its count says
        return p;
    }());
    EXPECT_EQ(fault_after(trailing), "malformed getdata");

    // A fault later in the same read: the reply to the earlier, good message is not sent either.
    Bytes ping_then_garbage = BtcP2pMessage(kNet, "ping", Bytes(8, 1));
    ping_then_garbage.insert(ping_then_garbage.end(), mainnet.begin(), mainnet.end());
    EXPECT_EQ(fault_after(ping_then_garbage), "wrong network magic");
    EXPECT_EQ(fault_after(BtcP2pMessage(kNet, "ping", Bytes(8, 1))), "");  // sanity: a good message is no fault
}

TEST(BtcP2p, HandshakeRules) {
    auto open = [](const Bytes& first) {
        BtcHeaderChain chain(BtcChainParams::Regtest());
        BtcPeerSession session(kNet, chain, 7);
        session.Start(kNow);
        const auto step = session.OnReceived(first.data(), first.size(), kNow);
        EXPECT_TRUE(session.fault().has_value() ? step.send.empty() : true);
        return session.fault().value_or("");
    };
    EXPECT_EQ(open(PeerVersion()), "");
    EXPECT_EQ(open(BtcP2pMessage(kNet, "verack", {})), "verack before version");
    EXPECT_EQ(open(BtcP2pMessage(kNet, "ping", Bytes(8, 1))), "ping before version");
    EXPECT_EQ(open(PeerVersion(60002)), "peer protocol too old");
    EXPECT_EQ(open(PeerVersion(70016, /*nonce=*/7)), "connected to ourselves");
    EXPECT_EQ(open(BtcP2pMessage(kNet, "version", Bytes(20, 0))), "malformed version");

    // Headers before the peer's verack are not acted on.
    BtcHeaderChain chain(BtcChainParams::Regtest());
    BtcPeerSession session(kNet, chain, 7);
    session.Start(kNow);
    Bytes stream = PeerVersion();
    const Bytes headers = HeadersMessage({Mine(chain.Tip())});
    stream.insert(stream.end(), headers.begin(), headers.end());
    session.OnReceived(stream.data(), stream.size(), kNow);
    EXPECT_EQ(session.fault().value_or(""), "headers before the handshake finished");
    EXPECT_EQ(chain.TipHeight(), 0u);
}

// Whatever a peer sends, the session answers and stays within its limits.
TEST(BtcP2p, RandomInputNeverCrashes) {
    std::mt19937 rng(20261118);
    const FakePeer peer(40);
    Bytes transcript = PeerVersion();
    for (const Bytes& part : {BtcP2pMessage(kNet, "verack", {}), BtcP2pMessage(kNet, "ping", Bytes(8, 3)),
                              HeadersMessage({peer.chain.begin() + 1, peer.chain.begin() + 30}),
                              GetData(1, Bytes32{}), BtcP2pMessage(kNet, "addr", Bytes(300, 7))}) {
        transcript.insert(transcript.end(), part.begin(), part.end());
    }
    size_t faults = 0, clean = 0;
    for (int round = 0; round < 20000; ++round) {
        Bytes stream = transcript;
        const int edits = rng() % 4;  // sometimes none: the clean transcript must pass
        for (int e = 0; e < edits; ++e) {
            switch (rng() % 3) {
                case 0: stream[rng() % stream.size()] = static_cast<uint8_t>(rng()); break;
                case 1: stream.resize(rng() % (stream.size() + 1)); break;
                default: stream.insert(stream.begin() + rng() % (stream.size() + 1), static_cast<uint8_t>(rng())); break;
            }
            if (stream.empty()) stream.push_back(0);
        }
        BtcHeaderChain chain(BtcChainParams::Regtest());
        BtcPeerSession session(kNet, chain, 7);
        session.Start(kNow);
        for (size_t pos = 0; pos < stream.size();) {
            const size_t n = std::min<size_t>(1 + rng() % 200, stream.size() - pos);
            const auto step = session.OnReceived(stream.data() + pos, n, kNow);
            EXPECT_LT(step.send.size(), 4096u);
            pos += n;
        }
        EXPECT_LE(chain.TipHeight(), 29u);
        (session.fault() ? faults : clean) += 1;
        if (edits == 0) {
            EXPECT_FALSE(session.fault().has_value());
            EXPECT_EQ(chain.TipHeight(), 29u);
        }
    }
    EXPECT_GT(faults, 1000u);
    EXPECT_GT(clean, 1000u);
}
