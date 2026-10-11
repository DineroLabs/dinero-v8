// BtcWatcher against a scripted Bitcoin Core (getblock verbosity 2 JSON):
// claims are recognised by content, funding is the exact swap output (not a
// decoy paying the same script), and a reorg of an already-scanned block is
// noticed.
#include "wallet/swap/btc_watcher.h"

#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <map>

namespace {

using namespace dinero;
using namespace dinero::swap;

const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();
constexpr uint64_t kAmount = 1'000'000;

std::array<uint8_t, 33> Pub(uint8_t b) { std::array<uint8_t, 33> k{}; k.fill(b); k[0] = 0x02; return k; }

BtcHtlcTerms Terms() {
    BtcHtlcTerms t;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(t.payment_hash.data());
    t.claim_pubkey = Pub(0x11);
    t.refund_pubkey = Pub(0x22);
    t.refund_locktime_unix = 1'800'000'000;
    return t;
}

std::string Hex(const std::vector<uint8_t>& b) { return detail::ToHex(b); }

// A chain of blocks, each a list of txs in getblock(…, 2) form.
struct FakeBitcoin {
    struct Block { Json::Value txs{Json::arrayValue}; int gen{0}; };
    std::vector<Block> chain;
    std::string chain_name{"regtest"};
    Json::Value mempool_spender;  // gettxspendingprevout entry
    int getblocks{0};
    int hash_fails{0};  // the next N getblockhash calls fail
    std::optional<uint32_t> fail_height_once;  // getblockhash of this height fails once
    std::string Hash(size_t h) const { return "b" + std::to_string(h) + "g" + std::to_string(chain[h].gen); }
    void Mine(Json::Value txs = Json::Value(Json::arrayValue)) { chain.push_back({txs, 0}); }
    std::optional<Json::Value> Call(const std::string& m, const Json::Value& p) {
        if (m == "getblockchaininfo") {
            Json::Value r;
            r["blocks"] = Json::UInt(chain.size() - 1);
            r["mediantime"] = 1'790'000'000;
            r["chain"] = chain_name;
            return r;
        }
        if (m == "getblockhash") {
            if (hash_fails > 0) { --hash_fails; return std::nullopt; }
            const auto h = p[0].asUInt();
            if (fail_height_once == h) { fail_height_once.reset(); return std::nullopt; }
            if (h >= chain.size()) return std::nullopt;
            return Json::Value(Hash(h));
        }
        if (m == "getblock") {
            ++getblocks;
            for (size_t h = 0; h < chain.size(); ++h) {
                if (Hash(h) == p[0].asString()) {
                    Json::Value b;
                    b["tx"] = chain[h].txs;
                    return b;
                }
            }
            return std::nullopt;
        }
        if (m == "gettxspendingprevout") {
            Json::Value r(Json::arrayValue);
            r.append(mempool_spender.isNull() ? Json::Value(Json::objectValue) : mempool_spender);
            return r;
        }
        return std::nullopt;
    }
};

Json::Value FundingTx(const std::string& txid, uint64_t sat, const std::string& spk_hex) {
    Json::Value tx;
    tx["txid"] = txid;
    Json::Value out;
    out["n"] = 0;
    out["value"] = double(sat) / 1e8;
    out["scriptPubKey"]["hex"] = spk_hex;
    tx["vout"].append(out);
    tx["vin"] = Json::Value(Json::arrayValue);
    return tx;
}

Json::Value SpendTx(const std::string& txid, const std::string& prev, const std::vector<std::vector<uint8_t>>& w) {
    Json::Value tx;
    tx["txid"] = txid;
    Json::Value in;
    in["txid"] = prev;
    in["vout"] = 0;
    for (const auto& item : w) in["txinwitness"].append(Hex(item));
    tx["vin"].append(in);
    tx["vout"] = Json::Value(Json::arrayValue);
    return tx;
}

const std::string kFund = std::string(64, 'a');
const std::string kDecoy = std::string(64, 'd');

std::string SpkHex() { return Hex(BtcP2wshScriptPubKey(BuildBtcHtlcWitnessScript(Terms()))); }

BtcWatcher Watcher(FakeBitcoin& node, std::optional<std::string> expected_txid = std::nullopt) {
    BtcWatchTarget t;
    t.terms = Terms();
    t.scan_from_height = 0;
    t.expected_amount_sat = kAmount;
    t.expected_chain = "regtest";
    if (expected_txid) t.expected_funding_txid = *expected_txid;
    return BtcWatcher([&node](const std::string& m, const Json::Value& p) { return node.Call(m, p); }, t);
}

TEST(SwapBtcWatcher, AClaimIsRecognisedByItsSecretNotItsWitnessShape) {
    FakeBitcoin node;
    node.Mine();
    node.Mine([] { Json::Value a(Json::arrayValue); a.append(FundingTx(kFund, kAmount, SpkHex())); return a; }());
    const std::vector<uint8_t> s(kSecret.begin(), kSecret.end());
    // A claim mined with a non-minimal OP_IF selector (valid by consensus).
    node.Mine([&] {
        Json::Value a(Json::arrayValue);
        a.append(SpendTx(std::string(64, 'c'), kFund, {std::vector<uint8_t>(72, 0x30), s, {0x02}, {0x63}}));
        return a;
    }());
    auto w = Watcher(node);
    const auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_TRUE(r.htlc.spent_by_claim);
    ASSERT_TRUE(r.htlc.revealed_preimage.has_value());
    EXPECT_EQ(*r.htlc.revealed_preimage, kSecret);
}

TEST(SwapBtcWatcher, ADecoyPayingTheSameScriptIsNotTheFunding) {
    // Alice front-runs Bob's funding with dust to the same P2WSH script, mined
    // first in the same block; then claims Bob's real output.
    FakeBitcoin node;
    node.Mine();
    node.Mine([] {
        Json::Value a(Json::arrayValue);
        a.append(FundingTx(kDecoy, 330, SpkHex()));
        a.append(FundingTx(kFund, kAmount, SpkHex()));
        return a;
    }());
    const std::vector<uint8_t> s(kSecret.begin(), kSecret.end());
    node.Mine([&] {
        Json::Value a(Json::arrayValue);
        a.append(SpendTx(std::string(64, 'c'), kFund, {std::vector<uint8_t>(72, 0x30), s, {0x01}, {0x63}}));
        return a;
    }());
    for (auto expected : {std::optional<std::string>{}, std::optional<std::string>{kFund}}) {
        auto w = Watcher(node, expected);
        const auto r = w.Observe();
        ASSERT_TRUE(r.ok);
        ASSERT_TRUE(r.funding.has_value());
        EXPECT_EQ(r.htlc.output_value, kAmount) << "funding must be the exact swap output";
        EXPECT_TRUE(r.htlc.spent_by_claim) << "the claim of the real output must be seen";
        EXPECT_EQ(r.htlc.revealed_preimage, kSecret);
    }
    // Bob, who knows his funding txid, ignores even an exact-amount decoy.
    FakeBitcoin node2;
    node2.Mine();
    node2.Mine([] {
        Json::Value a(Json::arrayValue);
        a.append(FundingTx(kDecoy, kAmount, SpkHex()));
        a.append(FundingTx(kFund, kAmount, SpkHex()));
        return a;
    }());
    auto w = Watcher(node2, kFund);
    const auto r = w.Observe();
    ASSERT_TRUE(r.funding.has_value());
    EXPECT_EQ(detail::ToHex(std::vector<uint8_t>(r.funding->txid.rbegin(), r.funding->txid.rend())), kFund);
}

TEST(SwapBtcWatcher, AReorgOfAnAlreadyScannedBlockIsNoticed) {
    FakeBitcoin node;
    node.Mine();
    node.Mine([] { Json::Value a(Json::arrayValue); a.append(FundingTx(kFund, kAmount, SpkHex())); return a; }());
    node.Mine();  // height 2: scanned, nothing in it
    auto w = Watcher(node);
    ASSERT_FALSE(w.Observe().htlc.spent);
    // Height 2 is replaced by a block that contains the claim.
    const std::vector<uint8_t> s(kSecret.begin(), kSecret.end());
    node.chain[2].gen = 1;
    node.chain[2].txs.append(SpendTx(std::string(64, 'c'), kFund, {std::vector<uint8_t>(72, 0x30), s, {0x01}, {0x63}}));
    const auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent_by_claim);
}

