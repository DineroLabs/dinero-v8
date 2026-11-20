// The light Bitcoin watcher against a simulated Bitcoin: blocks with real
// Merkle trees, a mempool, and several servers, some of which lie or hide
// things. What reaches the swap engine must be true whatever they say.

#include "wallet/swap/btc_light_deposit.h"
#include "wallet/swap/btc_light_watcher.h"
#include "wallet/swap/btc_tx.h"

#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <functional>
#include <map>

using namespace dinero::swap;

namespace {

constexpr uint32_t kStart = 1'900'000'000;
constexpr uint64_t kAmount = 250'000;
using Bytes = std::vector<uint8_t>;

Bytes32 Sha256(const uint8_t* data, size_t n) {
    Bytes32 out{};
    dinero::crypto::CSHA256().Write(data, n).Finalize(out.data());
    return out;
}
Bytes32 Sha256d(const uint8_t* a, size_t na, const uint8_t* b = nullptr, size_t nb = 0) {
    Bytes32 once{}, twice{};
    dinero::crypto::CSHA256 h;
    h.Write(a, na);
    if (b) h.Write(b, nb);
    h.Finalize(once.data());
    dinero::crypto::CSHA256().Write(once.data(), 32).Finalize(twice.data());
    return twice;
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
Bytes32 TxidOf(const Bytes& raw) { return ParseBtcRawTx(raw)->txid; }

const Bytes32 kSecret = [] {
    Bytes32 s{};
    s.fill(0x5e);
    return s;
}();

BtcHtlcTerms Terms() {
    BtcHtlcTerms t;
    t.payment_hash = Sha256(kSecret.data(), 32);
    t.claim_pubkey.fill(0x02);
    t.refund_pubkey.fill(0x03);
    t.refund_locktime_unix = kStart + 48 * 3600;
    return t;
}
Bytes HtlcScript() { return BtcP2wshScriptPubKey(BuildBtcHtlcWitnessScript(Terms())); }

// A transaction paying `script` `value` (from some unrelated coin).
Bytes PayTo(const Bytes& script, uint64_t value, uint8_t salt) {
    BtcTx tx;
    BtcTxIn in;
    in.prev_txid.fill(salt);
    in.witness = {{0x30, salt}, {0x02, salt}};
    tx.vin.push_back(in);
    tx.vout.push_back({value, script});
    tx.vout.push_back({1000, {0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, salt}});
    return SerializeBtcTx(tx, true);
}
BtcFunding FundingOf(const Bytes& raw) {
    BtcFunding f;
    f.txid = TxidOf(raw);
    f.vout = 0;
    f.value_sat = kAmount;
    return f;
}
const Bytes kPayout{0x00, 0x14, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
Bytes Claim(const Bytes& funding_raw, const Bytes32& preimage = kSecret) {
    BtcTx tx = BuildBtcClaimTx(FundingOf(funding_raw), kPayout, 500);
    // The witness as a claim lays it out: signature, preimage, branch selector, script.
    tx.vin[0].witness = {{0x30, 0x01}, Bytes(preimage.begin(), preimage.end()), {0x01}, BuildBtcHtlcWitnessScript(Terms())};
    return SerializeBtcTx(tx, true);
}
Bytes Refund(const Bytes& funding_raw) {
    BtcTx tx = BuildBtcRefundTx(Terms(), FundingOf(funding_raw), kPayout, 500);
    SetBtcRefundWitness(tx, Terms(), {0x30, 0x01});
    return SerializeBtcTx(tx, true);
}
Bytes Coinbase(uint32_t salt) {
    Bytes tx{1, 0, 0, 0, 1};
    tx.insert(tx.end(), 32, 0);
    tx.insert(tx.end(), {0xff, 0xff, 0xff, 0xff, 4});
    for (int i = 0; i < 4; ++i) tx.push_back(static_cast<uint8_t>(salt >> (8 * i)));
    tx.insert(tx.end(), {0xff, 0xff, 0xff, 0xff, 1, 0, 0xf2, 5, 0x2a, 1, 0, 0, 0, 1, 0x51, 0, 0, 0, 0});
    return tx;
}

// The simulated network: the truth every honest server tells.
struct World {
    struct Block {
        BtcBlockHeader header;
        Bytes32 hash{};
        std::vector<Bytes> txs;  // txs[0] is the coinbase
    };
    BtcHeaderChain truth{BtcChainParams::Regtest()};  // which blocks are on the best chain
    std::map<Bytes32, Block> blocks;
    std::map<Bytes32, Bytes> mempool;
    uint32_t salt{0};

    static std::vector<std::vector<Bytes32>> Levels(const std::vector<Bytes>& txs) {
        std::vector<std::vector<Bytes32>> levels(1);
        for (const auto& raw : txs) levels[0].push_back(TxidOf(raw));
        while (levels.back().size() > 1) {
            const auto& below = levels.back();
            std::vector<Bytes32> above;
            for (size_t i = 0; i < below.size(); i += 2) {
                const Bytes32& right = i + 1 < below.size() ? below[i + 1] : below[i];
                above.push_back(Sha256d(below[i].data(), 32, right.data(), 32));
            }
            levels.push_back(above);
        }
        return levels;
    }

    // Mines a block with `txs` on `parent` (default: the tip). Returns its hash.
    Bytes32 Mine(std::vector<Bytes> txs = {}, std::optional<Bytes32> parent = std::nullopt) {
        const BtcBlockHeader prev = parent ? blocks.count(*parent) ? blocks[*parent].header : truth.params().checkpoint : truth.Tip();
        Block b;
        b.txs.push_back(Coinbase(++salt));
        for (auto& raw : txs) {
            mempool.erase(TxidOf(raw));
            b.txs.push_back(std::move(raw));
        }
        b.header.version = 0x20000000;
        b.header.prev_hash = BtcBlockHash(prev);
        b.header.merkle_root = Levels(b.txs).back()[0];
        b.header.time = std::max(prev.time + 600, kStart);
        b.header.bits = prev.bits;
        for (b.header.nonce = 0; !BtcCheckProofOfWork(b.header, 0x207fffff); ++b.header.nonce) {
        }
        b.hash = BtcBlockHash(b.header);
        EXPECT_EQ(truth.Accept(b.header, b.header.time).status, BtcHeaderChain::Status::Accepted);
        blocks[b.hash] = b;
        return b.hash;
    }
    void MineEmpty(int count) {
        for (int i = 0; i < count; ++i) Mine();
    }
    void Broadcast(const Bytes& raw) { mempool[TxidOf(raw)] = raw; }
    uint32_t Now() const { return truth.Tip().time + 60; }

    // Where a transaction is on the best chain.
    struct Location {
        const Block* block{nullptr};
        size_t index{0};
        uint32_t height{0};
    };
    std::optional<Location> Find(const Bytes32& txid) const {
        for (const auto& [hash, block] : blocks) {
            const auto height = truth.HeightOf(hash);
            if (!height) continue;
            for (size_t i = 0; i < block.txs.size(); ++i) {
                if (TxidOf(block.txs[i]) == txid) return Location{&block, i, *height};
            }
        }
        return std::nullopt;
    }
    std::optional<Bytes> Raw(const Bytes32& txid) const {
        if (const auto at = Find(txid)) return at->block->txs[at->index];
        const auto it = mempool.find(txid);
        return it == mempool.end() ? std::nullopt : std::optional<Bytes>(it->second);
    }
    Json::Value Status(const Bytes32& txid) const {
        Json::Value s(Json::objectValue);
        const auto at = Find(txid);
        s["confirmed"] = at.has_value();
        if (at) {
            s["block_height"] = at->height;
            s["block_hash"] = BtcHashToHex(at->block->hash);
        }
        return s;
    }
    std::vector<Bytes32> AllTxids() const {
        std::vector<Bytes32> out;
        for (const auto& [hash, block] : blocks) {
            if (!truth.HeightOf(hash)) continue;
            for (const auto& raw : block.txs) out.push_back(TxidOf(raw));
        }
        for (const auto& [txid, raw] : mempool) out.push_back(txid);
        return out;
    }

    // What an honest server answers for `path`.
    BtcHttpResponse Serve(const std::string& path) const {
        static const std::string kTx = "/tx/", kScript = "/scripthash/", kBlock = "/block/";
        auto json = [](const Json::Value& v) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            return BtcHttpResponse{200, Json::writeString(w, v)};
        };
        if (path.rfind(kScript, 0) == 0) {
            const std::string want = path.substr(kScript.size(), 64);
            Json::Value list(Json::arrayValue);
            for (const Bytes32& txid : AllTxids()) {
                const auto tx = ParseBtcRawTx(*Raw(txid));
                for (const auto& out : tx->vout) {
                    const Bytes32 digest = Sha256(out.script_pubkey.data(), out.script_pubkey.size());
                    if (Hex(Bytes(digest.begin(), digest.end())) != want) continue;
                    Json::Value entry(Json::objectValue);
                    entry["txid"] = BtcHashToHex(txid);
                    entry["status"] = Status(txid);
                    list.append(entry);
                    break;
                }
            }
            return json(list);
        }
        if (path.rfind(kBlock, 0) == 0) {  // /block/<hash>/txid/0
            const auto hash = BtcHashFromHex(path.substr(kBlock.size(), 64));
            if (!hash || !blocks.count(*hash) || path.substr(kBlock.size() + 64) != "/txid/0") return {404, ""};
            return {200, BtcHashToHex(TxidOf(blocks.at(*hash).txs[0]))};
        }
        if (path.rfind(kTx, 0) != 0) return {404, ""};
        const auto txid = BtcHashFromHex(path.substr(kTx.size(), 64));
        const std::string leaf = path.substr(kTx.size() + 64);
        if (!txid) return {400, ""};
        const auto raw = Raw(*txid);
        if (leaf == "/status") return json(Status(*txid));  // unknown transactions are "unconfirmed", as on the real servers
        if (!raw) return {404, "Transaction not found"};
        if (leaf == "/hex") return {200, Hex(*raw)};
        if (leaf == "/merkle-proof") {
            const auto at = Find(*txid);
            if (!at) return {404, ""};
            const auto levels = Levels(at->block->txs);
            Json::Value proof(Json::objectValue), merkle(Json::arrayValue);
            size_t index = at->index;
            for (size_t l = 0; l + 1 < levels.size(); ++l, index /= 2) {
                const size_t sibling = index ^ 1;
                merkle.append(BtcHashToHex(sibling < levels[l].size() ? levels[l][sibling] : levels[l][index]));
            }
            proof["block_height"] = at->height;
            proof["pos"] = static_cast<Json::UInt>(at->index);
            proof["merkle"] = merkle;
            return json(proof);
        }
        if (leaf.rfind("/outspend/", 0) == 0) {
            const uint32_t vout = static_cast<uint32_t>(std::stoul(leaf.substr(10)));
            Json::Value out(Json::objectValue);
            out["spent"] = false;
            for (const Bytes32& other : AllTxids()) {
                const auto tx = ParseBtcRawTx(*Raw(other));
                for (size_t i = 0; i < tx->vin.size(); ++i) {
                    if (tx->vin[i].prev_txid != *txid || tx->vin[i].prev_vout != vout) continue;
                    out["spent"] = true;
                    out["txid"] = BtcHashToHex(other);
                    out["vin"] = static_cast<Json::UInt>(i);
                    out["status"] = Status(other);
                }
            }
            return json(out);
        }
        return {404, ""};
    }
};

// A server: honest unless `lie` answers for a path.
struct Server {
    const World& world;
    std::function<std::optional<BtcHttpResponse>(const std::string& path)> lie;
    size_t requests{0};
    EsploraServer api;
    explicit Server(const World& w, const std::string& name = "a")
        : world(w), api("https://" + name + ".example/api", Http(name)) {}
    BtcHttp Http(const std::string& name) {
        BtcHttp http;
        const std::string base = "https://" + name + ".example/api";
        http.get = [this, base](const std::string& url) {
            ++requests;
            const std::string path = url.substr(base.size());
            if (lie) {
                if (const auto answer = lie(path)) return *answer;
            }
            return world.Serve(path);
        };
        http.post = [](const std::string&, const std::string&) { return BtcHttpResponse{500, ""}; };
        return http;
    }
};

BtcWatchTarget Target() {
    BtcWatchTarget t;
    t.terms = Terms();
    t.expected_amount_sat = kAmount;
    t.expected_chain = "regtest";
    return t;
}

// The node: its own header chain, fed from the world's best chain.
struct Node {
    BtcHeaderChain chain{BtcChainParams::Regtest()};
    void Sync(const World& world) {
        for (uint32_t h = 1; h <= world.truth.TipHeight(); ++h) chain.Accept(*world.truth.AtHeight(h), world.Now());
    }
};

bool Contains(const std::string& path, const char* part) { return path.find(part) != std::string::npos; }
std::string Compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

}  // namespace

TEST(BtcLightWatcher, LockThenClaimThroughTheirWholeLife) {
    World world;
    world.MineEmpty(12);
    Node node;
    node.Sync(world);
    Server server(world);
    BtcLightWatcher watcher(node.chain, {&server.api}, Target());

    auto r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.ok) << watcher.last_problem();
    EXPECT_FALSE(r.htlc.output_seen);
    EXPECT_FALSE(r.funding.has_value());
    EXPECT_EQ(r.mtp_unix, *node.chain.MedianTimePast());

