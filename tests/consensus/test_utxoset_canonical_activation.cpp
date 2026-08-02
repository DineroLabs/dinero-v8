// Canonical-roots activation plumbing in the pure ConsensusUTXOSet API (#490).
//
// WHAT THIS PROVES
// ----------------
// The canonical-roots fork (Stage 3, utreexo_canonical_roots_activation.h) is
// already deployed and active on mainnet at height 2870 (regtest 10,
// testnet 0). It is NOT an open defect.
//
// What was missing is plumbing: ApplyBlock and UndoBlock never applied the
// height-derived canonical mode, while Restore() did. This API could therefore
// build a POST-activation forest still running LEGACY semantics -- and that
// mode mismatch, not unrepaired root logic, is what made a freshly generated
// proof fail to verify here.
//
// These tests prove the boundary is now handled in both directions, and that
// once the mode is right the proof path and the trusted path agree exactly.
//
// WHY DRAINED SUBTREES, NOT RANDOM COVERAGE
// -----------------------------------------
// Stage 3 changed exactly one thing: what recomputePath() writes when the LAST
// live leaf of a subtree is deleted. Pre-fix it wrote nullopt, breaking the
// invariant `roots_[h].has_value() <=> bit h of numLeaves_`; the next add()
// then took the "place" branch instead of "merge" and cascaded into the state
// that broke proof.verify.
//
// A random spend sequence may never fully drain a subtree, so random coverage
// can pass while never touching the changed behaviour at all. These tests
// deliberately drain subtrees to empty and then add, which is the only
// construction that exercises it.

#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_canonical_roots_activation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;

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

uint256 HashFromByte(uint8_t seed) {
    uint256 out;
    std::vector<uint8_t> raw(32, seed);
    std::copy(raw.begin(), raw.end(), out.data);
    return out;
}

Transaction MakeCoinbase(uint8_t tag, uint64_t value) {
    Transaction tx;
    TxInput input;
    input.prevout.txid = TxId(uint256());
    input.prevout.vout = 0xffffffff;
    input.scriptSig = {tag, 0x02};
    tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Una(value),
                         std::vector<uint8_t>{0x51, tag});
    return tx;
}

Transaction MakeSpend(const TxId& txid, uint32_t vout, uint8_t tag,
                      uint64_t value) {
    Transaction tx;
    TxInput input;
    input.prevout.txid = txid;
    input.prevout.vout = vout;
    tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Una(value),
                         std::vector<uint8_t>{0x51, tag});
    return tx;
}

Block MakeBlock(const std::vector<Transaction>& txs) {
    Block block;
    block.vtx = txs;
    return block;
}

class UtxoSetCanonicalActivation : public ::testing::Test {
protected:
    void SetUp() override { SelectParams(Chain::REGTEST); }
};

// Regtest activates at height 10. Establish that as fact before relying on it.
TEST_F(UtxoSetCanonicalActivation, RegtestActivationBoundaryIsTen) {
    EXPECT_FALSE(IsUtreexoCanonicalRootsActive(9));
    EXPECT_TRUE(IsUtreexoCanonicalRootsActive(10));
    EXPECT_TRUE(IsUtreexoCanonicalRootsActive(11));
}

