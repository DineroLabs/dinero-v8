// Transactionality of ConsensusUTXOSet::ApplyBlock / UndoBlock (issue #490).
//
// WHY A DEDICATED EQUALITY TEST
// -----------------------------
// The first version of this repair claimed to be transactional while rolling
// back through Snapshot()/Restore() -- a path its own evidence showed to be
// LOSSY (the forest deserializer refuses its own payload and the fallback
// rebuild lands on a different leaf count and root). A rollback built on a
// lossy primitive can replace one partial state with a different corrupted
// state, so "returns false" was never sufficient evidence of anything.
//
// This file asserts the actual property: after a FAILED operation, every
// observable component of the set is byte-for-byte what it was before the call.
//
//   * the UTXO map, entry by entry
//   * height and best-block hash
//   * forest leaf count
//   * forest commitment
//   * internal forest state (roots and per-position node/deletion state)
//
// Commitment equality alone would be too weak: two different internal states
// can share a commitment if deletion bookkeeping diverges, which is exactly the
// class of bug being guarded against.

#include "consensus/consensus_utxo_set.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;

// A full observable fingerprint of the set. Anything that can differ must be
// represented here, or a rollback bug could hide behind an unchecked field.
struct StateFingerprint {
    size_t utxo_count = 0;
    std::string utxo_map;
    uint32_t height = 0;
    std::string best_block;
    uint64_t leaf_count = 0;
    std::string commitment;
    std::string forest_internal;

    bool operator==(const StateFingerprint& other) const {
        return utxo_count == other.utxo_count && utxo_map == other.utxo_map &&
               height == other.height && best_block == other.best_block &&
               leaf_count == other.leaf_count && commitment == other.commitment &&
               forest_internal == other.forest_internal;
    }
};