    // In the mempool only: not a lock yet.
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x11);
    world.Broadcast(funding);
    const size_t unconfirmed = server.requests;
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.htlc.output_seen);
    EXPECT_EQ(server.requests - unconfirmed, 1u);  // listed as unconfirmed: nothing to prove yet, nothing fetched

    world.Mine({funding});
    node.Sync(world);
    r = watcher.Observe(world.Now());
    ASSERT_TRUE(r.ok) << watcher.last_problem();
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_EQ(r.htlc.output_value, kAmount);
    EXPECT_EQ(r.htlc.output_confirmations, 1u);
    EXPECT_FALSE(r.htlc.spent);
    ASSERT_TRUE(r.funding.has_value());
    EXPECT_EQ(r.funding->txid, TxidOf(funding));
    EXPECT_EQ(r.funding->vout, 0u);
    EXPECT_EQ(r.funding->value_sat, kAmount);

    world.MineEmpty(2);
    node.Sync(world);
    const size_t before = server.requests;
    r = watcher.Observe(world.Now());
    EXPECT_EQ(r.htlc.output_confirmations, 3u);
    EXPECT_EQ(server.requests - before, 1u);  // the proven lock is remembered: only "is it spent?" is asked

    // The claim in the mempool: the secret is out, and it proves itself.
    const Bytes claim = Claim(funding);
    world.Broadcast(claim);
    r = watcher.Observe(world.Now());
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_TRUE(r.htlc.spent_by_claim);
    EXPECT_EQ(r.htlc.spend_confirmations, 0u);
    ASSERT_TRUE(r.htlc.revealed_preimage.has_value());
    EXPECT_EQ(*r.htlc.revealed_preimage, kSecret);

    world.Mine({claim});
    node.Sync(world);
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.htlc.spent_by_claim);
    EXPECT_EQ(r.htlc.spend_confirmations, 1u);
    EXPECT_EQ(*r.htlc.revealed_preimage, kSecret);
    world.MineEmpty(5);
    node.Sync(world);
    const size_t settled = server.requests;
    r = watcher.Observe(world.Now());
    EXPECT_EQ(r.htlc.spend_confirmations, 6u);
    EXPECT_EQ(r.htlc.output_confirmations, 9u);
    EXPECT_EQ(server.requests, settled);  // all proven: nothing more to ask anyone
}

