// The public-server client for swaps without Bitcoin Core: a real mainnet
// transaction with its real proof (block 973,000, as two servers served it)
// proven into the node's own header chain, and what happens when a server lies.

#include "wallet/swap/btc_esplora.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <fstream>
#include <iterator>
#include <map>

using namespace dinero::swap;

namespace {

constexpr uint32_t kNow = 1'900'000'000;
const char* kBase = "https://btc.example/api";

std::string ReadFixture(const std::string& name) {
    std::ifstream in(std::string(DINERO_SWAP_FIXTURES) + "/" + name, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture " + name);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

Bytes32 Hash(const std::string& hex) { return *BtcHashFromHex(hex); }

// A server that answers from a table; everything else is 404.
struct FakeServer {
    std::map<std::string, BtcHttpResponse> pages;  // by path
    std::vector<std::string> asked;
    std::vector<std::string> posted;
    BtcHttpResponse post_reply{200, ""};
    Json::Value fx;

    FakeServer() {
        Json::CharReaderBuilder b;
        std::string errors;
        std::istringstream in(ReadFixture("btc_esplora_mainnet_973000.json"));
        if (!Json::parseFromStream(b, in, &fx, &errors)) throw std::runtime_error(errors);
        const std::string tx = fx["txid"].asString(), cb = fx["coinbase_txid"].asString(), block = fx["block_hash"].asString();
        Set("/tx/" + tx + "/hex", fx["tx_hex"].asString());
        Set("/tx/" + tx + "/status", Compact(fx["status"]));
        Set("/tx/" + tx + "/merkle-proof", Compact(fx["proof"]));
        Set("/tx/" + tx + "/outspend/0", Compact(fx["outspend"]));
        Set("/block/" + block + "/txid/0", cb);
        Set("/block/" + block + "/header", fx["header_hex"].asString());
        Set("/tx/" + cb + "/hex", fx["coinbase_hex"].asString());
        Set("/tx/" + cb + "/merkle-proof", Compact(fx["coinbase_proof"]));
        Set("/scripthash/" + fx["scripthash"].asString() + "/txs", Compact(fx["scripthash_txs"]));
        Set("/block-height/973000", block);
        Set("/blocks/tip/height", "976513");
    }
    void Set(const std::string& path, const std::string& body, int status = 200) { pages[path] = {status, body}; }
    std::string TxPath(const char* leaf) const { return "/tx/" + fx["txid"].asString() + leaf; }
    Bytes32 Txid() const { return Hash(fx["txid"].asString()); }

    BtcHttp Http() {
        BtcHttp http;
        http.get = [this](const std::string& url) -> BtcHttpResponse {
            EXPECT_EQ(url.rfind(kBase, 0), 0u) << url;
            const std::string path = url.substr(std::string(kBase).size());
            asked.push_back(path);
            const auto it = pages.find(path);
            return it == pages.end() ? BtcHttpResponse{404, "not found"} : it->second;
        };
        http.post = [this](const std::string& url, const std::string& body) {
            EXPECT_EQ(url, std::string(kBase) + "/tx");
            posted.push_back(body);
            return post_reply;
        };
        return http;
    }
};

// The node's own chain: the mainnet checkpoint plus `count` real headers after it.
BtcHeaderChain Chain(size_t count = 2029) {
    BtcHeaderChain chain(BtcChainParams::Mainnet());
    const std::string headers = ReadFixture("btc_headers_main_971713.bin");
    EXPECT_EQ(chain.LoadBestChain({headers.begin(), headers.begin() + 80 * count}, kNow), count);
    return chain;
}

// What FetchProvenTx says with one answer of the server changed.
std::string Refusal(const std::function<void(FakeServer&)>& lie, size_t headers = 2029) {
    FakeServer fake;
    lie(fake);
    EsploraServer server(kBase, fake.Http());
    const BtcHeaderChain chain = Chain(headers);
    std::string why;
    const auto proven = FetchProvenTx(server, chain, fake.Txid(), why);
    EXPECT_EQ(proven.has_value(), why.empty());
    return why;
}

}  // namespace

TEST(BtcEsplora, ARealMainnetTransactionProvesIntoOurChain) {
    FakeServer fake;
    EsploraServer server(kBase, fake.Http());
    const BtcHeaderChain chain = Chain();
    std::string why;
    const auto proven = FetchProvenTx(server, chain, fake.Txid(), why);
    ASSERT_TRUE(proven.has_value()) << why;
    EXPECT_EQ(proven->height, 973000u);
    EXPECT_EQ(BtcHashToHex(proven->block_hash), fake.fx["block_hash"].asString());
    EXPECT_EQ(proven->confirmations, 973741u - 973000u + 1u);
    EXPECT_EQ(proven->tx.txid, fake.Txid());   // computed from the bytes: a segwit transaction
    EXPECT_TRUE(proven->tx.has_witness);
    // What a swap watcher reads from it: the output's script and amount.
    ASSERT_FALSE(proven->tx.vout.empty());
    std::string spk;
    for (uint8_t b : proven->tx.vout[0].script_pubkey) {
        static const char* d = "0123456789abcdef";
        spk += d[b >> 4];
        spk += d[b & 15];
    }
    EXPECT_EQ(spk, fake.fx["spk0_hex"].asString());
    EXPECT_EQ(proven->tx.vout[0].value_sat, fake.fx["value0"].asUInt64());
    // The block's own header, as the server gives it, is the one in our chain.
    EXPECT_EQ(server.Header(proven->block_hash), chain.AtHeight(973000));
}

TEST(BtcEsplora, WhatDoesNotProveIsNotReturned) {
    EXPECT_EQ(Refusal([](FakeServer&) {}), "");

    // Another (real) transaction's bytes under this id.
    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/hex"), f.fx["spend_hex"].asString()); }),
              "transaction: the server sent a different transaction");
    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/hex"), "00ff"); }), "transaction: not a transaction");

    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/status"), R"({"confirmed":false})"); }), "unconfirmed");

    // "Confirmed", in a block our chain has never heard of (a made-up hash, or a real one we have not reached).
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value s = f.fx["status"];
                  s["block_hash"] = std::string(64, 'a');
                  f.Set(f.TxPath("/status"), Compact(s));
              }),
              "its block is not on our header chain");
    EXPECT_EQ(Refusal([](FakeServer&) {}, /*headers=*/1000), "its block is not on our header chain");  // our chain ends at 972,712

    // A real block of our chain, but not the one the transaction is in.
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value s = f.fx["status"];
                  s["block_hash"] = f.fx["outspend"]["status"]["block_hash"];  // block 973,551
                  f.Set(f.TxPath("/status"), Compact(s));
              }),
              "the server's block height does not match our chain");
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value s = f.fx["status"];
                  s["block_hash"] = f.fx["outspend"]["status"]["block_hash"];
                  s["block_height"] = f.fx["outspend"]["status"]["block_height"];
                  f.Set(f.TxPath("/status"), Compact(s));
              }),
              "the proof is for another block");

    // The proof itself.
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value p = f.fx["proof"];
                  std::string sibling = p["merkle"][3].asString();
                  sibling[0] = sibling[0] == 'a' ? 'b' : 'a';
                  p["merkle"][3] = sibling;
                  f.Set(f.TxPath("/merkle-proof"), Compact(p));
              }),
              "proof does not reach the block's Merkle root");
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value p = f.fx["proof"];
                  p["pos"] = 4;
                  f.Set(f.TxPath("/merkle-proof"), Compact(p));
              }),
              "proof does not reach the block's Merkle root");
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  Json::Value p = f.fx["proof"];
                  Json::Value shorter(Json::arrayValue);
                  for (Json::ArrayIndex i = 0; i + 1 < p["merkle"].size(); ++i) shorter.append(p["merkle"][i]);
                  p["merkle"] = shorter;
                  f.Set(f.TxPath("/merkle-proof"), Compact(p));
              }),
              "proof depth differs from the coinbase's");

    // The coinbase that pins the depth.
    EXPECT_EQ(Refusal([](FakeServer& f) {
                  f.Set("/block/" + f.fx["block_hash"].asString() + "/txid/0", f.fx["txid"].asString());
              }),
              "coinbase: not a coinbase transaction");

    // A server that stops answering part way.
    EXPECT_EQ(Refusal([](FakeServer& f) { f.pages.erase(f.TxPath("/hex")); }), "transaction: not found");
    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/merkle-proof"), "", 503); }), "proof: server unreachable");
    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/status"), "", 0); }), "status: server unreachable");
    EXPECT_EQ(Refusal([](FakeServer& f) { f.Set(f.TxPath("/status"), "{", 200); }), "status: malformed answer");
    EXPECT_EQ(Refusal([](FakeServer& f) { f.pages.erase("/tx/" + f.fx["coinbase_txid"].asString() + "/hex"); }),
              "coinbase: not found");
}

