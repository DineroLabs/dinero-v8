// The node's shared light view of Bitcoin: header sync from peers and, when no
// peer can be reached, from servers; the header file across restarts; and who
// has to take a transaction for it to count as sent. The network is scripted.

#include "wallet/swap/btc_light_backend.h"
#include "wallet/swap/btc_raw_tx.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>

using namespace dinero::swap;

namespace {

constexpr uint32_t kStart = 1'900'000'000;
using Bytes = std::vector<uint8_t>;

BtcBlockHeader Mine(const BtcBlockHeader& parent, uint32_t salt = 0) {
    BtcBlockHeader h;
    h.version = 0x20000000;
    h.prev_hash = BtcBlockHash(parent);
    h.merkle_root[0] = static_cast<uint8_t>(salt);
    h.time = std::max(parent.time + 600, kStart);
    h.bits = parent.bits;
    for (h.nonce = 0; !BtcCheckProofOfWork(h, 0x207fffff); ++h.nonce) {
    }
    return h;
}
std::string Hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}
Bytes Tx(uint8_t salt) {
    Bytes tx{1, 0, 0, 0, 1};
    tx.insert(tx.end(), 32, salt);
    tx.insert(tx.end(), {0, 0, 0, 0, 1, 0x51, 0xff, 0xff, 0xff, 0xff, 1, 9, 0, 0, 0, 0, 0, 0, 0, 2, 0x51, 0x52, 0, 0, 0, 0});
    return tx;
}

// The scripted network.
struct Net {
    std::vector<BtcBlockHeader> best;  // best[0] is the regtest genesis; what honest sources serve
    Net() { best.push_back(BtcChainParams::Regtest().checkpoint); }
    void Grow(size_t n, uint32_t salt = 0) {
        for (size_t i = 0; i < n; ++i) best.push_back(Mine(best.back(), salt));
    }
    uint32_t Now() const { return best.back().time + 60; }

    // Peers.
    bool peers_reachable{true};
    size_t conversations{0};
    std::vector<Bytes> handed_to_peers;
    bool peers_take_txs{true};
    // Servers, by host name ("a", "b").
    std::map<std::string, bool> server_up{{"a", true}, {"b", true}};
    std::map<std::string, BtcHttpResponse> post_reply;  // default: accept
    std::vector<std::pair<std::string, std::string>> posts;
    std::map<std::string, size_t> gets;

    BtcLightTransport Transport() {
        BtcLightTransport t;
        t.resolve = [](const std::vector<std::string>& hosts, uint16_t port, size_t) {
            std::vector<BtcPeerEndpoint> out;
            for (const auto& h : hosts) {
                for (int i = 1; i <= 4; ++i) out.push_back({h + "-" + std::to_string(i), port});
            }
            return out;
        };
        t.talk = [this](const BtcPeerEndpoint&, const BtcP2pNetwork&, BtcHeaderChain& chain, const std::vector<Bytes>& txs) {
            ++conversations;
            BtcPeerOutcome out;
            if (!peers_reachable) {
                out.error = "timed out connecting";
                return out;
            }
            out.connected = out.handshake = true;
            for (size_t h = 1; h < best.size(); ++h) {
                if (chain.Accept(best[h], Now()).status == BtcHeaderChain::Status::Accepted) ++out.headers_accepted;
            }
            out.headers_synced = true;
            for (const auto& raw : txs) {
                handed_to_peers.push_back(raw);
                if (peers_take_txs) out.txs_sent.push_back(ParseBtcRawTx(raw)->txid);
            }
            return out;
        };
        t.http.get = [this](const std::string& url) -> BtcHttpResponse {
            const std::string host = url.substr(8, 1);  // https://a.example/api...
            ++gets[host];
            if (!server_up[host]) return {0, ""};
            const std::string path = url.substr(std::string("https://a.example/api").size());
            if (path == "/blocks/tip/height") return {200, std::to_string(best.size() - 1)};
            if (path.rfind("/block-height/", 0) == 0) {
                const size_t h = std::stoul(path.substr(14));
                if (h >= best.size()) return {404, ""};
                return {200, BtcHashToHex(BtcBlockHash(best[h]))};
            }
            if (path.rfind("/block/", 0) == 0) {
                const auto hash = BtcHashFromHex(path.substr(7, 64));
                for (const auto& header : best) {
                    if (hash && BtcBlockHash(header) == *hash) return {200, Hex(SerializeBtcBlockHeader(header))};
                }
            }
            return {404, ""};
        };
        t.http.post = [this](const std::string& url, const std::string& body) -> BtcHttpResponse {
            const std::string host = url.substr(8, 1);
            posts.emplace_back(host, body);
            if (!server_up[host]) return {0, ""};
            if (post_reply.count(host)) return post_reply[host];
            Bytes raw;
            for (size_t i = 0; i < body.size(); i += 2) raw.push_back(static_cast<uint8_t>(std::stoul(body.substr(i, 2), nullptr, 16)));
            return {200, BtcHashToHex(ParseBtcRawTx(raw)->txid)};
        };
        return t;
    }
};