TEST(BtcLightWatcher, ARefundCountsOnlyOnceItIsInABlock) {
    World world;
    world.MineEmpty(12);
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x12);
    world.Mine({funding});
    Node node;
    node.Sync(world);
    Server server(world);
    BtcLightWatcher watcher(node.chain, {&server.api}, Target());

    const Bytes refund = Refund(funding);
    world.Broadcast(refund);
    auto r = watcher.Observe(world.Now());
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_FALSE(r.htlc.spent);  // unconfirmed and carries no secret: it could be made up

    world.Mine({refund});
    node.Sync(world);
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_FALSE(r.htlc.spent_by_claim);
    EXPECT_FALSE(r.htlc.revealed_preimage.has_value());
    EXPECT_EQ(r.htlc.spend_confirmations, 1u);
}

TEST(BtcLightWatcher, OnlyTheExactSwapOutputIsTheLock) {
    World world;
    world.MineEmpty(12);
    const Bytes decoy_small = PayTo(HtlcScript(), kAmount - 1, 0x21);
    const Bytes decoy_large = PayTo(HtlcScript(), kAmount + 1, 0x22);
    // The right amount, but to another script, in a transaction that also pays ours the wrong amount.
    BtcTx mixed;
    BtcTxIn mixed_in;
    mixed_in.prev_txid.fill(0x24);
    mixed_in.witness = {{0x30, 0x24}};
    mixed.vin.push_back(mixed_in);
    mixed.vout.push_back({kAmount, kPayout});
    mixed.vout.push_back({kAmount / 2, HtlcScript()});
    world.Mine({decoy_small, decoy_large, SerializeBtcTx(mixed, true)});
    Node node;
    node.Sync(world);
    Server server(world);
    BtcLightWatcher watcher(node.chain, {&server.api}, Target());
    EXPECT_FALSE(watcher.Observe(world.Now()).htlc.output_seen);

    // Once proven not to be the lock, a decoy is not fetched again.
    const size_t first = server.requests;
    EXPECT_FALSE(watcher.Observe(world.Now()).htlc.output_seen);
    EXPECT_EQ(server.requests - first, 1u);

    const Bytes real = PayTo(HtlcScript(), kAmount, 0x23);
    world.Mine({real});
    node.Sync(world);
    const auto r = watcher.Observe(world.Now());
    ASSERT_TRUE(r.htlc.output_seen);
    EXPECT_EQ(r.funding->txid, TxidOf(real));

    // A watcher pinned to one funding transaction sees no other, even of the right amount.
    BtcWatchTarget pinned = Target();
    pinned.expected_funding_txid = std::string(64, 'e');
    BtcLightWatcher other(node.chain, {&server.api}, pinned);
    EXPECT_FALSE(other.Observe(world.Now()).htlc.output_seen);
    pinned.expected_funding_txid = BtcHashToHex(TxidOf(real));
    BtcLightWatcher exact(node.chain, {&server.api}, pinned);
    EXPECT_TRUE(exact.Observe(world.Now()).htlc.output_seen);

    // A lock older than the swap is not the swap's.
    BtcWatchTarget later = Target();
    later.scan_from_height = node.chain.TipHeight() + 1;
    BtcLightWatcher newer(node.chain, {&server.api}, later);
    EXPECT_FALSE(newer.Observe(world.Now()).htlc.output_seen);
}