TEST(BtcEsplora, AnswersAreParsedStrictly) {
    FakeServer fake;
    EsploraServer server(kBase, fake.Http());
    const Bytes32 txid = fake.Txid();

    EXPECT_EQ(server.TipHeight(), 976513u);
    for (const char* bad : {"", "abc", "-1", "12 34", "1e3", "9999999999", "0x10"}) {
        fake.Set("/blocks/tip/height", bad);
        EXPECT_FALSE(server.TipHeight().has_value()) << bad;
        EXPECT_EQ(server.last_error(), EsploraError::Malformed) << bad;
    }
    fake.Set("/blocks/tip/height", " 976513\n");
    EXPECT_EQ(server.TipHeight(), 976513u);
    EXPECT_EQ(server.last_error(), EsploraError::None);

    EXPECT_EQ(server.BlockHashAt(973000), Hash(fake.fx["block_hash"].asString()));
    fake.Set("/block-height/973000", "zz");
    EXPECT_FALSE(server.BlockHashAt(973000).has_value());
    EXPECT_FALSE(server.BlockHashAt(5).has_value());
    EXPECT_EQ(server.last_error(), EsploraError::NotFound);

    // A header is known by its hash: a real header of another block is not accepted for this one.
    const Bytes32 block = Hash(fake.fx["block_hash"].asString());
    ASSERT_TRUE(server.Header(block).has_value());
    const auto other = SerializeBtcBlockHeader(BtcChainParams::Mainnet().checkpoint);
    std::string other_hex;
    for (uint8_t b : other) {
        static const char* d = "0123456789abcdef";
        other_hex += d[b >> 4];
        other_hex += d[b & 15];
    }
    fake.Set("/block/" + fake.fx["block_hash"].asString() + "/header", other_hex);
    EXPECT_FALSE(server.Header(block).has_value());
    EXPECT_EQ(server.last_error(), EsploraError::Malformed);

    // Status and proof shapes.
    const char* statuses[] = {"[]", R"({"confirmed":"yes"})", R"({"confirmed":true})",
                              R"({"confirmed":true,"block_height":-5,"block_hash":"00"})",
                              R"({"confirmed":true,"block_height":973000,"block_hash":"00"})",
                              R"({"confirmed":true,"block_height":973000.5,"block_hash":"0000000000000000000000000000000000000000000000000000000000000001"})",
                              R"({"confirmed":true,"confirmed":false})", R"({"confirmed":false} trailing)"};
    for (const char* bad : statuses) {
        fake.Set(fake.TxPath("/status"), bad);
        EXPECT_FALSE(server.TxStatus(txid).has_value()) << bad;
    }
    fake.Set(fake.TxPath("/status"), R"({"confirmed":false})");
    ASSERT_TRUE(server.TxStatus(txid).has_value());
    EXPECT_FALSE(server.TxStatus(txid)->confirmed);

    Json::Value deep_proof = fake.fx["proof"];
    for (int i = 0; i < 12; ++i) deep_proof["merkle"].append(std::string(64, '1'));  // 24 steps: more than any block has
    fake.Set(fake.TxPath("/merkle-proof"), Compact(deep_proof));
    EXPECT_FALSE(server.MerkleProof(txid).has_value());
    for (const char* bad : {R"({"block_height":1,"pos":0})", R"({"block_height":1,"pos":0,"merkle":["00"]})",
                            R"({"block_height":1,"pos":-1,"merkle":[]})", R"({"block_height":1,"pos":0,"merkle":[5]})"}) {
        fake.Set(fake.TxPath("/merkle-proof"), bad);
        EXPECT_FALSE(server.MerkleProof(txid).has_value()) << bad;
    }

    // Size and nesting limits.
    fake.Set(fake.TxPath("/hex"), std::string(kEsploraMaxBody + 1, '0'));
    EXPECT_FALSE(server.RawTx(txid).has_value());
    EXPECT_EQ(server.last_error(), EsploraError::Malformed);
    // Valid JSON padded past the limit: refused for its size alone.
    fake.Set(fake.TxPath("/status"), R"({"confirmed":false})" + std::string(kEsploraMaxBody, ' '));
    EXPECT_FALSE(server.TxStatus(txid).has_value());
    EXPECT_EQ(server.last_error(), EsploraError::Malformed);
    fake.Set(fake.TxPath("/status"), std::string(5000, '[') + std::string(5000, ']'));
    EXPECT_FALSE(server.TxStatus(txid).has_value());
}