std::string HexOf(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

template <size_t N>
std::string HexOf(const std::array<uint8_t, N>& bytes) {
    return HexOf(std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

StateFingerprint Fingerprint(const ConsensusUTXOSet& set) {
    StateFingerprint fp;
    fp.utxo_count = set.GetSetSize();
    fp.height = set.GetHeight();
    fp.best_block = set.GetBestBlock().GetHex();

    const UtreexoForest& forest = set.GetForest();
    fp.leaf_count = forest.getNumLeaves();
    fp.commitment = HexOf(forest.getCommitment());

    // Internal forest state: roots plus per-position node/deletion state.
    // Two forests can agree on a commitment while disagreeing about which
    // positions are deleted -- precisely the divergence #490 is about.
    std::ostringstream internal;
    const auto roots = forest.getRoots();
    internal << "roots=" << roots.size() << ':';
    for (const auto& root : roots) {
        internal << HexOf(root) << ',';
    }
    internal << "|deleted=";
    for (uint64_t pos = 0; pos < forest.getNumLeaves(); ++pos) {
        internal << (forest.isDeleted(pos) ? '1' : '0');
    }
    fp.forest_internal = internal.str();

    // The UTXO map itself, ordered deterministically by outpoint text so the
    // comparison does not depend on hash-map iteration order.
    std::vector<std::string> entries;
    for (const auto& [outpoint, entry] : set.GetUTXOs()) {
        std::ostringstream line;
        line << outpoint.ToString() << '=' << entry.value.GetUna() << ':'
             << entry.height << ':' << (entry.isCoinbase ? 1 : 0) << ':'
             << HexOf(entry.scriptPubKey);
        entries.push_back(line.str());
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream joined;
    for (const auto& entry : entries) {
        joined << entry << '\n';
    }
    fp.utxo_map = joined.str();
    return fp;
}

void ExpectFingerprintsEqual(const StateFingerprint& before,
                             const StateFingerprint& after,
                             const std::string& context) {
    EXPECT_EQ(before.utxo_count, after.utxo_count) << context << ": UTXO count";
    EXPECT_EQ(before.utxo_map, after.utxo_map) << context << ": UTXO map";
    EXPECT_EQ(before.height, after.height) << context << ": height";
    EXPECT_EQ(before.best_block, after.best_block) << context << ": best block";
    EXPECT_EQ(before.leaf_count, after.leaf_count) << context << ": forest leaf count";
    EXPECT_EQ(before.commitment, after.commitment) << context << ": commitment";
    EXPECT_EQ(before.forest_internal, after.forest_internal)
        << context << ": internal forest state (roots / deletion bookkeeping)";
}

uint256 HashFromByte(uint8_t seed) {
    std::vector<uint8_t> raw(32, seed);
    uint256 out;
    std::copy(raw.begin(), raw.end(), out.data);
    return out;
}

Transaction MakeCoinbase(uint8_t tag, uint64_t value) {
    Transaction tx;
    TxInput input;
    input.prevout.txid = TxId(uint256());
    input.prevout.vout = 0xffffffff;
    input.scriptSig = {tag, 0x01};
    tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Una(value),
                         std::vector<uint8_t>{0x51, tag});
    return tx;
}

// A transaction spending an outpoint that does not exist. ProcessTransaction
// rejects it at SpendCoin, AFTER the block's earlier transactions have already
// mutated both the UTXO map and the forest.
Transaction MakeSpendOfMissingInput(uint8_t tag) {
    Transaction tx;
    TxInput input;
    input.prevout.txid = TxId(HashFromByte(0xEE));
    input.prevout.vout = 7;
    tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Una(1000),
                         std::vector<uint8_t>{0x51, tag});
    return tx;
}

Block MakeBlock(const std::vector<Transaction>& txs) {
    Block block;
    block.vtx = txs;
    return block;
}

class ConsensusUTXOSetTransactional : public ::testing::Test {
protected:
    ConsensusUTXOSet set_;

    void SetUp() override {
        SelectParams(Chain::REGTEST);
        // Build a non-trivial starting state so a partial apply has something
        // to corrupt. An empty set would make rollback trivially correct.
        for (uint8_t i = 1; i <= 12; ++i) {
            BlockUndo undo;
            UtreexoHash root{};
            std::string error;
            Block block = MakeBlock({MakeCoinbase(i, 50'000 + i)});
            ASSERT_TRUE(set_.ApplyBlock(block, i, HashFromByte(i), undo, root, error))
                << "setup block " << static_cast<int>(i) << ": " << error;
        }
        ASSERT_GT(set_.GetSetSize(), 0U);
        ASSERT_GT(set_.GetForest().getNumLeaves(), 0U);
    }
};

// The core property. A block whose SECOND transaction fails must leave the set
// exactly as it was -- not merely "return false".
TEST_F(ConsensusUTXOSetTransactional, FailedApplyBlockLeavesStateByteIdentical) {
    const StateFingerprint before = Fingerprint(set_);

    // Transaction 0 (coinbase) applies successfully and mutates state; then
    // transaction 1 fails. Without rollback the coinbase's output and leaf
    // would persist.
    Block block = MakeBlock({MakeCoinbase(0x40, 99'000),
                             MakeSpendOfMissingInput(0x41)});

    BlockUndo undo;
    UtreexoHash root{};
    std::string error;
    EXPECT_FALSE(set_.ApplyBlock(block, 99, HashFromByte(0x99), undo, root, error))
        << "block with a missing input must be rejected";
    EXPECT_FALSE(error.empty()) << "a rejection must report why";

    const StateFingerprint after = Fingerprint(set_);
    ExpectFingerprintsEqual(before, after, "failed ApplyBlock");
}

// Repeated failures must not accumulate drift. A rollback that is merely
// approximately right would diverge over iterations.
TEST_F(ConsensusUTXOSetTransactional, RepeatedFailedAppliesDoNotAccumulateDrift) {
    const StateFingerprint before = Fingerprint(set_);

    for (int attempt = 0; attempt < 8; ++attempt) {
        Block block = MakeBlock({MakeCoinbase(static_cast<uint8_t>(0x50 + attempt),
                                              70'000 + attempt),
                                 MakeSpendOfMissingInput(0x5F)});
        BlockUndo undo;
        UtreexoHash root{};
        std::string error;
        EXPECT_FALSE(set_.ApplyBlock(block, 100 + attempt,
                                     HashFromByte(static_cast<uint8_t>(0xA0 + attempt)),
                                     undo, root, error));

        const StateFingerprint after = Fingerprint(set_);
        ExpectFingerprintsEqual(before, after,
                                "failed ApplyBlock attempt " + std::to_string(attempt));
        if (::testing::Test::HasFailure()) {
            return;  // drift already detected; further output adds nothing
        }
    }
}

// A failed apply must not leave a usable-looking undo record behind. A partial
// delta is worse than none: it describes work that was rolled back.
TEST_F(ConsensusUTXOSetTransactional, FailedApplyBlockDoesNotEmitPartialUndoDelta) {
    Block block = MakeBlock({MakeCoinbase(0x60, 88'000),
                             MakeSpendOfMissingInput(0x61)});
    BlockUndo undo;
    UtreexoHash root{};
    std::string error;
    ASSERT_FALSE(set_.ApplyBlock(block, 120, HashFromByte(0x77), undo, root, error));

    EXPECT_TRUE(undo.spent_coins.empty())
        << "failed apply left " << undo.spent_coins.size()
        << " spent-coin record(s) describing work that was rolled back";
    if (undo.utreexo_delta) {
        EXPECT_TRUE(undo.utreexo_delta->deletedLeaves.empty())
            << "failed apply left deletion records for leaves that are still live";
    }
}

// Undo of a genuinely applied block must round-trip exactly. This is the
// success-path counterpart: if it did not hold, the rollback assertions above
// could pass vacuously on a set that never changes.
TEST_F(ConsensusUTXOSetTransactional, ApplyThenUndoRoundTripsExactly) {
    const StateFingerprint before = Fingerprint(set_);

    Block block = MakeBlock({MakeCoinbase(0x70, 61'000)});
    BlockUndo undo;
    UtreexoHash root{};
    std::string error;
    ASSERT_TRUE(set_.ApplyBlock(block, 13, HashFromByte(0x13), undo, root, error))
        << error;

    const StateFingerprint applied = Fingerprint(set_);
    ASSERT_FALSE(before == applied)
        << "ApplyBlock did not change observable state; the round-trip assertion "
           "below would be vacuous";

    ASSERT_TRUE(set_.UndoBlock(block, 13, undo, error)) << error;

    StateFingerprint undone = Fingerprint(set_);
    // UndoBlock deliberately leaves best_block_ to the caller, so compare it
    // out rather than pretending it is restored here.
    undone.best_block = before.best_block;
    ExpectFingerprintsEqual(before, undone, "apply-then-undo round trip");
}

// A mismatched undo record must be rejected without touching state.
TEST_F(ConsensusUTXOSetTransactional, FailedUndoBlockLeavesStateByteIdentical) {
    Block block = MakeBlock({MakeCoinbase(0x80, 55'000)});
    BlockUndo undo;
    UtreexoHash root{};
    std::string error;
    ASSERT_TRUE(set_.ApplyBlock(block, 14, HashFromByte(0x14), undo, root, error))
        << error;

    const StateFingerprint before = Fingerprint(set_);

    // Wrong height: rejected up front, but the guard must still hold.
    EXPECT_FALSE(set_.UndoBlock(block, 999, undo, error));
    ExpectFingerprintsEqual(before, Fingerprint(set_), "failed UndoBlock (height mismatch)");
}

}  // namespace