// A server alone cannot make the engine believe a lock or a spend.
TEST(BtcLightWatcher, InventedLocksAndSpendsAreIgnored) {
    World world;
    world.MineEmpty(12);
    Node node;
    node.Sync(world);
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x31);
    const Bytes32 funding_id = TxidOf(funding);

    // The server claims an unmined funding is confirmed in a real block of our chain.
    Server liar(world, "liar");
    const Bytes32 some_block = BtcBlockHash(*world.truth.AtHeight(5));
    liar.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        const std::string id = BtcHashToHex(funding_id);
        const std::string status = R"({"confirmed":true,"block_height":5,"block_hash":")" + BtcHashToHex(some_block) + "\"}";
        if (Contains(path, "/scripthash/")) return BtcHttpResponse{200, R"([{"txid":")" + id + R"(","status":)" + status + "}]"};
        if (path == "/tx/" + id + "/hex") return BtcHttpResponse{200, Hex(funding)};
        if (path == "/tx/" + id + "/status") return BtcHttpResponse{200, status};
        if (path == "/tx/" + id + "/merkle-proof") return BtcHttpResponse{200, R"({"block_height":5,"pos":1,"merkle":[")" + std::string(64, '7') + "\"]}"};
        return std::nullopt;
    };
    BtcLightWatcher fooled(node.chain, {&liar.api}, Target());
    auto r = fooled.Observe(world.Now());
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.htlc.output_seen);

    // Now the lock is real, and a server invents spends of it.
    world.Mine({funding});
    node.Sync(world);
    const Bytes unrelated = PayTo(kPayout, 5000, 0x32);  // exists, but does not spend the lock
    world.Broadcast(unrelated);
    Bytes32 wrong_secret{};
    wrong_secret.fill(0x77);
    const Bytes fake_claim = Claim(funding, wrong_secret);  // spends the lock on paper, with a secret that is not the secret
    const Bytes real_refund = Refund(funding);              // well-formed, but never broadcast
    // Carries the real secret, as a claim would, but spends some other coin: the lock is not spent.
    const Bytes elsewhere = Claim(PayTo(HtlcScript(), kAmount, 0x33));

    for (const Bytes* invented : {&unrelated, &fake_claim, &real_refund, &elsewhere}) {
        Server inventor(world, "inventor");
        const Bytes32 id = TxidOf(*invented);
        inventor.lie = [&, id](const std::string& path) -> std::optional<BtcHttpResponse> {
            if (Contains(path, "/outspend/")) {
                return BtcHttpResponse{200, R"({"spent":true,"txid":")" + BtcHashToHex(id) + R"(","vin":0,"status":{"confirmed":false}})"};
            }
            if (path == "/tx/" + BtcHashToHex(id) + "/hex") return BtcHttpResponse{200, Hex(*invented)};
            return std::nullopt;
        };
        BtcLightWatcher watcher(node.chain, {&inventor.api}, Target());
        r = watcher.Observe(world.Now());
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(r.htlc.output_seen);
        EXPECT_FALSE(r.htlc.spent) << "an invented spend was believed";
        EXPECT_FALSE(r.htlc.revealed_preimage.has_value());
    }

    // The same refund "confirmed" by the server's word, with a proof that does not hold.
    Server confirmer(world, "confirmer");
    const Bytes32 refund_id = TxidOf(real_refund);
    confirmer.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        const std::string id = BtcHashToHex(refund_id);
        const std::string status = R"({"confirmed":true,"block_height":5,"block_hash":")" + BtcHashToHex(some_block) + "\"}";
        if (Contains(path, "/outspend/")) return BtcHttpResponse{200, R"({"spent":true,"txid":")" + id + R"(","vin":0,"status":)" + status + "}"};
        if (path == "/tx/" + id + "/hex") return BtcHttpResponse{200, Hex(real_refund)};
        if (path == "/tx/" + id + "/status") return BtcHttpResponse{200, status};
        if (path == "/tx/" + id + "/merkle-proof") return BtcHttpResponse{200, R"({"block_height":5,"pos":1,"merkle":[")" + std::string(64, '7') + "\"]}"};
        return std::nullopt;
    };
    BtcLightWatcher watcher(node.chain, {&confirmer.api}, Target());
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_FALSE(r.htlc.spent);
}