TEST(BtcEsplora, ScriptHistoryAndWhoSpentAnOutput) {
    FakeServer fake;
    EsploraServer server(kBase, fake.Http());
    std::vector<uint8_t> script;
    const std::string spk = fake.fx["spk0_hex"].asString();
    for (size_t i = 0; i < spk.size(); i += 2) script.push_back(static_cast<uint8_t>(std::stoul(spk.substr(i, 2), nullptr, 16)));

    // The server is asked by the SHA-256 of the script, as the real servers expect.
    const auto history = server.ScriptHistory(script);
    ASSERT_TRUE(history.has_value());
    EXPECT_EQ(fake.asked.back(), "/scripthash/" + fake.fx["scripthash"].asString() + "/txs");
    EXPECT_EQ(history->size(), fake.fx["scripthash_txs"].size());
    bool ours = false;
    for (const auto& entry : *history) {
        if (entry.txid != fake.Txid()) continue;
        ours = true;
        EXPECT_TRUE(entry.status.confirmed);
        EXPECT_EQ(entry.status.height, 973000u);
    }
    EXPECT_TRUE(ours);

    Json::Value too_many(Json::arrayValue);
    for (size_t i = 0; i <= kEsploraMaxListedTxs; ++i) too_many.append(fake.fx["scripthash_txs"][0]);
    fake.Set("/scripthash/" + fake.fx["scripthash"].asString() + "/txs", Compact(too_many));
    EXPECT_FALSE(server.ScriptHistory(script).has_value());
    fake.Set("/scripthash/" + fake.fx["scripthash"].asString() + "/txs", R"([{"txid":"nope","status":{"confirmed":false}}])");
    EXPECT_FALSE(server.ScriptHistory(script).has_value());
    fake.Set("/scripthash/" + fake.fx["scripthash"].asString() + "/txs", "[]");
    ASSERT_TRUE(server.ScriptHistory(script).has_value());
    EXPECT_TRUE(server.ScriptHistory(script)->empty());

    const auto spent = server.Outspend(fake.Txid(), 0);
    ASSERT_TRUE(spent.has_value());
    EXPECT_TRUE(spent->spent);
    EXPECT_EQ(BtcHashToHex(spent->txid), fake.fx["outspend"]["txid"].asString());
    EXPECT_EQ(spent->vin, 0u);
    EXPECT_TRUE(spent->status.confirmed);
    EXPECT_EQ(spent->status.height, 973551u);
    // The real spending transaction does spend that output, and carries a witness to read.
    std::vector<uint8_t> spend_raw;
    const std::string spend_hex = fake.fx["spend_hex"].asString();
    for (size_t i = 0; i < spend_hex.size(); i += 2) spend_raw.push_back(static_cast<uint8_t>(std::stoul(spend_hex.substr(i, 2), nullptr, 16)));
    const auto spend = ParseBtcRawTx(spend_raw);
    ASSERT_TRUE(spend.has_value());
    EXPECT_EQ(spend->txid, spent->txid);
    ASSERT_GT(spend->vin.size(), spent->vin);
    EXPECT_EQ(spend->vin[spent->vin].prev_txid, fake.Txid());
    EXPECT_EQ(spend->vin[spent->vin].prev_vout, 0u);
    EXPECT_FALSE(spend->vin[spent->vin].witness.empty());

    fake.Set(fake.TxPath("/outspend/0"), R"({"spent":false})");
    ASSERT_TRUE(server.Outspend(fake.Txid(), 0).has_value());
    EXPECT_FALSE(server.Outspend(fake.Txid(), 0)->spent);
    for (const char* bad : {R"({"spent":true})", R"({"spent":true,"txid":"00","vin":0,"status":{"confirmed":false}})",
                            R"({"spent":1})", "null"}) {
        fake.Set(fake.TxPath("/outspend/0"), bad);
        EXPECT_FALSE(server.Outspend(fake.Txid(), 0).has_value()) << bad;
    }
    EXPECT_FALSE(server.Outspend(fake.Txid(), 7).has_value());  // the server has no such page
    EXPECT_EQ(server.last_error(), EsploraError::NotFound);
}

