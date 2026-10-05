// DinWatcher against a scripted Dinero node: RPC cost per tick stays bounded
// as the chain grows (public nodes rate-limit RPC per IP), spends are found
// once mined, and a reorg of the spend block is noticed both ways.
#include "wallet/swap/din_watcher.h"
#include "wallet/swap/runner.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <map>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }

Bytes32 XOnly(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair kp;
    const auto s = Scalar(scalar);
    secp256k1_keypair_create(secp, &kp, s.data());
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

std::array<uint8_t, 33> Compressed(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Scalar(scalar);
    secp256k1_ec_pubkey_create(secp, &pk, s.data());
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();

SwapSession BobWithSecret() {
    SwapSession s;
    auto& o = s.record.offer;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 10 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = XOnly(3);
    o.btc_claim_pubkey = Compressed(4);
    o.expires_unix = kNow;
    o.t_btc_unix = kNow + 48 * 3600;
    o.t_din_unix = kNow + 96 * 3600;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 1;
    s.record.role = Role::BtcSeller;
    s.record.accept.offer_id = OfferId(o);
    s.record.accept.din_claim_pubkey = XOnly(5);
    s.record.accept.btc_refund_pubkey = Compressed(6);
    s.record.secret = kSecret;
    s.btc_scan_from_height = 1;
    s.din_payout_script = std::vector<uint8_t>{0x51, 0x20};
    s.din_payout_script.resize(34, 0x77);
    s.btc_payout_script = s.din_payout_script;
    return s;
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

// A Dinero node with blocks of raw transactions and per-method call counts.
struct FakeNode {
    struct Block { std::vector<Transaction> txs; int gen{0}; };
    std::vector<Block> chain;
    std::optional<std::pair<std::string, uint32_t>> funding;  // txid hex, height
    TxOutPoint funding_outpoint;
    std::map<std::string, int> calls;
    Transaction filler;

    std::string HashAt(uint32_t h) const { return "h" + std::to_string(h) + "g" + std::to_string(chain[h].gen); }
    // Real dinerod's gettxout reads only the block UTXO set (ChainDB), never the
    // mempool; spent_in_mempool here models a node that would also hide mempool
    // spends, which the watcher must not rely on either way.
    bool Spent() const {
        return spent_in_mempool || [&] {
            for (const auto& b : chain)
                for (const auto& tx : b.txs)
                    for (const auto& in : tx.vin)
                        if (in.prevout.txid == funding_outpoint.txid && in.prevout.vout == funding_outpoint.vout) return true;
            return false;
        }();
    }
    bool spent_in_mempool{false};
    int hash_fails{0};  // the next N getblockhash calls fail (node busy, connection drop)
    bool no_address_index{false};  // e.g. an AssumeUTXO node, or a pruned history
    bool odd_genesis{false};       // block 0 in a format the swap parser does not read

    void Mine(std::vector<Transaction> txs = {}) {
        txs.insert(txs.begin(), filler);
        chain.push_back({std::move(txs), 0});
    }

    std::optional<Json::Value> Call(const std::string& m, const Json::Value& p) {
        ++calls[m];
        const uint32_t tip = static_cast<uint32_t>(chain.size() - 1);
        if (m == "getblockchaininfo") {
            Json::Value r;
            r["blocks"] = tip;
            r["mediantime"] = kNow;
            return r;
        }
        if (m == "getaddresshistory") {
            if (no_address_index) return std::nullopt;
            Json::Value r;
            r["transactions"] = Json::Value(Json::arrayValue);
            if (funding) {
                Json::Value e;
                e["txid"] = funding->first;
                e["height"] = funding->second;
                r["transactions"].append(e);
            }
            return r;
        }
        if (m == "getblockhash") {
            if (hash_fails > 0) { --hash_fails; return std::nullopt; }
            const uint32_t h = p[0].asUInt();
            if (h > tip) return std::nullopt;
            return Json::Value(HashAt(h));
        }
        if (m == "getblock") {
            for (uint32_t h = 0; h <= tip; ++h) {
                if (HashAt(h) != p[0].asString()) continue;
                if (h == 0 && odd_genesis) return Json::Value("00");
                std::vector<uint8_t> raw(128, 0);
                raw.push_back(static_cast<uint8_t>(chain[h].txs.size()));
                for (const auto& tx : chain[h].txs) {
                    const auto t = tx.Serialize(TxSerializationMode::WithWitness);
                    raw.insert(raw.end(), t.begin(), t.end());
                }
                raw.push_back(0x00);
                return Json::Value(Hex(raw));
            }
            return std::nullopt;
        }
        if (m == "gettxout") {
            if (Spent()) return Json::Value(Json::nullValue);
            Json::Value r;
            r["value"] = 10.0;
            return r;
        }
        return std::nullopt;
    }
    int BlockCalls() const {
        int n = 0;
        for (const char* m : {"getblockhash", "getblock"}) {
            const auto it = calls.find(m);
            if (it != calls.end()) n += it->second;
        }
        return n;
    }
};

struct Fixture {
    SwapSession bob = BobWithSecret();
    DinHtlcTerms terms = MakeDinTerms(bob.record.offer, bob.record.accept);
    DinHtlcOutput htlc = BuildDinHtlc(terms);
    FakeNode node;
    Transaction funding_tx;
    FundingOutput funding;
    Transaction claim_tx;

    Fixture() {
        // Any well-formed one-in/one-out transaction serves as a template.
        FundingOutput dummy;
        dummy.txid = TxId(uint256::FromHexUnsafe(std::string(64, 'c')));
        dummy.vout = 0;
        dummy.value = AmountUna::Una(bob.record.offer.din_amount_una + 50'000);
        dummy.script_pubkey = htlc.script_pubkey;
        node.filler = BuildDinClaimTx(htlc, dummy, Payout{bob.din_payout_script, AmountUna::Una(1)});
        funding_tx = BuildDinClaimTx(htlc, dummy, Payout{htlc.script_pubkey, AmountUna::Una(50'000)});
        funding.txid = TxId::Compute(funding_tx);
        funding.vout = 0;
        funding.value = AmountUna::Una(bob.record.offer.din_amount_una);
        funding.script_pubkey = htlc.script_pubkey;
        node.funding_outpoint = TxOutPoint(funding.txid, 0);
        const auto raw = SignedDinClaim(bob, SwapKeys{Scalar(5), Scalar(6)}, funding, 100'000);
        size_t used = 0;
        EXPECT_TRUE(TransactionSerializer::Deserialize(claim_tx, raw, used));
    }
};

TEST(SwapDinWatcher, RpcCostPerTickStaysBoundedWhileTheClaimIsUnconfirmed) {
    Fixture f;
    for (int i = 0; i < 150; ++i) f.node.Mine();
    f.node.Mine({f.funding_tx});
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 150};
    for (int i = 0; i < 40; ++i) f.node.Mine();
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, f.terms, "rdin");

    ASSERT_TRUE(w.Observe().ok);
    f.node.spent_in_mempool = true;  // claim broadcast, not yet mined
    ASSERT_TRUE(w.Observe().ok);     // first look after the spend may scan back once
    for (int tick = 0; tick < 30; ++tick) {
        f.node.Mine();
        const int before = f.node.BlockCalls();
        const auto r = w.Observe();
        ASSERT_TRUE(r.ok);
        EXPECT_FALSE(r.htlc.spent);  // the address index cannot see mempool spends
        EXPECT_LE(f.node.BlockCalls() - before, 6) << "tick " << tick << ": rescanning old blocks";
    }
    f.node.spent_in_mempool = false;
    f.node.Mine({f.claim_tx});
    const auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_TRUE(r.htlc.spent_by_claim);
    EXPECT_EQ(r.htlc.revealed_preimage, kSecret);
    EXPECT_EQ(r.htlc.spend_confirmations, 1u);
}

TEST(SwapDinWatcher, AShallowReorgOrAnRpcHiccupRewindsOnlyToTheForkPoint) {
    // A 1-block reorg at the tip, or one failed getblockhash, must not rescan
    // the whole swap history (on mainnet: hundreds of blocks per swap, every
    // time, under the swap manager's lock).
    Fixture f;
    for (int i = 0; i < 150; ++i) f.node.Mine();
    f.node.Mine({f.funding_tx});
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 150};
    for (int i = 0; i < 100; ++i) f.node.Mine();
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, f.terms, "rdin");
    ASSERT_TRUE(w.Observe().ok);

    f.node.chain.back().gen = 1;  // the tip block is replaced
    int before = f.node.BlockCalls();
    auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.output_seen) << "the funding below the fork is kept";
    EXPECT_EQ(r.htlc.output_confirmations, 101u);
    EXPECT_LE(f.node.BlockCalls() - before, 10) << "rewound to the fork point, not to the scan start";

    f.node.Mine();
    f.node.hash_fails = 1;  // the reorg check's own getblockhash fails
    before = f.node.BlockCalls();
    r = w.Observe();
    EXPECT_FALSE(r.ok) << "cannot tell: not observed";
    r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.output_seen);
    EXPECT_LE(f.node.BlockCalls() - before, 10) << "an RPC failure is not a reorg";
}

TEST(SwapDinWatcher, ReorgOfTheSpendBlockIsNoticedBothWays) {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.node.Mine();
    f.node.Mine({f.funding_tx});
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 10};
    for (int i = 0; i < 5; ++i) f.node.Mine();
    f.node.Mine({f.claim_tx});  // height 16
    f.node.Mine();
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, f.terms, "rdin");
    auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_EQ(r.htlc.spend_confirmations, 2u);

    // Reorg: heights 16..17 replaced by blocks without the claim.
    f.node.chain.resize(16);
    f.node.Mine();
    f.node.chain.back().gen = 1;
    f.node.Mine();
    f.node.chain.back().gen = 1;
    r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.htlc.spent);

    // The claim is mined again on the new branch.
    f.node.Mine({f.claim_tx});
    r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_EQ(r.htlc.spend_confirmations, 1u);
}