// Hiding is the other lie. One honest server is enough.
TEST(BtcLightWatcher, OneHonestServerIsEnough) {
    World world;
    world.MineEmpty(12);
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x41);
    world.Mine({funding});
    Node node;
    node.Sync(world);
    const Bytes claim = Claim(funding);
    world.Broadcast(claim);

    Server hider1(world, "h1"), hider2(world, "h2"), honest(world, "ok");
    for (Server* s : {&hider1, &hider2}) {
        s->lie = [](const std::string& path) -> std::optional<BtcHttpResponse> {
            if (Contains(path, "/scripthash/")) return BtcHttpResponse{200, "[]"};
            if (Contains(path, "/outspend/")) return BtcHttpResponse{200, R"({"spent":false})"};
            return std::nullopt;
        };
    }
    BtcLightWatcher blind(node.chain, {&hider1.api, &hider2.api}, Target());
    auto r = blind.Observe(world.Now());
    EXPECT_TRUE(r.ok);                 // they answered; they just hid everything
    EXPECT_FALSE(r.htlc.output_seen);

    for (const auto& order : {std::vector<EsploraServer*>{&hider1.api, &hider2.api, &honest.api},
                              std::vector<EsploraServer*>{&honest.api, &hider1.api, &hider2.api}}) {
        BtcLightWatcher watcher(node.chain, order, Target());
        r = watcher.Observe(world.Now());
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(r.htlc.output_seen);
        EXPECT_TRUE(r.htlc.spent_by_claim);
        ASSERT_TRUE(r.htlc.revealed_preimage.has_value());
        EXPECT_EQ(*r.htlc.revealed_preimage, kSecret);
    }

    // The server that reports the claim refuses to hand it over: another one does.
    Server tease(world, "tease");
    tease.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        if (path == "/tx/" + BtcHashToHex(TxidOf(claim)) + "/hex") return BtcHttpResponse{404, ""};
        return std::nullopt;
    };
    BtcLightWatcher watcher(node.chain, {&tease.api, &honest.api}, Target());
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.htlc.spent_by_claim);
}

TEST(BtcLightWatcher, AReorgTakesTheLockAwayAndBringsItBack) {
    World world;
    world.MineEmpty(12);
    const Bytes32 fork_point = world.truth.TipHash();
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x51);
    world.Mine({funding});
    Node node;
    node.Sync(world);
    Server server(world);
    BtcLightWatcher watcher(node.chain, {&server.api}, Target());
    ASSERT_TRUE(watcher.Observe(world.Now()).htlc.output_seen);

    // A longer branch without the lock replaces its block.
    Bytes32 tip = world.Mine({}, fork_point);
    tip = world.Mine({}, tip);
    world.Broadcast(funding);  // back in the mempool
    node.Sync(world);
    auto r = watcher.Observe(world.Now());
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.htlc.output_seen);  // an engine waiting for N confirmations starts over

    world.Mine({funding});
    node.Sync(world);
    r = watcher.Observe(world.Now());
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_EQ(r.htlc.output_confirmations, 1u);
}