TEST(BtcEsplora, FeeEstimates) {
    FakeServer fake;
    EsploraServer server(kBase, fake.Http());
    // As mempool.space answered (including its HTTP 203), with rates below 1 sat/vB.
    fake.Set("/fee-estimates", R"({"1":1,"2":1,"3":0.5,"6":0.122,"144":0.113,"x":9,"7":"fast","8":-1,"9":1e12})", 203);
    EXPECT_EQ(server.FeeRate(1), 1.0);
    EXPECT_EQ(server.FeeRate(4), 0.5);       // no entry for 4: the more urgent 3
    EXPECT_EQ(server.FeeRate(6), 0.122);
    EXPECT_EQ(server.FeeRate(100), 0.122);
    EXPECT_EQ(server.FeeRate(1000), 0.113);
    EXPECT_EQ(server.FeeRate(0), 1.0);       // nothing as urgent listed: the most urgent there is
    for (const char* bad : {"{}", "[]", R"({"x":1})", R"({"6":"1"})", "nonsense"}) {
        fake.Set("/fee-estimates", bad);
        EXPECT_FALSE(server.FeeRate(6).has_value()) << bad;
    }
    fake.Set("/fee-estimates", "", 429);
    EXPECT_FALSE(server.FeeRate(6).has_value());
    EXPECT_EQ(server.last_error(), EsploraError::Unreachable);
}