TEST(SwapDinWatcher, SpendIsFoundAgainWhenTheSameBlocksComeBack) {
    // invalidateblock on the funding block, then reconsiderblock: the very
    // same blocks return, and the spend must be reported again.
    Fixture f;
    for (int i = 0; i < 10; ++i) f.node.Mine();
    f.node.Mine({f.funding_tx});
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 10};
    f.node.Mine({f.claim_tx});  // height 11
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, f.terms, "rdin");
    ASSERT_TRUE(w.Observe().htlc.spent);

    const auto saved = f.node.chain;
    f.node.chain.resize(10);
    f.node.funding.reset();
    auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.htlc.output_seen);

    f.node.chain = saved;
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 10};
    r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_TRUE(r.htlc.spent_by_claim);
}

TEST(SwapDinWatcher, SpendMinedBelowTheScannedTipOnAReorgIsFound) {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.node.Mine();
    f.node.Mine({f.funding_tx});
    f.node.funding = {f.funding.txid.AsUint256().GetHex(), 10};
    for (int i = 0; i < 10; ++i) f.node.Mine();  // tip 20
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, f.terms, "rdin");
    f.node.spent_in_mempool = true;
    ASSERT_TRUE(w.Observe().ok);  // scanned through 20, nothing mined yet

    // A reorg from height 15 whose new branch mines the claim at 15.
    f.node.spent_in_mempool = false;
    f.node.chain.resize(15);
    f.node.Mine({f.claim_tx});
    f.node.chain.back().gen = 1;
    for (int i = 0; i < 5; ++i) {
        f.node.Mine();
        f.node.chain.back().gen = 1;
    }
    const auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.htlc.spent);
    EXPECT_TRUE(r.htlc.spent_by_claim);
    EXPECT_EQ(r.htlc.spend_confirmations, 6u);
}