BtcLightBackendConfig Config(bool with_peers = true) {
    BtcLightBackendConfig c;
    c.params = BtcChainParams::Regtest();
    c.server_urls = {"https://a.example/api", "https://b.example/api"};
    if (with_peers) c.peer_hosts = {"seed"};
    return c;
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name)
        : path((std::filesystem::temp_directory_path() / ("swap_btc_light_" + name)).string()) {
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".tmp");
    }
};

std::string BroadcastError(BtcLightBackend& backend, const Bytes& raw) {
    try {
        backend.Broadcast(raw);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST(BtcLightBackend, HeadersComeFromPeers) {
    Net net;
    net.Grow(40);
    BtcLightBackend backend(Config(), net.Transport());
    EXPECT_EQ(backend.tip_height(), 0u);
    backend.Sync(net.Now());
    EXPECT_EQ(backend.chain().TipHeight(), 40u);
    EXPECT_EQ(backend.tip_height(), 40u);
    EXPECT_EQ(backend.problem(), "");
    EXPECT_EQ(net.conversations, 2u);         // two peers per round
    EXPECT_EQ(net.gets["a"] + net.gets["b"], 0u);  // with peers answering, servers are not asked for headers

    // Not again right away; again after the interval, or when forced.
    const uint32_t synced_at = net.Now();
    net.Grow(3);
    backend.Sync(synced_at + 10);
    EXPECT_EQ(backend.chain().TipHeight(), 40u);
    EXPECT_EQ(net.conversations, 2u);
    backend.Sync(synced_at + 10, /*force=*/true);
    EXPECT_EQ(backend.chain().TipHeight(), 43u);
    net.Grow(1);
    backend.Sync(synced_at + 10 + 44);
    EXPECT_EQ(backend.chain().TipHeight(), 43u);
    backend.Sync(synced_at + 10 + 45);
    EXPECT_EQ(backend.chain().TipHeight(), 44u);
}

TEST(BtcLightBackend, ServersCarryHeadersWhenNoPeerCanBeReached) {
    Net net;
    net.Grow(30);
    net.peers_reachable = false;  // port 8333 blocked
    BtcLightBackend backend(Config(), net.Transport());
    backend.Sync(net.Now());
    EXPECT_EQ(backend.chain().TipHeight(), 30u);
    EXPECT_EQ(backend.problem(), "");
    EXPECT_GT(net.gets["a"], 30u);

    // A reorg three blocks deep, seen only through the servers.
    net.best.resize(28);
    net.Grow(6, /*salt=*/9);  // heights 28..33 on a new branch
    backend.Sync(net.Now(), true);
    EXPECT_EQ(backend.chain().TipHeight(), 33u);
    EXPECT_EQ(backend.chain().TipHash(), BtcBlockHash(net.best.back()));

    // Servers only, by configuration: the same path.
    Net quiet;
    quiet.Grow(5);
    BtcLightBackend servers_only(Config(/*with_peers=*/false), quiet.Transport());
    servers_only.Sync(quiet.Now());
    EXPECT_EQ(servers_only.chain().TipHeight(), 5u);
    EXPECT_EQ(quiet.conversations, 0u);

    // The slow path is bounded per round.
    Net far;
    far.Grow(400);
    BtcLightBackendConfig c = Config(false);
    BtcLightBackend bounded(c, far.Transport());
    bounded.Sync(far.Now());
    EXPECT_EQ(bounded.chain().TipHeight(), c.max_server_headers_per_sync);
    bounded.Sync(far.Now(), true);
    EXPECT_EQ(bounded.chain().TipHeight(), 2 * c.max_server_headers_per_sync);
}

TEST(BtcLightBackend, SaysWhenNobodyAnswers) {
    Net net;
    net.Grow(5);
    net.peers_reachable = false;
    net.server_up["a"] = net.server_up["b"] = false;
    BtcLightBackend backend(Config(), net.Transport());
    backend.Sync(net.Now());
    EXPECT_EQ(backend.chain().TipHeight(), 0u);
    EXPECT_EQ(backend.problem(), "cannot reach any Bitcoin peer or server");
    net.server_up["b"] = true;  // one server back: enough
    backend.Sync(net.Now(), true);
    EXPECT_EQ(backend.chain().TipHeight(), 5u);
    EXPECT_EQ(backend.problem(), "");
}

TEST(BtcLightBackend, HeaderFileSurvivesRestart) {
    TempFile file("headers");
    Net net;
    net.Grow(25);
    BtcLightBackendConfig c = Config();
    c.header_file = file.path;
    {
        BtcLightBackend backend(c, net.Transport());
        backend.Sync(net.Now());
        EXPECT_EQ(backend.chain().TipHeight(), 25u);
    }
    ASSERT_TRUE(std::filesystem::exists(file.path));
    // After a restart with the whole network down, what was verified before is still known.
    net.peers_reachable = false;
    net.server_up["a"] = net.server_up["b"] = false;
    BtcLightBackend restarted(c, net.Transport());
    restarted.Sync(net.Now());
    EXPECT_EQ(restarted.chain().TipHeight(), 25u);
    EXPECT_EQ(restarted.tip_height(), 25u);
    EXPECT_EQ(restarted.problem(), "cannot reach any Bitcoin peer or server");
}

TEST(BtcLightBackend, ATransactionIsSentWhenSomebodyTakesIt) {
    const Bytes tx = Tx(0x21);
    const std::string id = BtcHashToHex(ParseBtcRawTx(tx)->txid);
    {   // Everybody takes it: servers and peers are all given it.
        Net net;
        net.Grow(3);
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(backend.Broadcast(tx), id);
        ASSERT_EQ(net.posts.size(), 2u);
        EXPECT_EQ(net.posts[0].second, Hex(tx));
        EXPECT_EQ(net.handed_to_peers.size(), 2u);
        EXPECT_EQ(backend.chain().TipHeight(), 3u);  // the peers brought headers along
    }
    {   // Only one server takes it.
        Net net;
        net.peers_reachable = false;
        net.server_up["a"] = false;
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(backend.Broadcast(tx), id);
    }
    {   // No server answers, a peer takes it: no server was in the path.
        Net net;
        net.server_up["a"] = net.server_up["b"] = false;
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(backend.Broadcast(tx), id);
    }
    {   // A server that already has it (in its mempool, or in a block) is a success.
        Net net;
        net.peers_reachable = false;
        net.post_reply["a"] = {400, R"(sendrawtransaction RPC error: {"code":-27,"message":"Transaction already in block chain"})"};
        net.post_reply["b"] = {400, R"(sendrawtransaction RPC error: {"code":-25,"message":"txn-already-known"})"};
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(backend.Broadcast(tx), id);
    }
}

TEST(BtcLightBackend, ARefusalIsToldApartFromSilence) {
    const Bytes tx = Tx(0x22);
    const BtcHttpResponse refused{400, R"(sendrawtransaction RPC error: {"code":-26,"message":"min relay fee not met"})"};
    {   // Every server says no, and no peer took it: it did not leave.
        Net net;
        net.peers_take_txs = false;
        net.post_reply["a"] = net.post_reply["b"] = refused;
        BtcLightBackend backend(Config(), net.Transport());
        const std::string error = BroadcastError(backend, tx);
        EXPECT_EQ(error.rfind("Bitcoin transaction refused: HTTP 400", 0), 0u) << error;
        EXPECT_NE(error.find("min relay fee not met"), std::string::npos);
    }
    {   // One says no, one does not answer: it may have left through the silent one.
        Net net;
        net.peers_reachable = false;
        net.post_reply["a"] = refused;
        net.server_up["b"] = false;
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(BroadcastError(backend, tx).rfind("Bitcoin transaction may not have been sent", 0), 0u);
    }
    {   // Nobody answers at all.
        Net net;
        net.peers_reachable = false;
        net.server_up["a"] = net.server_up["b"] = false;
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(BroadcastError(backend, tx).rfind("Bitcoin transaction may not have been sent", 0), 0u);
    }
    {   // A server that answers with another transaction's id is not an acceptance.
        Net net;
        net.peers_reachable = false;
        net.post_reply["a"] = net.post_reply["b"] = {200, std::string(64, 'c')};
        BtcLightBackend backend(Config(), net.Transport());
        EXPECT_EQ(BroadcastError(backend, tx).rfind("Bitcoin transaction may not have been sent", 0), 0u);
    }
    Net net;
    BtcLightBackend backend(Config(), net.Transport());
    EXPECT_EQ(BroadcastError(backend, {1, 2, 3}), "Bitcoin transaction refused: not a transaction");
    EXPECT_TRUE(net.posts.empty());
    EXPECT_TRUE(net.handed_to_peers.empty());
}

// One server cannot move the fee on its own.
TEST(BtcLightBackend, FeeRateIsTheMedianOfTheServers) {
    std::map<std::string, std::string> fees{{"a", R"({"2":4.0})"}, {"b", R"({"2":9.0})"}, {"c", R"({"2":400.0})"}};
    BtcLightTransport t;
    t.http.get = [&](const std::string& url) -> BtcHttpResponse {
        const std::string host = url.substr(8, 1);
        return fees[host].empty() ? BtcHttpResponse{0, ""} : BtcHttpResponse{200, fees[host]};
    };
    t.http.post = [](const std::string&, const std::string&) { return BtcHttpResponse{500, ""}; };
    BtcLightBackendConfig c = Config(/*with_peers=*/false);
    c.server_urls = {"https://a.example/api", "https://b.example/api", "https://c.example/api"};
    BtcLightBackend backend(c, t);
    EXPECT_EQ(backend.FeeRate(2), 9.0);    // not the 400 one server asks for
    fees["c"] = R"({"2":0.01})";
    EXPECT_EQ(backend.FeeRate(2), 4.0);    // nor the 0.01 one offers
    fees["b"].clear();                     // two left: the higher, so nothing is left stuck
    EXPECT_EQ(backend.FeeRate(2), 4.0);
    fees["a"] = R"({"2":0.122})";          // the real servers' sub-1 rates: never below 1
    EXPECT_EQ(backend.FeeRate(2), 1.0);
    fees["a"].clear();
    fees["c"].clear();
    EXPECT_FALSE(backend.FeeRate(2).has_value());
}

TEST(BtcLightBackend, RefusesAConfigurationThatCannotWork) {
    Net net;
    BtcLightBackendConfig none = Config();
    none.server_urls.clear();
    EXPECT_THROW(BtcLightBackend(none, net.Transport()), std::invalid_argument);
    BtcLightBackendConfig plain = Config();
    plain.server_urls = {"http://mempool.space/api"};
    EXPECT_THROW(BtcLightBackend(plain, net.Transport()), std::invalid_argument);
    BtcLightBackendConfig many = Config();
    many.min_servers = 3;  // two configured
    EXPECT_THROW(BtcLightBackend(many, net.Transport()), std::invalid_argument);
    BtcLightBackendConfig unknown = Config();
    unknown.params.name = "signet";
    EXPECT_THROW(BtcLightBackend(unknown, net.Transport()), std::invalid_argument);
    BtcLightTransport no_peers = net.Transport();
    no_peers.talk = nullptr;
    EXPECT_THROW(BtcLightBackend(Config(), no_peers), std::invalid_argument);
    EXPECT_NO_THROW(BtcLightBackend(Config(false), no_peers));
    BtcLightBackend ok(Config(), net.Transport());
    EXPECT_EQ(ok.config().peer_port, 18444);  // the network's default
    EXPECT_EQ(ok.servers().size(), 2u);
}