TEST(BtcLightWatcher, SaysNothingOnAStaleOrPartialView) {
    World world;
    world.MineEmpty(12);
    const Bytes funding = PayTo(HtlcScript(), kAmount, 0x61);
    world.Mine({funding});
    Node node;
    node.Sync(world);
    Server a(world, "a"), b(world, "b");

    // Our headers are behind the clock.
    BtcLightWatcher watcher(node.chain, {&a.api}, Target());
    auto r = watcher.Observe(world.Now() + 2 * 3600 + 1);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.htlc.output_seen);
    EXPECT_NE(watcher.last_problem().find("headers are behind"), std::string::npos) << watcher.last_problem();
    EXPECT_TRUE(watcher.Observe(world.Now() + 2 * 3600 - 120).ok);
    EXPECT_EQ(watcher.last_problem(), "");

    // Servers down: with nobody answering, and with fewer than required.
    BtcLightWatchConfig two;
    two.min_servers = 2;
    BtcLightWatcher strict(node.chain, {&a.api, &b.api}, Target(), two);
    EXPECT_TRUE(strict.Observe(world.Now()).ok);  // both answer; the lock is proven and remembered
    b.lie = [](const std::string&) { return std::optional<BtcHttpResponse>(BtcHttpResponse{0, ""}); };
    r = strict.Observe(world.Now());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(strict.last_problem(), "only 1 of the 2 Bitcoin servers needed answered");
    a.lie = [](const std::string&) { return std::optional<BtcHttpResponse>(BtcHttpResponse{503, ""}); };
    r = strict.Observe(world.Now());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(strict.last_problem(), "cannot reach any Bitcoin server");
    a.lie = nullptr;
    b.lie = nullptr;
    EXPECT_TRUE(strict.Observe(world.Now()).ok);

    // Headers just above a checkpoint: the median time is not known yet.
    BtcChainParams later = BtcChainParams::Regtest();
    BtcHeaderChain full(later);
    BtcBlockHeader tip = full.Tip();
    for (uint32_t i = 0; i < kBtcRetargetInterval; ++i) {
        BtcBlockHeader h;
        h.version = 0x20000000;
        h.prev_hash = BtcBlockHash(tip);
        h.time = std::max(tip.time + 600, kStart);
        h.bits = tip.bits;
        for (h.nonce = 0; !BtcCheckProofOfWork(h, 0x207fffff); ++h.nonce) {
        }
        ASSERT_EQ(full.Accept(h, h.time).status, BtcHeaderChain::Status::Accepted);
        tip = h;
    }
    later.checkpoint_height = kBtcRetargetInterval;
    later.checkpoint = tip;
    const BtcHeaderChain young(later);
    BtcLightWatcher early(young, {&a.api}, Target());
    EXPECT_FALSE(early.Observe(tip.time + 60).ok);
    EXPECT_EQ(early.last_problem(), "Bitcoin headers are still syncing");
}

TEST(BtcLightWatcher, RefusesAnotherNetworksChain) {
    World world;
    Node node;
    Server server(world);
    BtcWatchTarget mainnet = Target();
    mainnet.expected_chain = "main";
    EXPECT_THROW(BtcLightWatcher(node.chain, {&server.api}, mainnet), std::invalid_argument);
    EXPECT_THROW(BtcLightWatcher(node.chain, {}, Target()), std::invalid_argument);
    EXPECT_THROW(BtcLightWatcher(node.chain, {nullptr}, Target()), std::invalid_argument);
    BtcWatchTarget bad = Target();
    bad.expected_funding_txid = "xyz";
    EXPECT_THROW(BtcLightWatcher(node.chain, {&server.api}, bad), std::invalid_argument);
}

// ---- The deposit address a buyer pays ------------------------------------------