// A transaction paying the HTLC script `value` una (a decoy unless it is the swap amount).
Transaction PayToHtlc(const Fixture& f, uint64_t value, char tag) {
    FundingOutput src;
    src.txid = TxId(uint256::FromHexUnsafe(std::string(64, tag)));
    src.vout = 0;
    src.value = AmountUna::Una(value + 1000);
    src.script_pubkey = f.htlc.script_pubkey;
    return BuildDinClaimTx(f.htlc, src, Payout{f.htlc.script_pubkey, AmountUna::Una(1000)});
}

DinWatchTarget Target(const Fixture& f, std::string pinned = "") {
    DinWatchTarget t;
    t.terms = f.terms;
    t.scan_from_height = 0;
    t.expected_amount_una = f.bob.record.offer.din_amount_una;
    t.expected_funding_txid = pinned;
    return t;
}

TEST(SwapDinWatcher, SpamToTheHtlcAddressCannotReplaceTheFunding) {
    // Alice sends 60 small payments to the HTLC address after Bob locked BTC:
    // they must not become "the funding", and Bob must still see her real
    // lock claimed, without any address index.
    Fixture f;
    f.node.no_address_index = true;
    for (int i = 0; i < 4; ++i) f.node.Mine();
    f.node.Mine({PayToHtlc(f, 1500, 'b')});  // a decoy BEFORE the real lock
    f.node.Mine({f.funding_tx});  // height 5
    for (int i = 0; i < 60; ++i) f.node.Mine({PayToHtlc(f, 2000 + i, char('0' + i % 10))});
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, Target(f));
    auto r = w.Observe();
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.funding.has_value());
    EXPECT_EQ(r.funding->txid, f.funding.txid);
    EXPECT_EQ(r.htlc.output_value, f.bob.record.offer.din_amount_una);
    f.node.Mine({f.claim_tx});
    r = w.Observe();
    EXPECT_TRUE(r.htlc.spent_by_claim);
    EXPECT_EQ(r.htlc.revealed_preimage, kSecret);
    // The mined claim's output, whoever broadcast it (Bob or his tower).
    ASSERT_TRUE(r.claim_output.has_value());
    EXPECT_EQ(r.claim_output->txid, TxId::Compute(f.claim_tx));
    EXPECT_EQ(r.claim_output->vout, 0u);
    EXPECT_EQ(r.claim_output->value.GetUna(), f.claim_tx.vout[0].value.GetUna());
}

TEST(SwapDinWatcher, TheGenesisBlockIsNeverScanned) {
    // Regtest shares mainnet's canonical genesis, which the swap block parser
    // does not read; it cannot hold an HTLC anyway.
    Fixture f;
    f.node.odd_genesis = true;
    f.node.Mine();
    f.node.Mine({f.funding_tx});
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); }, Target(f));
    const auto r = w.Observe();
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.funding.has_value());
}

TEST(SwapDinWatcher, APinnedFundingIgnoresAnExactAmountDecoy) {
    Fixture f;
    f.node.no_address_index = true;
    f.node.Mine();
    f.node.Mine({PayToHtlc(f, f.bob.record.offer.din_amount_una, 'e')});  // decoy, same amount, first
    f.node.Mine({f.funding_tx});
    DinWatcher w([&](const std::string& m, const Json::Value& p) { return f.node.Call(m, p); },
                 Target(f, f.funding.txid.AsUint256().GetHex()));
    const auto r = w.Observe();
    ASSERT_TRUE(r.funding.has_value());
    EXPECT_EQ(r.funding->txid, f.funding.txid);
}

}  // namespace