// THE BOUNDARY: 9 -> 10 activates; undo 10 -> 9 restores legacy semantics.
TEST_F(UtxoSetCanonicalActivation, ApplyAcrossBoundaryActivatesAndUndoRestores) {
    ConsensusUTXOSet set;
    std::string error;
    UtreexoHash root{};

    // Blocks 1..9: pre-activation. Mode must stay legacy throughout.
    for (uint8_t h = 1; h <= 9; ++h) {
        BlockUndo undo;
        ASSERT_TRUE(set.ApplyBlock(MakeBlock({MakeCoinbase(h, 50'000 + h)}), h,
                                   HashFromByte(h), undo, root, error))
            << "height " << static_cast<int>(h) << ": " << error;
        EXPECT_FALSE(set.GetForest().isCanonicalEmptyRoots())
            << "canonical mode enabled early at height " << static_cast<int>(h);
    }

    // Block 10: the activation block. Applying it must flip the mode.
    BlockUndo undo10;
    ASSERT_TRUE(set.ApplyBlock(MakeBlock({MakeCoinbase(10, 60'000)}), 10,
                               HashFromByte(10), undo10, root, error))
        << error;
    EXPECT_TRUE(set.GetForest().isCanonicalEmptyRoots())
        << "applying the activation block did not enable canonical mode";

    // Undoing back to 9 must restore LEGACY semantics. The asymmetry here --
    // flipping on but never off -- is what made block 2870 un-disconnectable
    // on mainnet.
    ASSERT_TRUE(set.UndoBlock(MakeBlock({MakeCoinbase(10, 60'000)}), 10, undo10,
                              error)) << error;
    EXPECT_FALSE(set.GetForest().isCanonicalEmptyRoots())
        << "undoing across the boundary left the forest in post-fork mode";
}

// Re-applying after an undo must return to the same commitment. If the mode
// transition were one-way, this would diverge.
TEST_F(UtxoSetCanonicalActivation, BoundaryRoundTripIsCommitmentStable) {
    ConsensusUTXOSet set;
    std::string error;
    UtreexoHash root{};

    for (uint8_t h = 1; h <= 9; ++h) {
        BlockUndo undo;
        ASSERT_TRUE(set.ApplyBlock(MakeBlock({MakeCoinbase(h, 50'000 + h)}), h,
                                   HashFromByte(h), undo, root, error)) << error;
    }
    const std::string commitment_at_9 = HexOf(set.GetForest().getCommitment());
    const std::string serialized_at_9 = HexOf(set.GetForest().serialize());

    const Block block10 = MakeBlock({MakeCoinbase(10, 60'000)});
    BlockUndo undo10;
    UtreexoHash root10{};
    ASSERT_TRUE(set.ApplyBlock(block10, 10, HashFromByte(10), undo10, root10,
                               error)) << error;
    const std::string commitment_at_10 = HexOf(set.GetForest().getCommitment());
    ASSERT_NE(commitment_at_9, commitment_at_10)
        << "block 10 did not change the commitment; the round trip below would "
           "be vacuous";

    ASSERT_TRUE(set.UndoBlock(block10, 10, undo10, error)) << error;
    EXPECT_EQ(HexOf(set.GetForest().getCommitment()), commitment_at_9)
        << "commitment did not return to its height-9 value after undo";
    EXPECT_EQ(HexOf(set.GetForest().serialize()), serialized_at_9)
        << "serialized forest did not return to its height-9 bytes after undo";

    BlockUndo undo10b;
    UtreexoHash root10b{};
    ASSERT_TRUE(set.ApplyBlock(block10, 10, HashFromByte(10), undo10b, root10b,
                               error)) << error;
    EXPECT_EQ(HexOf(set.GetForest().getCommitment()), commitment_at_10)
        << "re-applying block 10 produced a different commitment";
}

// BulkLoad is handed a height and must adopt that height's semantics. Found by
// auditing forest-construction sites: Clear() installs a fresh (legacy) forest
// and the rebuild loop never consulted the height, so a post-activation bulk
// load produced legacy semantics.
TEST_F(UtxoSetCanonicalActivation, BulkLoadAdoptsCanonicalModeForItsHeight) {
    std::unordered_map<OutPoint, UTXOEntry> utxos;
    for (uint8_t i = 1; i <= 6; ++i) {
        utxos.emplace(OutPoint(TxId(HashFromByte(i)), 0),
                      UTXOEntry(AmountUna::Una(10'000 + i),
                                std::vector<uint8_t>{0x51, i}, 1, false));
    }

    ConsensusUTXOSet pre;
    ASSERT_TRUE(pre.BulkLoad(utxos, 9, HashFromByte(0x09)));
    EXPECT_FALSE(pre.GetForest().isCanonicalEmptyRoots())
        << "bulk load at height 9 (pre-activation) enabled canonical mode";

    ConsensusUTXOSet post;
    ASSERT_TRUE(post.BulkLoad(utxos, 10, HashFromByte(0x0a)));
    EXPECT_TRUE(post.GetForest().isCanonicalEmptyRoots())
        << "bulk load at height 10 (activation) did not enable canonical mode";

    ConsensusUTXOSet later;
    ASSERT_TRUE(later.BulkLoad(utxos, 5000, HashFromByte(0x50)));
    EXPECT_TRUE(later.GetForest().isCanonicalEmptyRoots())
        << "bulk load well past activation did not enable canonical mode";
}

// ---------------------------------------------------------------------------
// Differential: prove()+remove() vs removeAtKnownPosition(), byte-for-byte,
// with DELIBERATELY DRAINED subtrees -- the construction Stage 3 changed.
// ---------------------------------------------------------------------------

struct ForestState {
    std::string serialized;
    std::string commitment;
    uint64_t leaves = 0;
    bool canonical = false;

    bool operator==(const ForestState& o) const {
        return serialized == o.serialized && commitment == o.commitment &&
               leaves == o.leaves && canonical == o.canonical;
    }
};

ForestState Capture(const UtreexoForest& forest) {
    ForestState fs;
    fs.serialized = HexOf(forest.serialize());
    fs.commitment = HexOf(forest.getCommitment());
    fs.leaves = forest.getNumLeaves();
    fs.canonical = forest.isCanonicalEmptyRoots();
    return fs;
}

UtreexoHash LeafFromByte(uint8_t seed) {
    return UtreexoHash(32, seed);
}

// Two forests, identical construction, differing ONLY in removal primitive.
// Every live leaf is then deleted, draining subtrees to empty, and new leaves
// added on top. States must stay byte-identical throughout.
TEST_F(UtxoSetCanonicalActivation,
       ProofAndTrustedRemovalAgreeByteForByteOnDrainedSubtrees) {
    for (const bool canonical : {false, true}) {
        SCOPED_TRACE(canonical ? "canonical mode (post-activation)"
                               : "legacy mode (pre-activation)");
        UtreexoForest via_proof;
        UtreexoForest via_trusted;
        via_proof.setCanonicalEmptyRoots(canonical);
        via_trusted.setCanonicalEmptyRoots(canonical);

        // Build a forest whose leaf count is a power of two, so subtrees are
        // full and can be drained completely.
        constexpr uint8_t kLeaves = 8;
        std::vector<UtreexoHash> leaves;
        for (uint8_t i = 0; i < kLeaves; ++i) {
            leaves.push_back(LeafFromByte(static_cast<uint8_t>(0x10 + i)));
            ASSERT_NE(via_proof.add(leaves.back()), UINT64_MAX);
            ASSERT_NE(via_trusted.add(leaves.back()), UINT64_MAX);
        }
        ASSERT_EQ(Capture(via_proof), Capture(via_trusted))
            << "forests diverged before any removal";

        // Drain EVERY leaf. The final removals empty whole subtrees, which is
        // exactly the case Stage 3 changed.
        for (uint8_t i = 0; i < kLeaves; ++i) {
            const auto pos_proof = via_proof.findLeafPosition(leaves[i]);
            const auto pos_trusted = via_trusted.findLeafPosition(leaves[i]);
            ASSERT_TRUE(pos_proof.has_value()) << "leaf " << int(i) << " missing";
            ASSERT_EQ(pos_proof, pos_trusted) << "positions diverged at leaf " << int(i);

            const auto proof = via_proof.prove(*pos_proof);
            ASSERT_TRUE(proof.has_value())
                << "prove() failed for a live leaf at position " << *pos_proof;
            EXPECT_TRUE(via_proof.remove(leaves[i], *proof))
                << "proof-based removal failed at leaf " << int(i)
                << " (canonical=" << canonical << ")";
            EXPECT_TRUE(via_trusted.removeAtKnownPosition(*pos_trusted, leaves[i]))
                << "trusted removal failed at leaf " << int(i);

            ASSERT_EQ(Capture(via_proof), Capture(via_trusted))
                << "states diverged after removing leaf " << int(i);
        }

        // Subtrees are now fully drained. Adding on top is where the pre-fix
        // nullopt-vs-sentinel difference cascaded.
        for (uint8_t i = 0; i < 4; ++i) {
            const UtreexoHash fresh = LeafFromByte(static_cast<uint8_t>(0xA0 + i));
            ASSERT_NE(via_proof.add(fresh), UINT64_MAX);
            ASSERT_NE(via_trusted.add(fresh), UINT64_MAX);
            ASSERT_EQ(Capture(via_proof), Capture(via_trusted))
                << "states diverged after post-drain add " << int(i);
        }
    }
}

// Per-mutation invariants on a drained-and-refilled forest.
TEST_F(UtxoSetCanonicalActivation, DrainedForestHoldsInvariantsUnderCanonicalMode) {
    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);

    std::vector<UtreexoHash> live;
    for (uint8_t i = 0; i < 8; ++i) {
        const UtreexoHash leaf = LeafFromByte(static_cast<uint8_t>(0x30 + i));
        ASSERT_NE(forest.add(leaf), UINT64_MAX);
        live.push_back(leaf);
    }

    const auto check_invariants = [&](const std::string& context) {
        // roots-slot / numLeaves invariant -- the one Stage 3 restored.
        const auto indexed = forest.getIndexedRoots();
        const uint64_t n = forest.getNumLeaves();
        for (size_t h = 0; h < indexed.size(); ++h) {
            const bool bit = ((n >> h) & 1ULL) != 0;
            EXPECT_EQ(indexed[h].has_value(), bit)
                << context << ": roots_[" << h << "].has_value()="
                << indexed[h].has_value() << " but bit " << h
                << " of numLeaves(" << n << ")=" << bit;
        }
        // Every still-live leaf must remain provable, and its proof verify.
        for (const auto& leaf : live) {
            const auto pos = forest.findLeafPosition(leaf);
            if (!pos.has_value()) {
                continue;  // already drained
            }
            const auto proof = forest.prove(*pos);
            ASSERT_TRUE(proof.has_value())
                << context << ": prove() failed for live leaf at " << *pos;
            EXPECT_TRUE(proof->verify(leaf, forest.getRoots()))
                << context << ": proof did not verify for live leaf at " << *pos;
        }
        // serialize -> deserialize must round-trip, and must not move the
        // commitment.
        const auto bytes = forest.serialize();
        const UtreexoForest reloaded = UtreexoForest::deserialize(bytes);
        EXPECT_EQ(HexOf(reloaded.serialize()), HexOf(bytes))
            << context << ": serialize/deserialize is not byte-identical";
        EXPECT_EQ(HexOf(reloaded.getCommitment()), HexOf(forest.getCommitment()))
            << context << ": commitment changed across a round trip";
    };

    check_invariants("initial");

    // Drain to empty, checking after every mutation.
    for (size_t i = 0; i < live.size(); ++i) {
        const auto pos = forest.findLeafPosition(live[i]);
        ASSERT_TRUE(pos.has_value());
        ASSERT_TRUE(forest.removeAtKnownPosition(*pos, live[i]));
        check_invariants("after draining leaf " + std::to_string(i));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }

    // Refill on top of fully drained subtrees.
    for (uint8_t i = 0; i < 6; ++i) {
        const UtreexoHash fresh = LeafFromByte(static_cast<uint8_t>(0xB0 + i));
        ASSERT_NE(forest.add(fresh), UINT64_MAX);
        live.push_back(fresh);
        check_invariants("after post-drain add " + std::to_string(i));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

}  // namespace