TEST(SwapBtcWatcher, AShallowReorgOrAnRpcHiccupRewindsOnlyToTheForkPoint) {
    FakeBitcoin node;
    for (int i = 0; i < 100; ++i) node.Mine();
    node.Mine([] { Json::Value a(Json::arrayValue); a.append(FundingTx(kFund, kAmount, SpkHex())); return a; }());
    for (int i = 0; i < 100; ++i) node.Mine();
    auto w = Watcher(node);
    ASSERT_TRUE(w.Observe().htlc.output_seen);

    node.chain.back().gen = 1;  // the tip block is replaced
    int before = node.getblocks;
    auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_LE(node.getblocks - before, 3) << "rewound to the fork point, not to the scan start";

    node.Mine();
    for (int fails = 1; fails <= 3; ++fails) {  // whichever reorg check's getblockhash fails
        if (fails == 3) {
            node.fail_height_once = 100;  // the funding block's own reorg check
        } else {
            node.hash_fails = fails;
        }
        before = node.getblocks;
        for (int i = 0; i < 4 && !(r = w.Observe()).ok; ++i) {
        }
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(r.htlc.output_seen) << fails;
        EXPECT_LE(node.getblocks - before, 3) << fails << ": an RPC failure is not a reorg";
    }
}

TEST(SwapBtcWatcher, AWrongNetworkNodeIsNotObserved) {
    FakeBitcoin node;
    node.chain_name = "test";
    node.Mine();
    auto w = Watcher(node);
    EXPECT_FALSE(w.Observe().ok);
}

}  // namespace