namespace {
const Bytes kDepositScript{0x00, 0x14, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

// Spends output 0 of `payment` (some coin of the deposit) to elsewhere.
Bytes SpendOf(const Bytes& payment, uint8_t salt) {
    BtcTx tx;
    BtcTxIn in;
    in.prev_txid = TxidOf(payment);
    in.prev_vout = 0;
    in.witness = {{0x30, salt}, {0x02, salt}};
    tx.vin.push_back(in);
    tx.vout.push_back({1000, kPayout});
    return SerializeBtcTx(tx, true);
}
}  // namespace

TEST(BtcDepositWatcher, CountsOnlyProvenUnspentPayments) {
    World world;
    world.MineEmpty(12);
    Node node;
    node.Sync(world);
    Server server(world);
    BtcDepositWatcher watcher(node.chain, {&server.api}, kDepositScript);

    auto v = watcher.Observe();
    EXPECT_TRUE(v.ok);
    EXPECT_EQ(v.seen_sat, 0u);
    EXPECT_TRUE(v.coins.empty());

    // In the mempool: not counted. One confirmation: seen. Two: usable for the lock.
    const Bytes first = PayTo(kDepositScript, 700'000, 0x71);
    world.Broadcast(first);
    EXPECT_EQ(watcher.Observe().seen_sat, 0u);
    EXPECT_FALSE(watcher.Observe().ever_paid);  // nothing proven yet
    EXPECT_EQ(watcher.Observe().arriving_sat, 700'000u);  // but it can be said to be on its way
    EXPECT_TRUE(watcher.Observe().coins.empty());         // and is nothing a payment may spend
    world.Mine({first});
    node.Sync(world);
    v = watcher.Observe();
    EXPECT_EQ(v.arriving_sat, 0u);  // arrived: counted once, as a balance
    EXPECT_TRUE(v.ever_paid);
    EXPECT_EQ(v.seen_sat, 700'000u);
    EXPECT_EQ(v.confirmed_sat, 0u);
    EXPECT_EQ(v.newest_confirmations, 1u);
    ASSERT_EQ(v.coins.size(), 1u);
    EXPECT_EQ(v.coins[0].txid, TxidOf(first));
    EXPECT_EQ(v.coins[0].vout, 0u);
    EXPECT_TRUE(v.deep_coins.empty());

    const Bytes second = PayTo(kDepositScript, 450'000, 0x72);
    world.Mine({second});
    node.Sync(world);
    v = watcher.Observe();
    EXPECT_EQ(v.seen_sat, 1'150'000u);
    EXPECT_EQ(v.confirmed_sat, 700'000u);   // the first is two deep, the second one
    EXPECT_EQ(v.newest_confirmations, 1u);
    world.MineEmpty(1);
    node.Sync(world);
    const size_t before = server.requests;
    v = watcher.Observe();
    EXPECT_EQ(v.confirmed_sat, 1'150'000u);
    EXPECT_EQ(v.deep_coins.size(), 2u);
    EXPECT_EQ(server.requests - before, 3u);  // the list and "spent?" for each coin: nothing is proven twice

    // A coin that is spent (even unconfirmed) is no longer ours to spend.
    world.Broadcast(SpendOf(first, 0x73));
    v = watcher.Observe();
    EXPECT_EQ(v.seen_sat, 450'000u);
    ASSERT_EQ(v.coins.size(), 1u);
    EXPECT_EQ(v.coins[0].txid, TxidOf(second));

    // Both spent: nothing held, but the address was used (the built-in wallet
    // must not hand it out again, and looks past it when restoring).
    world.Broadcast(SpendOf(second, 0x74));
    v = watcher.Observe();
    EXPECT_EQ(v.seen_sat, 0u);
    EXPECT_TRUE(v.coins.empty());
    EXPECT_TRUE(v.ever_paid);
    // A fresh watcher (a restore) learns the same from the chain.
    BtcDepositWatcher restored(node.chain, {&server.api}, kDepositScript);
    v = restored.Observe();
    EXPECT_EQ(v.seen_sat, 0u);
    EXPECT_TRUE(v.ever_paid);
}

TEST(BtcDepositWatcher, ServersCannotInflateOrHideADeposit) {
    World world;
    world.MineEmpty(12);
    const Bytes real = PayTo(kDepositScript, 500'000, 0x74);
    world.Mine({real});
    world.MineEmpty(1);
    Node node;
    node.Sync(world);
    const Bytes32 block5 = BtcBlockHash(*world.truth.AtHeight(5));

    // A payment that was never mined, "confirmed" by the server's word and a made-up proof.
    const Bytes fake = PayTo(kDepositScript, 9'000'000, 0x75);
    Server inflater(world, "inflater");
    inflater.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        const std::string id = BtcHashToHex(TxidOf(fake)), real_id = BtcHashToHex(TxidOf(real));
        const std::string status = R"({"confirmed":true,"block_height":5,"block_hash":")" + BtcHashToHex(block5) + "\"}";
        if (Contains(path, "/scripthash/")) {
            return BtcHttpResponse{200, R"([{"txid":")" + id + R"(","status":)" + status + R"(},{"txid":")" + real_id +
                                            R"(","status":)" + Compact(world.Status(TxidOf(real))) + "}]"};
        }
        if (path == "/tx/" + id + "/hex") return BtcHttpResponse{200, Hex(fake)};
        if (path == "/tx/" + id + "/status") return BtcHttpResponse{200, status};
        if (path == "/tx/" + id + "/merkle-proof") return BtcHttpResponse{200, R"({"block_height":5,"pos":1,"merkle":[")" + std::string(64, '7') + "\"]}"};
        return std::nullopt;
    };
    BtcDepositWatcher watcher(node.chain, {&inflater.api}, kDepositScript);
    auto v = watcher.Observe();
    EXPECT_TRUE(v.ok);
    EXPECT_EQ(v.seen_sat, 500'000u);  // the real payment only

    // "Spent", with no transaction to show for it, or one that spends something else.
    Server spoiler(world, "spoiler");
    const Bytes unrelated = PayTo(kPayout, 1000, 0x76);
    world.Broadcast(unrelated);
    spoiler.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        if (Contains(path, "/outspend/")) {
            return BtcHttpResponse{200, R"({"spent":true,"txid":")" + BtcHashToHex(TxidOf(unrelated)) + R"(","vin":0,"status":{"confirmed":false}})"};
        }
        return std::nullopt;
    };
    BtcDepositWatcher spoiled(node.chain, {&spoiler.api}, kDepositScript);
    EXPECT_EQ(spoiled.Observe().seen_sat, 500'000u);

    // A server that hides the deposit, next to one that does not.
    Server hider(world, "hider"), honest(world, "honest");
    hider.lie = [](const std::string& path) -> std::optional<BtcHttpResponse> {
        if (Contains(path, "/scripthash/")) return BtcHttpResponse{200, "[]"};
        return std::nullopt;
    };
    BtcDepositWatcher blind(node.chain, {&hider.api}, kDepositScript);
    EXPECT_EQ(blind.Observe().seen_sat, 0u);
    BtcDepositWatcher both(node.chain, {&hider.api, &honest.api}, kDepositScript);
    EXPECT_EQ(both.Observe().seen_sat, 500'000u);

    // Nobody answers: nothing is known, and that is said.
    Server down(world, "down");
    down.lie = [](const std::string&) { return std::optional<BtcHttpResponse>(BtcHttpResponse{0, ""}); };
    BtcDepositWatcher silent(node.chain, {&down.api}, kDepositScript);
    EXPECT_FALSE(silent.Observe().ok);
    EXPECT_THROW(BtcDepositWatcher(node.chain, {}, kDepositScript), std::invalid_argument);
    EXPECT_THROW(BtcDepositWatcher(node.chain, {&honest.api}, {}), std::invalid_argument);
}

// Giving coins back does not need proof: a payment below the chain's checkpoint
// cannot be proven, and an invented one only makes a sweep the network refuses.
TEST(BtcDepositWatcher, RecoveryAlsoSeesWhatCannotBeProven) {
    World world;
    world.MineEmpty(12);
    const Bytes old_payment = PayTo(kDepositScript, 300'000, 0x78);
    world.Mine({old_payment});
    world.MineEmpty(1);
    const Bytes spent_payment = PayTo(kDepositScript, 90'000, 0x79);
    world.Mine({spent_payment});
    world.Broadcast(SpendOf(spent_payment, 0x7a));
    const Bytes unconfirmed = PayTo(kDepositScript, 50'000, 0x7b);
    world.Broadcast(unconfirmed);
    Server server(world);

    // A node whose header chain does not reach those blocks (it starts later, or is behind).
    Node behind;  // knows only the genesis block
    BtcDepositWatcher watcher(behind.chain, {&server.api}, kDepositScript);
    auto proven = watcher.Observe();
    EXPECT_TRUE(proven.ok);
    EXPECT_EQ(proven.seen_sat, 0u);  // nothing provable: nothing a swap may be funded from

    auto recovery = watcher.ObserveForRecovery();
    EXPECT_TRUE(recovery.ok);
    EXPECT_EQ(recovery.seen_sat, 300'000u);  // the unspent, confirmed payment; not the spent one, not the unconfirmed one
    ASSERT_EQ(recovery.coins.size(), 1u);
    EXPECT_EQ(recovery.coins[0].txid, TxidOf(old_payment));
    EXPECT_TRUE(recovery.deep_coins.empty());  // never offered as funding
    EXPECT_EQ(recovery.confirmed_sat, 0u);

    // The bytes must still be the transaction named, and must pay our script.
    Server liar(world, "liar");
    liar.lie = [&](const std::string& path) -> std::optional<BtcHttpResponse> {
        if (path == "/tx/" + BtcHashToHex(TxidOf(old_payment)) + "/hex") return BtcHttpResponse{200, Hex(PayTo(kDepositScript, 9'000'000, 0x7c))};
        return std::nullopt;
    };
    BtcDepositWatcher fooled(behind.chain, {&liar.api}, kDepositScript);
    EXPECT_EQ(fooled.ObserveForRecovery().seen_sat, 0u);
    BtcDepositWatcher with_honest(behind.chain, {&liar.api, &server.api}, kDepositScript);
    EXPECT_EQ(with_honest.ObserveForRecovery().seen_sat, 300'000u);

    Server down(world, "down");
    down.lie = [](const std::string&) { return std::optional<BtcHttpResponse>(BtcHttpResponse{0, ""}); };
    BtcDepositWatcher silent(behind.chain, {&down.api}, kDepositScript);
    EXPECT_FALSE(silent.ObserveForRecovery().ok);
}

TEST(BtcDepositWatcher, AReorgUncountsAPayment) {
    World world;
    world.MineEmpty(12);
    const Bytes32 fork_point = world.truth.TipHash();
    const Bytes payment = PayTo(kDepositScript, 300'000, 0x77);
    world.Mine({payment});
    Node node;
    node.Sync(world);
    Server server(world);
    BtcDepositWatcher watcher(node.chain, {&server.api}, kDepositScript);
    EXPECT_EQ(watcher.Observe().seen_sat, 300'000u);
    Bytes32 tip = world.Mine({}, fork_point);
    world.Mine({}, tip);
    node.Sync(world);
    EXPECT_EQ(watcher.Observe().seen_sat, 0u);
}
