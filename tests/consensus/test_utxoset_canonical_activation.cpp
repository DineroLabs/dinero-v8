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
#include <cstdlib>
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

// Mainnet activation boundary. This pins the HEIGHT LOGIC only.
//
// It deliberately does NOT claim to verify anything about the live chain: the
// fleet's commitment and leaf/root counts are recorded in issue #490, not here,
// because a comment in a test file is not an assertion and would rot silently.
//
// Note also that popcount(num_leaves) == num_roots does NOT prove canonical
// mode is enabled -- a healthy LEGACY forest satisfies the same relation
// whenever no root subtree has been fully drained. The two modes only diverge
// once a subtree empties. That relation is consistency evidence, not a mode
// oracle.
TEST(UtxoSetCanonicalActivationMainnet, MainnetActivationBoundaryIs2870) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(GetUtreexoCanonicalRootsActivationHeight(), 2870U)
        << "mainnet activation height moved; the live observations below no "
           "longer describe the same fork";
    EXPECT_FALSE(IsUtreexoCanonicalRootsActive(2869));
    EXPECT_TRUE(IsUtreexoCanonicalRootsActive(2870));
    EXPECT_TRUE(IsUtreexoCanonicalRootsActive(77700))
        << "a height well past activation must derive canonical mode";
    SelectParams(Chain::REGTEST);
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