TEST(BtcEsplora, Broadcast) {
    FakeServer fake;
    EsploraServer server(kBase, fake.Http());
    const std::string hex = fake.fx["tx_hex"].asString();
    std::vector<uint8_t> raw;
    for (size_t i = 0; i < hex.size(); i += 2) raw.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));

    fake.post_reply = {200, fake.fx["txid"].asString() + "\n"};
    auto r = server.Broadcast(raw);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.txid, fake.Txid());
    ASSERT_EQ(fake.posted.size(), 1u);
    EXPECT_EQ(fake.posted[0], hex);  // the transaction as hex, nothing else

    fake.post_reply = {200, std::string(64, 'c')};
    r = server.Broadcast(raw);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.error, "server answered with another transaction id");

    fake.post_reply = {400, "sendrawtransaction RPC error: {\"code\":-26,\"message\":\"min relay fee not met\"}\n\x01"};
    r = server.Broadcast(raw);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.error, "HTTP 400: sendrawtransaction RPC error: {\"code\":-26,\"message\":\"min relay fee not met\"}");
    fake.post_reply = {400, std::string(5000, 'x')};
    EXPECT_LT(server.Broadcast(raw).error.size(), 230u);

    fake.post_reply = {0, ""};
    r = server.Broadcast(raw);
    EXPECT_EQ(r.error, "no answer");
    EXPECT_EQ(server.last_error(), EsploraError::Unreachable);

    const size_t before = fake.posted.size();
    r = server.Broadcast({1, 2, 3});
    EXPECT_EQ(r.error, "not a transaction");
    EXPECT_EQ(fake.posted.size(), before);  // never sent
}

TEST(BtcEsplora, OnlyEncryptedServersExceptThisMachine) {
    FakeServer fake;
    EXPECT_NO_THROW(EsploraServer("https://mempool.space/api", fake.Http()));
    EXPECT_NO_THROW(EsploraServer("https://mempool.space/api/", fake.Http()));
    EXPECT_NO_THROW(EsploraServer("http://127.0.0.1:3002", fake.Http()));
    EXPECT_NO_THROW(EsploraServer("http://localhost:3002/api", fake.Http()));
    for (const char* bad : {"http://mempool.space/api", "http://127.0.0.1.evil.example/api", "http://localhost.evil.example",
                            "https://", "ftp://x/y", "mempool.space/api", "", "https://a b/api", "https://a\n.example"}) {
        EXPECT_THROW(EsploraServer(bad, fake.Http()), std::invalid_argument) << bad;
    }
    EXPECT_THROW(EsploraServer("https://mempool.space/api", BtcHttp{}), std::invalid_argument);
    EXPECT_EQ(EsploraServer("https://mempool.space/api///", fake.Http()).base_url(), "https://mempool.space/api");
}
