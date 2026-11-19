// Reading other people's Bitcoin transactions and proving them into a block,
// for swaps without Bitcoin Core: the real genesis transaction, transactions
// built here, and everything a hostile server could send instead.

#include "wallet/swap/btc_merkle_proof.h"
#include "wallet/swap/btc_raw_tx.h"
#include "wallet/swap/btc_tx.h"

#include "crypto/sha256.h"

#include <gtest/gtest.h>

#include <random>

using namespace dinero::swap;

namespace {

std::vector<uint8_t> Unhex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// Bitcoin's first transaction (the coinbase of block 0).
const char* kGenesisCoinbase =
    "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4d04ffff001d0104455468"
    "652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261"
    "696c6f757420666f722062616e6b73ffffffff0100f2052a01000000434104678afdb0fe5548271967f1a67130b7105cd6a828e0"
    "3909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5fac00000000";

// A legacy transaction: one input with a signature script, one output.
std::vector<uint8_t> LegacyTx(uint32_t salt, bool coinbase = false, size_t script_sig_len = 3, size_t spk_len = 25) {
    std::vector<uint8_t> tx{1, 0, 0, 0, 1};
    for (int i = 0; i < 32; ++i) tx.push_back(coinbase ? 0 : static_cast<uint8_t>(salt >> (8 * (i % 4))));
    for (int i = 0; i < 4; ++i) tx.push_back(coinbase ? 0xff : static_cast<uint8_t>(i == 0));
    tx.push_back(static_cast<uint8_t>(script_sig_len));
    for (size_t i = 0; i < script_sig_len; ++i) tx.push_back(static_cast<uint8_t>(salt >> (8 * (i % 4))));
    tx.insert(tx.end(), {0xff, 0xff, 0xff, 0xff, 1});
    for (int i = 0; i < 8; ++i) tx.push_back(static_cast<uint8_t>(i == 0 ? 0x10 : 0));
    tx.push_back(static_cast<uint8_t>(spk_len));
    for (size_t i = 0; i < spk_len; ++i) tx.push_back(static_cast<uint8_t>(0x51 + i % 3));
    tx.insert(tx.end(), {0, 0, 0, 0});
    return tx;
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

// Bitcoin's Merkle tree, written independently of the code under test: pairs
// are hashed level by level and an odd last node is paired with itself.
struct Tree {
    std::vector<std::vector<Bytes32>> levels;  // levels[0] are the txids
    explicit Tree(const std::vector<std::vector<uint8_t>>& txs) {
        std::vector<Bytes32> level;
        for (const auto& raw : txs) level.push_back(Sha256d(raw.data(), raw.size()));  // legacy: id = hash of the bytes
        levels.push_back(level);
        while (levels.back().size() > 1) {
            const auto& below = levels.back();
            std::vector<Bytes32> above;
            for (size_t i = 0; i < below.size(); i += 2) {
                const Bytes32& right = i + 1 < below.size() ? below[i + 1] : below[i];
                above.push_back(Sha256d(below[i].data(), 32, right.data(), 32));
            }
            levels.push_back(above);
        }
    }
    Bytes32 Root() const { return levels.back()[0]; }
    BtcMerkleProof Proof(uint32_t position) const {
        BtcMerkleProof p;
        p.position = position;
        size_t index = position;
        for (size_t l = 0; l + 1 < levels.size(); ++l, index /= 2) {
            const size_t sibling = index ^ 1;
            p.siblings.push_back(sibling < levels[l].size() ? levels[l][sibling] : levels[l][index]);
        }
        return p;
    }
};

struct Block {
    std::vector<std::vector<uint8_t>> txs;
    Tree tree;
    BtcBlockHeader header;
    explicit Block(size_t count) : txs(Make(count)), tree(txs) { header.merkle_root = tree.Root(); }
    static std::vector<std::vector<uint8_t>> Make(size_t count) {
        std::vector<std::vector<uint8_t>> txs{LegacyTx(0xc0ffee, /*coinbase=*/true, 4)};
        for (size_t i = 1; i < count; ++i) txs.push_back(LegacyTx(static_cast<uint32_t>(1000 + i)));
        return txs;
    }
    BtcTxInclusion Inclusion(uint32_t position) const {
        return {txs[position], tree.Proof(position), txs[0], tree.Proof(0)};
    }
};

}  // namespace

// ---- Transactions -------------------------------------------------------------

TEST(BtcRawTx, TheRealGenesisTransaction) {
    const auto raw = Unhex(kGenesisCoinbase);
    ASSERT_EQ(raw.size(), 204u);
    const auto tx = ParseBtcRawTx(raw);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(BtcHashToHex(tx->txid), "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    EXPECT_TRUE(tx->IsCoinbase());
    EXPECT_FALSE(tx->has_witness);
    EXPECT_EQ(tx->stripped_size, 204u);
    ASSERT_EQ(tx->vout.size(), 1u);
    EXPECT_EQ(tx->vout[0].value_sat, 5'000'000'000ULL);
    EXPECT_EQ(tx->vout[0].script_pubkey.size(), 67u);
    EXPECT_EQ(tx->vin[0].script_sig.size(), 77u);

    // It is the whole of block 0: the id is that block's Merkle root.
    const BtcBlockHeader genesis = BtcChainParams::MainnetFromGenesis().checkpoint;
    EXPECT_EQ(tx->txid, genesis.merkle_root);
    EXPECT_FALSE(BtcCheckTxInclusion({raw, {}, raw, {}}, genesis).has_value());
}

TEST(BtcRawTx, WitnessIsNotPartOfTheId) {
    BtcTx built;
    BtcTxIn in;
    in.prev_txid.fill(0x42);
    in.prev_vout = 1;
    in.witness = {{0x30, 0x44, 0x01}, std::vector<uint8_t>(32, 0x07), {}, {0x51, 0x52}};
    built.vin.push_back(in);
    built.vout.push_back({123456, {0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}});
    built.locktime = 777;

    const auto with = ParseBtcRawTx(SerializeBtcTx(built, true));
    const auto without = ParseBtcRawTx(SerializeBtcTx(built, false));
    ASSERT_TRUE(with.has_value());
    ASSERT_TRUE(without.has_value());
    EXPECT_TRUE(with->has_witness);
    EXPECT_FALSE(without->has_witness);
    EXPECT_EQ(with->txid, BtcTxid(built));
    EXPECT_EQ(without->txid, BtcTxid(built));
    EXPECT_EQ(with->stripped_size, SerializeBtcTx(built, false).size());
    EXPECT_EQ(with->vin[0].witness, in.witness);  // the secret is read from here
    EXPECT_EQ(with->vin[0].prev_vout, 1u);
    EXPECT_EQ(with->locktime, 777u);
    EXPECT_FALSE(with->IsCoinbase());
}

TEST(BtcRawTx, RefusesAnythingBitcoinWould) {
    const auto good = LegacyTx(7);
    ASSERT_TRUE(ParseBtcRawTx(good).has_value());

    // Every proper prefix, and any extra byte.
    for (size_t n = 0; n < good.size(); ++n) {
        EXPECT_FALSE(ParseBtcRawTx({good.begin(), good.begin() + n}).has_value()) << n;
    }
    auto longer = good;
    longer.push_back(0);
    EXPECT_FALSE(ParseBtcRawTx(longer).has_value());

    auto no_inputs = good;
    no_inputs[4] = 0;  // read as a witness marker with a bad flag
    EXPECT_FALSE(ParseBtcRawTx(no_inputs).has_value());

    // Input and output counts far beyond the bytes present.
    auto many = good;
    many[4] = 0xfc;
    EXPECT_FALSE(ParseBtcRawTx(many).has_value());
    std::vector<uint8_t> huge{1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    huge.resize(200);
    EXPECT_FALSE(ParseBtcRawTx(huge).has_value());

    // A count written longer than needed (0xfd 0x01 0x00 for 1).
    std::vector<uint8_t> padded(good.begin(), good.begin() + 4);
    padded.insert(padded.end(), {0xfd, 0x01, 0x00});
    padded.insert(padded.end(), good.begin() + 5, good.end());
    EXPECT_FALSE(ParseBtcRawTx(padded).has_value());

    // The witness flag with no witness in it, and a flag that is not 1.
    std::vector<uint8_t> flagged(good.begin(), good.begin() + 4);
    flagged.insert(flagged.end(), {0x00, 0x01});
    flagged.insert(flagged.end(), good.begin() + 4, good.end() - 4);
    flagged.push_back(0x00);  // an empty witness for the one input
    flagged.insert(flagged.end(), good.end() - 4, good.end());
    EXPECT_FALSE(ParseBtcRawTx(flagged).has_value());
    flagged[flagged.size() - 5] = 0x01;  // one item ...
    flagged.insert(flagged.end() - 4, {0x01, 0xaa});  // ... of one byte
    EXPECT_TRUE(ParseBtcRawTx(flagged).has_value());
    flagged[5] = 0x02;
    EXPECT_FALSE(ParseBtcRawTx(flagged).has_value());

    // Well-formed but larger than any block: one output script fills it up.
    auto oversized = [](size_t total) {
        const size_t fixed = 4 + 1 + 32 + 4 + 1 + 4 + 1 + 8 + 5 + 4;
        const uint32_t script_len = static_cast<uint32_t>(total - fixed);
        std::vector<uint8_t> tx{1, 0, 0, 0, 1};
        tx.insert(tx.end(), 32, 0x11);
        tx.insert(tx.end(), {0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0xfe});
        for (int i = 0; i < 4; ++i) tx.push_back(static_cast<uint8_t>(script_len >> (8 * i)));
        tx.insert(tx.end(), script_len, 0x6a);
        tx.insert(tx.end(), {0, 0, 0, 0});
        return tx;
    };
    ASSERT_EQ(oversized(kBtcMaxRawTxSize).size(), kBtcMaxRawTxSize);
    EXPECT_TRUE(ParseBtcRawTx(oversized(kBtcMaxRawTxSize)).has_value());
    EXPECT_FALSE(ParseBtcRawTx(oversized(kBtcMaxRawTxSize + 1)).has_value());
    EXPECT_FALSE(ParseBtcRawTx({}).has_value());
}

// Whatever arrives from the network, the parser answers and stays inside its input.
TEST(BtcRawTx, RandomDamageNeverCrashes) {
    std::mt19937 rng(20261118);
    const std::vector<std::vector<uint8_t>> seeds{LegacyTx(1), LegacyTx(2, true, 60), Unhex(kGenesisCoinbase)};
    size_t parsed = 0;
    for (int round = 0; round < 60000; ++round) {
        auto raw = seeds[round % seeds.size()];
        const int edits = 1 + rng() % 4;
        for (int e = 0; e < edits; ++e) {
            switch (rng() % 4) {
                case 0: raw[rng() % raw.size()] = static_cast<uint8_t>(rng()); break;
                case 1: raw.resize(rng() % (raw.size() + 1)); break;
                case 2: raw.insert(raw.begin() + rng() % (raw.size() + 1), static_cast<uint8_t>(rng())); break;
                default: raw[rng() % raw.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            }
            if (raw.empty()) raw.push_back(0);
        }
        if (const auto tx = ParseBtcRawTx(raw)) {
            ++parsed;
            EXPECT_LE(tx->stripped_size, raw.size());
            EXPECT_FALSE(tx->vin.empty());
        }
    }
    EXPECT_GT(parsed, 0u);  // some damage leaves a different, well-formed transaction
}

// ---- Proofs -------------------------------------------------------------------

TEST(BtcMerkleProof, EveryTransactionOfEveryTreeShape) {
    for (size_t count : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 13u, 33u}) {
        const Block block(count);
        for (uint32_t position = 0; position < count; ++position) {
            const auto why = BtcCheckTxInclusion(block.Inclusion(position), block.header);
            EXPECT_FALSE(why.has_value()) << count << " txs, position " << position << ": " << why.value_or("");
        }
    }
}

TEST(BtcMerkleProof, RefusesWhatIsNotInTheBlock) {
    const Block block(13);
    const auto good = block.Inclusion(5);
    ASSERT_FALSE(BtcCheckTxInclusion(good, block.header).has_value());

    auto other_tx = good;
    other_tx.raw_tx = LegacyTx(999999);  // a transaction the block does not hold
    EXPECT_EQ(BtcCheckTxInclusion(other_tx, block.header), "proof does not reach the block's Merkle root");

    auto wrong_position = good;
    wrong_position.proof.position = 4;
    EXPECT_EQ(BtcCheckTxInclusion(wrong_position, block.header), "proof does not reach the block's Merkle root");

    auto bad_sibling = good;
    bad_sibling.proof.siblings[1][0] ^= 1;
    EXPECT_EQ(BtcCheckTxInclusion(bad_sibling, block.header), "proof does not reach the block's Merkle root");

    BtcBlockHeader other_block = block.header;
    other_block.merkle_root[31] ^= 1;
    EXPECT_EQ(BtcCheckTxInclusion(good, other_block), "coinbase: proof does not reach the block's Merkle root");

    auto garbage = good;
    garbage.raw_tx = {1, 2, 3};
    EXPECT_EQ(BtcCheckTxInclusion(garbage, block.header), "transaction: not a transaction");

    auto out_of_range = good;
    out_of_range.proof.position = 16;  // four steps address positions 0..15
    EXPECT_EQ(BtcCheckTxInclusion(out_of_range, block.header), "malformed proof");
}

// The tree does not commit to its depth: the coinbase pins it.
TEST(BtcMerkleProof, TheCoinbasePinsTheDepth) {
    const Block block(8);
    const auto good = block.Inclusion(3);
    ASSERT_EQ(good.proof.siblings.size(), 3u);

    auto deeper = good;  // one step more than the block's tree has
    deeper.proof.siblings.insert(deeper.proof.siblings.begin(), Bytes32{});
    EXPECT_EQ(BtcCheckTxInclusion(deeper, block.header), "proof depth differs from the coinbase's");
    auto shallower = good;
    shallower.proof.siblings.pop_back();
    EXPECT_EQ(BtcCheckTxInclusion(shallower, block.header), "proof depth differs from the coinbase's");

    auto not_first = good;  // a real transaction of the block, but not its first
    not_first.raw_coinbase_tx = block.txs[2];
    not_first.coinbase_proof = block.tree.Proof(2);
    EXPECT_EQ(BtcCheckTxInclusion(not_first, block.header), "coinbase: not a coinbase transaction");

    auto moved = good;
    moved.coinbase_proof.position = 1;
    EXPECT_EQ(BtcCheckTxInclusion(moved, block.header), "coinbase: not the block's first transaction");

    auto foreign = good;  // a coinbase, of some other block
    foreign.raw_coinbase_tx = LegacyTx(0xdead, /*coinbase=*/true, 4);
    EXPECT_EQ(BtcCheckTxInclusion(foreign, block.header), "coinbase: proof does not reach the block's Merkle root");
}

// A 64-byte transaction and an inner node (two hashes) look the same.
TEST(BtcMerkleProof, SixtyFourByteTransactionsAreRefused) {
    const auto tx64 = LegacyTx(5, false, 0, 4);
    ASSERT_EQ(tx64.size(), 64u);
    ASSERT_TRUE(ParseBtcRawTx(tx64).has_value());  // well-formed, which is the danger

    std::vector<std::vector<uint8_t>> txs{LegacyTx(1, true, 4), tx64, LegacyTx(3), LegacyTx(4)};
    const Tree tree(txs);
    BtcBlockHeader header;
    header.merkle_root = tree.Root();
    EXPECT_EQ(BtcCheckTxInclusion({txs[1], tree.Proof(1), txs[0], tree.Proof(0)}, header),
              "transaction: a 64-byte transaction cannot be told from an inner tree node");
    EXPECT_FALSE(BtcCheckTxInclusion({txs[2], tree.Proof(2), txs[0], tree.Proof(0)}, header).has_value());
}

TEST(BtcMerkleProof, ProofLengthIsBounded) {
    BtcMerkleProof p;
    p.siblings.resize(kBtcMaxMerkleDepth);
    EXPECT_TRUE(BtcMerkleRootFromProof(Bytes32{}, p).has_value());
    p.siblings.resize(kBtcMaxMerkleDepth + 1);
    EXPECT_FALSE(BtcMerkleRootFromProof(Bytes32{}, p).has_value());
    p.siblings.clear();
    p.position = 1;  // no steps: only position 0 exists
    EXPECT_FALSE(BtcMerkleRootFromProof(Bytes32{}, p).has_value());
}