// Legacy V2 snapshot import at a POST-activation height.
//
// This is the live case that actually matters for BulkLoad. Normal startup does
// NOT keep BulkLoad's forest -- chainstate_service.cpp:2164 discards it
// immediately, because BulkLoad rebuilds sorted by OutPoint rather than in
// chronological insertion order, and the real forest comes from checkpoint
// deserialization or block replay. The consumer that DOES keep it is snapshot
// import (LoadSnapshot / assumeutxo, chainstate_service.cpp:9196).
//
// A v2 payload carries no canonical-mode flag, so deserialize() defaults it to
// false -- the pre-fork mode it was written under. Importing such a snapshot at
// a post-activation height therefore yields a forest in the WRONG mode unless
// something derives the mode from the height.
TEST_F(UtxoSetCanonicalActivation, LegacyV2PayloadImportedPostActivationNeedsHeightDerivedMode) {
    // Build a forest in canonical (post-activation) mode.
    UtreexoForest original;
    original.setCanonicalEmptyRoots(true);
    for (uint8_t i = 0; i < 8; ++i) {
        ASSERT_NE(original.add(UtreexoHash(32, static_cast<uint8_t>(0x40 + i))),
                  UINT64_MAX);
    }
    const std::string canonical_commitment = HexOf(original.getCommitment());

    // Emit a genuine v2 payload via the debug injection knob, so this exercises
    // the real legacy format rather than a hand-built approximation.
    ::setenv("DINERO_FOREST_SERIALIZE_LEGACY_V2", "1", 1);
    const auto v2_bytes = original.serialize();
    ::unsetenv("DINERO_FOREST_SERIALIZE_LEGACY_V2");

    const auto v3_bytes = original.serialize();
    ASSERT_NE(HexOf(v2_bytes), HexOf(v3_bytes))
        << "the v2 injection knob produced a v3 payload; this test would be "
           "checking nothing";

    // Importing the v2 payload loses the flag: it defaults to legacy.
    UtreexoForest imported = UtreexoForest::deserialize(v2_bytes);
    ASSERT_GT(imported.getNumLeaves(), 0U)
        << "v2 payload failed to deserialize at all";
    EXPECT_FALSE(imported.isCanonicalEmptyRoots())
        << "a v2 payload carries no flag; deserialize must default to legacy";

    // Deriving the mode from a post-activation height repairs it. This is what
    // BulkLoad now does, and what snapshot import relies on.
    SelectParams(Chain::MAINNET);
    const uint32_t post_activation_height = 77'700;
    ASSERT_TRUE(IsUtreexoCanonicalRootsActive(post_activation_height));
    imported.setCanonicalEmptyRoots(true);
    imported.rebuildRoots();
    SelectParams(Chain::REGTEST);

    EXPECT_TRUE(imported.isCanonicalEmptyRoots());
    EXPECT_EQ(HexOf(imported.getCommitment()), canonical_commitment)
        << "commitment did not return to the canonical value after applying "
           "height-derived mode to an imported v2 payload";
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
        std::vector<UtreexoHash> refilled;
        for (uint8_t i = 0; i < 4; ++i) {
            const UtreexoHash fresh = LeafFromByte(static_cast<uint8_t>(0xA0 + i));
            refilled.push_back(fresh);
            ASSERT_NE(via_proof.add(fresh), UINT64_MAX);
            ASSERT_NE(via_trusted.add(fresh), UINT64_MAX);
            ASSERT_EQ(Capture(via_proof), Capture(via_trusted))
                << "states diverged after post-drain add " << int(i);
        }

        // REMOVE AGAIN after drain-and-refill. This is the decisive sequence:
        // pre-fix, the drained subtree left roots_ in a shape that made the
        // following add() take the "place" branch instead of "merge", and it
        // was the NEXT proof-based removal that then failed to verify. A test
        // that stopped at the refill would miss exactly that cascade.
        for (size_t i = 0; i < refilled.size(); ++i) {
            const auto pos_proof = via_proof.findLeafPosition(refilled[i]);
            const auto pos_trusted = via_trusted.findLeafPosition(refilled[i]);
            ASSERT_TRUE(pos_proof.has_value())
                << "refilled leaf " << i << " not found after re-add";
            ASSERT_EQ(pos_proof, pos_trusted)
                << "positions diverged for refilled leaf " << i;

            const auto proof = via_proof.prove(*pos_proof);
            ASSERT_TRUE(proof.has_value())
                << "prove() failed for a refilled leaf at " << *pos_proof;
            EXPECT_TRUE(proof->verify(refilled[i], via_proof.getRoots()))
                << "a proof generated after drain-and-refill did not verify "
                   "(canonical=" << canonical << ") -- this is the exact "
                   "pre-Stage-3 failure";
            EXPECT_TRUE(via_proof.remove(refilled[i], *proof))
                << "proof-based removal failed after drain-and-refill at leaf " << i;
            EXPECT_TRUE(via_trusted.removeAtKnownPosition(*pos_trusted, refilled[i]))
                << "trusted removal failed after drain-and-refill at leaf " << i;

            ASSERT_EQ(Capture(via_proof), Capture(via_trusted))
                << "states diverged removing refilled leaf " << i;
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

    const auto check_invariants = [&](const std::string& context,
                                     size_t expected_live) {
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
        // Root slots BEYOND indexed.size() must correspond to unset bits. A
        // shorter roots_ vector than numLeaves_ requires would otherwise slip
        // past the loop above entirely.
        for (size_t h = indexed.size(); h < 64; ++h) {
            EXPECT_EQ(((n >> h) & 1ULL), 0ULL)
                << context << ": numLeaves(" << n << ") has bit " << h
                << " set but roots_ only has " << indexed.size() << " slots";
        }

        // Every still-live leaf must remain provable and its proof verify.
        //
        // `live` is filtered explicitly rather than skipping absent leaves with
        // a bare continue: a silent skip would let this loop assert nothing at
        // all if findLeafPosition stopped finding anything, and the test would
        // still pass.
        size_t checked = 0;
        for (const auto& leaf : live) {
            const auto pos = forest.findLeafPosition(leaf);
            if (!pos.has_value()) {
                continue;  // drained earlier in this test, by construction
            }
            ++checked;
            const auto proof = forest.prove(*pos);
            ASSERT_TRUE(proof.has_value())
                << context << ": prove() failed for live leaf at " << *pos;
            EXPECT_TRUE(proof->verify(leaf, forest.getRoots()))
                << context << ": proof did not verify for live leaf at " << *pos;
        }
        EXPECT_EQ(checked, expected_live)
            << context << ": expected " << expected_live
            << " live leaves but found " << checked
            << " -- the provability loop is not covering what it claims";

        // serialize -> deserialize must round-trip, must not move the
        // commitment, AND the reloaded forest must still serve valid proofs
        // for every live leaf. Byte equality alone would not catch a forest
        // that reloads identically but can no longer prove membership.
        const auto bytes = forest.serialize();
        const UtreexoForest reloaded = UtreexoForest::deserialize(bytes);
        EXPECT_EQ(HexOf(reloaded.serialize()), HexOf(bytes))
            << context << ": serialize/deserialize is not byte-identical";
        EXPECT_EQ(HexOf(reloaded.getCommitment()), HexOf(forest.getCommitment()))
            << context << ": commitment changed across a round trip";
        size_t reloaded_checked = 0;
        for (const auto& leaf : live) {
            const auto pos = reloaded.findLeafPosition(leaf);
            if (!pos.has_value()) {
                continue;
            }
            ++reloaded_checked;
            const auto proof = reloaded.prove(*pos);
            ASSERT_TRUE(proof.has_value())
                << context << ": prove() failed after deserialize at " << *pos;
            EXPECT_TRUE(proof->verify(leaf, reloaded.getRoots()))
                << context << ": proof did not verify after deserialize at " << *pos;
        }
        EXPECT_EQ(reloaded_checked, expected_live)
            << context << ": deserialized forest lost live leaves ("
            << reloaded_checked << " vs " << expected_live << ")";
    };

    check_invariants("initial", live.size());

    // Drain to empty, checking after every mutation.
    for (size_t i = 0; i < live.size(); ++i) {
        const auto pos = forest.findLeafPosition(live[i]);
        ASSERT_TRUE(pos.has_value());
        ASSERT_TRUE(forest.removeAtKnownPosition(*pos, live[i]));
        check_invariants("after draining leaf " + std::to_string(i),
                         live.size() - (i + 1));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }

    // Refill on top of fully drained subtrees.
    for (uint8_t i = 0; i < 6; ++i) {
        const UtreexoHash fresh = LeafFromByte(static_cast<uint8_t>(0xB0 + i));
        ASSERT_NE(forest.add(fresh), UINT64_MAX);
        live.push_back(fresh);
        check_invariants("after post-drain add " + std::to_string(i),
                         static_cast<size_t>(i) + 1);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

}  // namespace
