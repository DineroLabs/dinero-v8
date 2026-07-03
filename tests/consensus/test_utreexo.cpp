/**
 * Utreexo Comprehensive Test Suite
 *
 * Tests complete Utreexo implementation including:
 * - Forest operations (add, remove, prove)
 * - Proof generation and verification
 * - Batched proofs
 * - Serialization/deserialization
 * - Reorg safety
 * - Stateless validation
 * - Edge cases
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_activation.h"
#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "crypto/hash.h"
#include <algorithm>
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <vector>
#include <string>
#include <iomanip>

#ifdef _WIN32
// _putenv_s sets ENV=VALUE; passing an empty value unsets per CRT docs.
static inline int dinero_setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int dinero_unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#define setenv   dinero_setenv
#define unsetenv dinero_unsetenv
#endif

using namespace dinero;
using namespace dinero::consensus;

// Helper: Print hash (first 8 bytes)
void printHash(const UtreexoHash& hash) {
    for (size_t i = 0; i < std::min(size_t(8), hash.size()); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    std::cout << std::dec << "...";
}

// Helper: Create test hash from integer
UtreexoHash makeTestHash(uint64_t value) {
    UtreexoHash hash(32, 0);
    for (int i = 0; i < 8; i++) {
        hash[i] = (value >> (i * 8)) & 0xFF;
    }
    return hash;
}

// Helper: Create UTXO leaf hash
UtreexoHash makeUTXOHash(uint64_t utxo_id, uint64_t value) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &utxo_id, sizeof(utxo_id));

    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14}; // P2PKH prefix
    for (int i = 0; i < 20; i++) {
        scriptPubKey.push_back((utxo_id >> (i % 8)) & 0xFF);
    }
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    return HashUTXOLegacy(txid, 0, value, scriptPubKey);
}

//=============================================================================
// Test 1: Basic Forest Operations
//=============================================================================

void testBasicForestOperations() {
    std::cout << "\n[Test 1] Basic forest operations..." << std::endl;

    UtreexoForest forest;

    // Initially empty
    assert(forest.isEmpty());
    assert(forest.getNumLeaves() == 0);
    assert(forest.getNumRoots() == 0);
    std::cout << "  ✅ Empty forest initialized correctly" << std::endl;

    // Add first leaf
    UtreexoHash leaf1 = makeTestHash(1);
    uint64_t pos1 = forest.add(leaf1);
    assert(pos1 == 0);
    assert(forest.getNumLeaves() == 1);
    assert(forest.getNumRoots() == 1);
    std::cout << "  ✅ Added first leaf (pos=" << pos1 << ")" << std::endl;

    // Add second leaf (should merge into tree of 2)
    UtreexoHash leaf2 = makeTestHash(2);
    uint64_t pos2 = forest.add(leaf2);
    assert(pos2 == 1);
    assert(forest.getNumLeaves() == 2);
    assert(forest.getNumRoots() == 1); // Merged into single root
    std::cout << "  ✅ Added second leaf (pos=" << pos2 << "), roots=" << forest.getNumRoots() << std::endl;

    // Add third leaf
    UtreexoHash leaf3 = makeTestHash(3);
    uint64_t pos3 = forest.add(leaf3);
    assert(pos3 == 2);
    assert(forest.getNumLeaves() == 3);
    assert(forest.getNumRoots() == 2); // 3 = 2 + 1, so 2 roots
    std::cout << "  ✅ Added third leaf (pos=" << pos3 << "), roots=" << forest.getNumRoots() << std::endl;

    // Add fourth leaf
    UtreexoHash leaf4 = makeTestHash(4);
    uint64_t pos4 = forest.add(leaf4);
    assert(pos4 == 3);
    assert(forest.getNumLeaves() == 4);
    assert(forest.getNumRoots() == 1); // 4 = 4, so 1 root (perfect tree)
    std::cout << "  ✅ Added fourth leaf (pos=" << pos4 << "), roots=" << forest.getNumRoots() << std::endl;

    // Commitment should be deterministic
    UtreexoHash commitment = forest.getCommitment();
    assert(commitment.size() == 32);
    assert(!commitment.empty());
    std::cout << "  ✅ Commitment: ";
    printHash(commitment);
    std::cout << std::endl;

    std::cout << "✅ Test 1 passed: Basic forest operations work correctly" << std::endl;
}

//=============================================================================
// Test 2: Proof Generation
//=============================================================================

void testProofGeneration() {
    std::cout << "\n[Test 2] Proof generation..." << std::endl;

    UtreexoForest forest;

    // Add 4 leaves
    std::vector<UtreexoHash> leaves;
    for (int i = 0; i < 4; i++) {
        UtreexoHash leaf = makeTestHash(100 + i);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    std::cout << "  Added 4 leaves to forest" << std::endl;

    // Generate proof for each leaf
    for (size_t i = 0; i < leaves.size(); i++) {
        auto proof_opt = forest.prove(i);
        assert(proof_opt.has_value());

        const UtreexoProof& proof = proof_opt.value();
        assert(proof.position == i);
        assert(proof.numLeaves == 4);

        std::cout << "  ✅ Generated proof for position " << i
                  << " (siblings=" << proof.siblings.size() << ")" << std::endl;
    }

    // Proof for invalid position
    auto invalid_proof = forest.prove(999);
    assert(!invalid_proof.has_value());
    std::cout << "  ✅ Invalid position correctly returns nullopt" << std::endl;

    std::cout << "✅ Test 2 passed: Proof generation works correctly" << std::endl;
}

//=============================================================================
// Test 3: Proof Verification
//=============================================================================

void testProofVerification() {
    std::cout << "\n[Test 3] Proof verification..." << std::endl;

    UtreexoForest forest;

    // Add 8 leaves for better testing
    std::vector<UtreexoHash> leaves;
    for (int i = 0; i < 8; i++) {
        UtreexoHash leaf = makeTestHash(200 + i);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    std::cout << "  Added 8 leaves to forest" << std::endl;

    // Get current roots
    std::vector<UtreexoHash> roots = forest.getRoots();
    std::cout << "  Forest has " << roots.size() << " root(s)" << std::endl;

    // Verify each leaf
    size_t verified_count = 0;
    for (size_t i = 0; i < leaves.size(); i++) {
        auto proof_opt = forest.prove(i);
        assert(proof_opt.has_value());

        const UtreexoProof& proof = proof_opt.value();
        bool valid = proof.verify(leaves[i], roots);
        assert(valid);
        verified_count++;
    }

    std::cout << "  ✅ All " << verified_count << " proofs verified successfully" << std::endl;

    // Test invalid proof (wrong leaf hash)
    auto proof_opt = forest.prove(0);
    assert(proof_opt.has_value());
    UtreexoHash wrong_leaf = makeTestHash(999);
    bool invalid_result = proof_opt.value().verify(wrong_leaf, roots);
    assert(!invalid_result);
    std::cout << "  ✅ Invalid leaf hash correctly fails verification" << std::endl;

    std::cout << "✅ Test 3 passed: Proof verification works correctly" << std::endl;
}

//=============================================================================
// Test 4: Batched Proof Generation
//=============================================================================

void testBatchedProofGeneration() {
    std::cout << "\n[Test 4] Batched proof generation..." << std::endl;

    UtreexoForest forest;

    // Add 16 leaves
    std::vector<UtreexoHash> all_leaves;
    for (int i = 0; i < 16; i++) {
        UtreexoHash leaf = makeTestHash(300 + i);
        forest.add(leaf);
        all_leaves.push_back(leaf);
    }

    std::cout << "  Added 16 leaves to forest" << std::endl;

    // Select subset of leaves to prove (simulate spending UTXOs)
    std::vector<UtreexoHash> targets = {
        all_leaves[0],
        all_leaves[5],
        all_leaves[10],
        all_leaves[15]
    };

    std::cout << "  Selected " << targets.size() << " targets for batch proof" << std::endl;

    // Generate batched proof
    std::vector<UtreexoHash> proof_hashes = forest.generateBatchProof(targets);

    std::cout << "  ✅ Generated batch proof with " << proof_hashes.size()
              << " proof hashes (deduplicated)" << std::endl;

    // Proof size should be reasonable (log n per target, with deduplication)
    size_t max_expected = targets.size() * 8; // Very generous upper bound
    assert(proof_hashes.size() <= max_expected);
    std::cout << "  ✅ Proof size reasonable (≤ " << max_expected << ")" << std::endl;

    // Empty targets should produce empty proof
    std::vector<UtreexoHash> empty_targets;
    std::vector<UtreexoHash> empty_proof = forest.generateBatchProof(empty_targets);
    assert(empty_proof.empty());
    std::cout << "  ✅ Empty targets correctly produce empty proof" << std::endl;

    std::cout << "✅ Test 4 passed: Batched proof generation works correctly" << std::endl;
}

//=============================================================================
// Test 5: Batched Proof Verification
//=============================================================================

void testBatchedProofVerification() {
    std::cout << "\n[Test 5] Batched proof verification..." << std::endl;

    UtreexoForest forest;

    // Add 32 leaves
    std::vector<UtreexoHash> all_leaves;
    for (int i = 0; i < 32; i++) {
        UtreexoHash leaf = makeTestHash(400 + i);
        forest.add(leaf);
        all_leaves.push_back(leaf);
    }

    std::cout << "  Added 32 leaves to forest" << std::endl;

    // Test Case 1: Verify subset of leaves
    std::vector<UtreexoHash> targets1 = {
        all_leaves[0],
        all_leaves[7],
        all_leaves[15],
        all_leaves[23],
        all_leaves[31]
    };

    std::vector<UtreexoHash> proof_hashes1 = forest.generateBatchProof(targets1);
    bool valid1 = forest.verifyBatchProof(targets1, proof_hashes1);
    assert(valid1);
    std::cout << "  ✅ Verified batch of " << targets1.size() << " targets" << std::endl;

    // Test Case 2: Verify all leaves
    std::vector<UtreexoHash> proof_hashes_all = forest.generateBatchProof(all_leaves);
    bool valid_all = forest.verifyBatchProof(all_leaves, proof_hashes_all);
    assert(valid_all);
    std::cout << "  ✅ Verified all " << all_leaves.size() << " leaves" << std::endl;

    // Test Case 3: Empty proof
    std::vector<UtreexoHash> empty_targets;
    std::vector<UtreexoHash> empty_proof;
    bool valid_empty = forest.verifyBatchProof(empty_targets, empty_proof);
    assert(valid_empty);
    std::cout << "  ✅ Empty proof correctly validates" << std::endl;

    // Test Case 4: Invalid target (not in forest)
    std::vector<UtreexoHash> invalid_targets = {makeTestHash(9999)};
    std::vector<UtreexoHash> invalid_proof;
    bool invalid_result = forest.verifyBatchProof(invalid_targets, invalid_proof);
    assert(!invalid_result);
    std::cout << "  ✅ Invalid target correctly fails verification" << std::endl;

    // Test Case 5: Missing proof hashes
    std::vector<UtreexoHash> insufficient_proof = {proof_hashes1[0]}; // Only partial proof
    bool insufficient_result = forest.verifyBatchProof(targets1, insufficient_proof);
    assert(!insufficient_result);
    std::cout << "  ✅ Insufficient proof correctly fails verification" << std::endl;

    std::cout << "✅ Test 5 passed: Batched proof verification works correctly" << std::endl;
}

//=============================================================================
// Test 6: Serialization and Deserialization
//=============================================================================

void testSerialization() {
    std::cout << "\n[Test 6] Serialization/deserialization..." << std::endl;

    UtreexoForest original;

    // Add some leaves
    std::vector<UtreexoHash> leaves;
    for (int i = 0; i < 7; i++) {
        UtreexoHash leaf = makeTestHash(500 + i);
        original.add(leaf);
        leaves.push_back(leaf);
    }

    std::cout << "  Original forest: " << original.getNumLeaves() << " leaves, "
              << original.getNumRoots() << " roots" << std::endl;

    UtreexoHash original_commitment = original.getCommitment();

    // Serialize
    std::vector<uint8_t> serialized = original.serialize();
    std::cout << "  Serialized to " << serialized.size() << " bytes" << std::endl;

    // Deserialize
    UtreexoForest restored = UtreexoForest::deserialize(serialized);

    std::cout << "  Restored forest: " << restored.getNumLeaves() << " leaves, "
              << restored.getNumRoots() << " roots" << std::endl;

    // Verify state matches
    assert(restored.getNumLeaves() == original.getNumLeaves());
    assert(restored.getNumRoots() == original.getNumRoots());

    UtreexoHash restored_commitment = restored.getCommitment();
    assert(restored_commitment == original_commitment);
    std::cout << "  ✅ Commitments match after deserialization" << std::endl;

    // Verify proofs still work after deserialization
    for (size_t i = 0; i < leaves.size(); i++) {
        auto proof_opt = restored.prove(i);
        assert(proof_opt.has_value());

        std::cout << "  Verifying leaf " << i << ": ";
        printHash(leaves[i]);
        std::cout << "    Proof has " << proof_opt.value().siblings.size() << " siblings" << std::endl;
        std::cout << "    Restored forest has " << restored.getNumRoots() << " roots" << std::endl;

        // Try generating proof from original too
        auto orig_proof_opt = original.prove(i);
        std::cout << "    Original proof has " << (orig_proof_opt.has_value() ? orig_proof_opt.value().siblings.size() : 0) << " siblings" << std::endl;

        bool valid = proof_opt.value().verify(leaves[i], restored.getRoots());
        if (!valid) {
            std::cout << "    ❌ PROOF VERIFICATION FAILED" << std::endl;
            std::cout << "    Leaf hash: ";
            printHash(leaves[i]);
            std::cout << "    Original roots:" << std::endl;
            for (const auto& r : original.getRoots()) {
                std::cout << "      ";
                printHash(r);
            }
            std::cout << "    Restored roots:" << std::endl;
            for (const auto& r : restored.getRoots()) {
                std::cout << "      ";
                printHash(r);
            }
            std::cout << "    Testing if original proof verifies against original roots..." << std::endl;
            if (orig_proof_opt.has_value()) {
                bool orig_valid = orig_proof_opt.value().verify(leaves[i], original.getRoots());
                std::cout << "      Original -> Original: " << (orig_valid ? "YES" : "NO") << std::endl;
            }
        }
        assert(valid);
    }
    std::cout << "  ✅ All " << leaves.size() << " proofs still valid after restore" << std::endl;

    // Test batch proof generation after restore
    std::vector<UtreexoHash> targets = {leaves[0], leaves[3], leaves[6]};
    std::vector<UtreexoHash> proof_hashes = restored.generateBatchProof(targets);
    bool batch_valid = restored.verifyBatchProof(targets, proof_hashes);
    assert(batch_valid);
    std::cout << "  ✅ Batch proof generation/verification works after restore" << std::endl;

    std::cout << "✅ Test 6 passed: Serialization preserves complete state" << std::endl;
}

//=============================================================================
// Test 6b: canonical_empty_roots_ flag survives serialize/deserialize
//=============================================================================
// Pre-2026-04-28: serialize() omitted canonical_empty_roots_. A forest
// in canonical mode (post-fork) round-tripped through deserialize with
// the flag silently reset to false; deserialize's own rebuildRoots()
// then recomputed roots under the legacy rule and produced nullopt
// where the stored roots held ZERO_HASH for fully-deleted subtrees.
// The post-rebuild self-check failed and silently returned an empty
// UtreexoForest. ConsensusUTXOSet::Restore's only feedback was a
// stderr warning, so the daemon proceeded with a wiped forest. v3
// prepends the flag byte; this test pins that.

void testCanonicalEmptyRootsRoundtrip() {
    std::cout << "\n[Test 6b] canonical_empty_roots_ survives serialize/deserialize..." << std::endl;

    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);

    // 4 leaves so we can fully drain the left subtree.
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 4; ++i) {
        leaves.push_back(makeUTXOHash(0xC1A0E + i, 100000000ULL));
        forest.add(leaves.back());
    }

    // Drain leaves 0 and 1 — that subtree becomes fully deleted; in
    // canonical mode its slot commits as ZERO_HASH instead of nullopt.
    auto pos0 = forest.findLeafPosition(leaves[0]);
    auto pos1 = forest.findLeafPosition(leaves[1]);
    assert(pos0.has_value() && pos1.has_value());
    auto proof0 = forest.prove(*pos0);
    auto proof1 = forest.prove(*pos1);
    assert(proof0.has_value() && proof1.has_value());
    assert(forest.remove(leaves[0], *proof0));
    assert(forest.remove(leaves[1], *proof1));

    assert(forest.isCanonicalEmptyRoots());
    UtreexoHash original_commitment = forest.getCommitment();
    std::cout << "  Built canonical-mode forest, 4 leaves, 2 deleted. Commitment: ";
    printHash(original_commitment);
    std::cout << std::endl;

    auto blob = forest.serialize();
    UtreexoForest restored = UtreexoForest::deserialize(blob);

    // (1) Flag must travel with the payload.
    assert(restored.isCanonicalEmptyRoots());
    std::cout << "  ✅ canonical_empty_roots_ flag preserved" << std::endl;

    // (2) Forest must NOT have been silently wiped (the v2 bug shape).
    assert(restored.getNumLeaves() == forest.getNumLeaves());
    std::cout << "  ✅ Restored forest retains numLeaves (v2 bug regressed: would be 0)" << std::endl;

    // (3) Commitment must match exactly — proves the deserialize-time
    //     rebuildRoots() now runs under the correct flag.
    assert(restored.getCommitment() == original_commitment);
    std::cout << "  ✅ Commitment matches after roundtrip" << std::endl;

    // (4) Negative assurance: a flag-off forest stays flag-off across
    //     the same path.
    UtreexoForest legacy;
    legacy.add(makeUTXOHash(0x1eaca7, 1));
    assert(!legacy.isCanonicalEmptyRoots());
    UtreexoForest legacy_restored = UtreexoForest::deserialize(legacy.serialize());
    assert(!legacy_restored.isCanonicalEmptyRoots());
    assert(legacy_restored.getNumLeaves() == 1);
    std::cout << "  ✅ Legacy (flag-off) forest also roundtrips correctly" << std::endl;

    std::cout << "✅ Test 6b passed: canonical_empty_roots_ flag survives serialize/deserialize" << std::endl;
}

//=============================================================================
// Test 6c: audit gap #10b — v2-serialized flag=true forest with a fully-
// deleted subtree must NOT silently wipe when read by the v3 deserializer.
//=============================================================================
// The 2026-04-28 LA failure happened because:
//   1. live forest had canonical_empty_roots_ = true (post-fork)
//   2. some subtree had been fully drained → its root cell held ZERO_HASH
//      (canonical-zero-sentinel from computeSubtreeHash)
//   3. forest serialized under v2 — v2 carries no flag byte
//   4. on deserialize the flag defaulted to false
//   5. the post-load rebuildRoots() recomputed empty subtrees as nullopt
//      (legacy rule), not ZERO_HASH
//   6. comparison stored_roots vs rebuilt roots failed
//   7. deserialize returned a default-constructed (empty) UtreexoForest
//   8. ConsensusUTXOSet::Restore logged a stderr WARNING and proceeded
//      with the wiped forest
//
// Commit a72053a9a closed step 4 for v3-on-v3 round-trips: writers stamp
// the flag byte. The hypothesis going into this test was that v2 payloads
// (no flag byte) might still trigger the silent-wipe path under the v3
// reader if the source had flag=true with a fully-deleted root. Empirical
// finding: as of 2026-04-28, the v3 deserializer's self-check + rebuild
// path correctly recovers the forest for the fully-deleted-root case
// covered here (size-1 tree at position 4, fully drained). Either the
// stored ZERO_HASH cell roundtrips byte-equal through rebuildRoots() under
// flag=false in this specific shape, or the self-check tolerates the
// mismatch in a way the static code-trace missed. Whichever it is, this
// test locks that behavior in: any future change to deserialize that
// breaks v2-payload recovery on fully-deleted roots fails this test loud
// rather than silently re-introducing the LA-9291 symptom.
//
// Open: this test does NOT cover all fully-deleted-root shapes. It uses
// numLeaves=5 (binary 101) with the size-1 tree drained. Wider coverage
// (size-2, size-4 fully-deleted trees, multiple drained trees) is
// tracked under audit sub-gap #10c if any future investigation suggests
// the v2-payload recovery has a shape where it still fails.
//=============================================================================

void testCanonicalEmptyRootsLegacyV2Recovery() {
    std::cout << "\n[Test 6c] v2-payload deserialize with fully-deleted "
                 "root must not silently wipe..." << std::endl;

    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);

    // The bug fires when an ENTIRE root tree is fully deleted (every
    // leaf under one of numLeaves' set bits is removed). For numLeaves
    // = 5 (binary 101) the forest holds two trees: one of size 4
    // (positions 0-3) and one of size 1 (position 4). Drain the
    // size-1 tree by removing the single leaf at position 4 — its
    // root cell goes to ZERO_HASH under flag=true and nullopt under
    // flag=false. The bug shape is exactly that root cell being
    // recomputed wrong by a v3 reader handed a v2 payload.
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 5; ++i) {
        leaves.push_back(makeUTXOHash(0xA1ACE + i, 100000000ULL));
        forest.add(leaves.back());
    }

    auto pos4 = forest.findLeafPosition(leaves[4]);
    assert(pos4.has_value());
    auto proof4 = forest.prove(*pos4);
    assert(proof4.has_value());
    assert(forest.remove(leaves[4], *proof4));
    assert(forest.isCanonicalEmptyRoots());

    UtreexoHash original_commitment = forest.getCommitment();

    // Force v2 serialization (the audit gap #10 debug knob).
    setenv("DINERO_FOREST_SERIALIZE_LEGACY_V2", "1", /*overwrite=*/1);
    auto v2_blob = forest.serialize();
    unsetenv("DINERO_FOREST_SERIALIZE_LEGACY_V2");

    // First byte of the payload is the version byte.
    assert(!v2_blob.empty());
    assert(v2_blob[0] == 2 && "DINERO_FOREST_SERIALIZE_LEGACY_V2 should produce v2");
    std::cout << "  ✅ v2 payload produced via debug knob (" << v2_blob.size()
              << " bytes, version byte = " << static_cast<int>(v2_blob[0]) << ")"
              << std::endl;

    // Deserialize via the v3 reader (no env knob).
    UtreexoForest restored = UtreexoForest::deserialize(v2_blob);

    // The bug shape: deserialize returns an empty UtreexoForest (the
    // silent-wipe state). Property: restored forest must NOT be empty.
    if (restored.getNumLeaves() != forest.getNumLeaves()) {
        std::cout << "  ❌ silent wipe: restored numLeaves="
                  << restored.getNumLeaves()
                  << ", expected " << forest.getNumLeaves() << std::endl;
        std::cout << "  This is the LA-9291-style residual sub-gap #10b."
                  << std::endl;
        std::cout << "  v3 reader on v2 payload from a flag=true source with"
                  << " a fully-deleted subtree → silent forest wipe."
                  << std::endl;
        throw std::runtime_error("audit gap #10b: silent forest wipe on v2 payload");
    }
    std::cout << "  ✅ restored forest retained " << restored.getNumLeaves()
              << " leaves (no silent wipe)" << std::endl;

    if (restored.getCommitment() != original_commitment) {
        throw std::runtime_error("audit gap #10b: commitment mismatch on v2 payload");
    }
    std::cout << "  ✅ commitment matches across v2 → v3 reader roundtrip"
              << std::endl;
    std::cout << "✅ Test 6c passed: v3 reader recovers v2 flag=true payload"
              << std::endl;
}

//=============================================================================
// Test 6d: audit gap #10c — wider fully-deleted-root shape coverage.
//=============================================================================
// Test 6c covered exactly one shape: numLeaves=5 with the size-1 tree at
// position 4 fully drained. Audit row #10c was opened to cover the rest:
//
//   - size-2 root fully deleted (numLeaves=2 with both leaves drained)
//   - size-4 root fully deleted (numLeaves=4 with all four drained)
//   - multi-tree forests with multiple drained roots
//   - mixed half-drained subtrees
//
// Each shape exercises a different binary decomposition of numLeaves and
// a different combination of root cells holding ZERO_HASH (under
// canonical_empty_roots_=true) vs nullopt (under flag=false). If any
// shape has a configuration where v2 → v3 recovery silently wipes the
// forest, this parameterized test catches it.
//
// Property under test: for every shape, v2-serialize a flag=true forest
// that contains at least one fully-deleted root, deserialize via the v3
// reader, assert numLeaves and commitment round-trip byte-equal.
//=============================================================================

namespace {
struct V2RecoveryShape {
    std::string name;
    uint64_t total_leaves;
    std::vector<uint64_t> positions_to_delete;
};
}

void testCanonicalEmptyRootsLegacyV2RecoveryShapes() {
    std::cout << "\n[Test 6d] v2-payload deserialize: wider shape coverage..."
              << std::endl;

    // Each entry is a shape we want the v3 reader to handle without
    // silent wipe. Every shape contains AT LEAST one fully-deleted
    // root tree (so the canonical_empty_roots_ flag matters).
    const std::vector<V2RecoveryShape> shapes = {
        // Shape A: one size-1 root at numLeaves=1, leaf 0 drained.
        // Smallest possible fully-deleted-root configuration.
        {"size-1 root drained, numLeaves=1", 1, {0}},

        // Shape B: numLeaves=2 (one size-2 tree), both leaves drained.
        {"size-2 root drained, numLeaves=2", 2, {0, 1}},

        // Shape C: numLeaves=4 (one size-4 tree), all four drained.
        {"size-4 root drained, numLeaves=4", 4, {0, 1, 2, 3}},

        // Shape D: numLeaves=3 (size-2 + size-1), drain ONLY the size-1.
        // Mixed: one root drained, one root live.
        {"size-1 root drained while size-2 stays live, numLeaves=3", 3, {2}},

        // Shape E: numLeaves=5 (size-4 + size-1) — same as 6c, kept for parity.
        {"size-1 root drained alongside live size-4, numLeaves=5", 5, {4}},

        // Shape F: numLeaves=6 (size-4 + size-2), drain BOTH roots fully.
        {"both roots drained, numLeaves=6", 6, {0, 1, 2, 3, 4, 5}},

        // Shape G: numLeaves=7 (size-4 + size-2 + size-1), drain only the
        // size-1 and the size-2; size-4 stays live (mixed shape with two
        // fully-deleted roots and one live root).
        {"size-2 + size-1 drained while size-4 stays live, numLeaves=7", 7, {4, 5, 6}},
    };

    int passed = 0;
    for (const auto& shape : shapes) {
        UtreexoForest forest;
        forest.setCanonicalEmptyRoots(true);

        // Deterministic leaf hashes per position so this test is stable.
        std::vector<UtreexoHash> leaves;
        for (uint64_t i = 0; i < shape.total_leaves; ++i) {
            leaves.push_back(makeUTXOHash(0xC10C + i, 100000000ULL));
            forest.add(leaves.back());
        }

        // Drain the requested positions. order matters for proof
        // freshness — drain higher positions first so each drain's
        // proof is still valid against the live tree.
        std::vector<uint64_t> drain_order = shape.positions_to_delete;
        std::sort(drain_order.begin(), drain_order.end(),
                  [](uint64_t a, uint64_t b) { return a > b; });
        for (uint64_t pos : drain_order) {
            const auto& leaf = leaves[pos];
            auto p = forest.findLeafPosition(leaf);
            if (!p.has_value()) {
                throw std::runtime_error(shape.name +
                    ": findLeafPosition failed during fixture setup");
            }
            auto proof = forest.prove(*p);
            if (!proof.has_value()) {
                throw std::runtime_error(shape.name +
                    ": prove failed during fixture setup");
            }
            if (!forest.remove(leaf, *proof)) {
                throw std::runtime_error(shape.name +
                    ": remove failed during fixture setup");
            }
        }

        const UtreexoHash original_commitment = forest.getCommitment();
        const uint64_t original_num_leaves = forest.getNumLeaves();

        // v2-serialize via the audit gap #10 debug knob.
        setenv("DINERO_FOREST_SERIALIZE_LEGACY_V2", "1", /*overwrite=*/1);
        const auto v2_blob = forest.serialize();
        unsetenv("DINERO_FOREST_SERIALIZE_LEGACY_V2");
        if (v2_blob.empty() || v2_blob[0] != 2) {
            throw std::runtime_error(shape.name +
                ": v2 knob did not produce v2 payload");
        }

        // v3 reader.
        UtreexoForest restored = UtreexoForest::deserialize(v2_blob);

        if (restored.getNumLeaves() != original_num_leaves) {
            std::cout << "  ❌ [" << shape.name << "] silent wipe: "
                      << "restored numLeaves=" << restored.getNumLeaves()
                      << ", expected " << original_num_leaves << std::endl;
            throw std::runtime_error(shape.name +
                ": audit gap #10c — v2 payload silently wiped on this shape");
        }
        if (restored.getCommitment() != original_commitment) {
            std::cout << "  ❌ [" << shape.name << "] commitment mismatch"
                      << std::endl;
            throw std::runtime_error(shape.name +
                ": audit gap #10c — commitment mismatch on this shape");
        }

        std::cout << "  ✅ [" << shape.name << "] v2 → v3 roundtrip clean ("
                  << v2_blob.size() << " bytes)" << std::endl;
        ++passed;
    }

    std::cout << "✅ Test 6d passed: " << passed << " of "
              << shapes.size() << " shapes recovered cleanly" << std::endl;
}

//=============================================================================
// Test 7: Reorg Safety (Rollback)
//=============================================================================

void testReorgSafety() {
    std::cout << "\n[Test 7] Reorg safety (rollback)..." << std::endl;

    UtreexoForest forest;

    // Initial state: Add 5 leaves
    std::vector<UtreexoHash> initial_leaves;
    for (int i = 0; i < 5; i++) {
        UtreexoHash leaf = makeTestHash(600 + i);
        forest.add(leaf);
        initial_leaves.push_back(leaf);
    }

    std::cout << "  Initial state: " << forest.getNumLeaves() << " leaves" << std::endl;
    UtreexoHash commitment_before = forest.getCommitment();

    // Debug: show roots before snapshot
    auto roots_before = forest.getRoots();
    std::cout << "  Roots before snapshot (" << roots_before.size() << "):" << std::endl;
    for (const auto& r : roots_before) {
        std::cout << "    ";
        printHash(r);
    }

    // Snapshot the state (simulate BlockUndo)
    std::vector<uint8_t> snapshot = forest.serialize();
    std::cout << "  Created snapshot (" << snapshot.size() << " bytes)" << std::endl;

    // Apply new block: Add 3 more leaves
    std::vector<UtreexoHash> new_leaves;
    for (int i = 0; i < 3; i++) {
        UtreexoHash leaf = makeTestHash(700 + i);
        forest.add(leaf);
        new_leaves.push_back(leaf);
    }

    std::cout << "  After block: " << forest.getNumLeaves() << " leaves" << std::endl;
    UtreexoHash commitment_after = forest.getCommitment();
    assert(commitment_after != commitment_before);
    std::cout << "  ✅ Commitment changed after applying block" << std::endl;

    // Reorg: Restore from snapshot
    UtreexoForest restored = UtreexoForest::deserialize(snapshot);

    std::cout << "  After rollback: " << restored.getNumLeaves() << " leaves" << std::endl;
    UtreexoHash commitment_restored = restored.getCommitment();

    // Debug: show roots after restore
    auto roots_restored = restored.getRoots();
    std::cout << "  Roots after restore (" << roots_restored.size() << "):" << std::endl;
    for (const auto& r : roots_restored) {
        std::cout << "    ";
        printHash(r);
    }

    // Debug: show commitments
    std::cout << "  Commitment before: ";
    printHash(commitment_before);
    std::cout << "  Commitment restored: ";
    printHash(commitment_restored);

    // Verify restored state matches original
    assert(restored.getNumLeaves() == initial_leaves.size());
    if (commitment_restored != commitment_before) {
        std::cout << "  ❌ Commitment mismatch!" << std::endl;
        std::cout << "  Before roots (" << forest.getNumRoots() << "):" << std::endl;
        // Actually we need to save the roots from before
    }
    assert(commitment_restored == commitment_before);
    std::cout << "  ✅ State correctly restored to pre-block state" << std::endl;

    // Verify old proofs still work
    for (size_t i = 0; i < initial_leaves.size(); i++) {
        auto proof_opt = restored.prove(i);
        assert(proof_opt.has_value());
        bool valid = proof_opt.value().verify(initial_leaves[i], restored.getRoots());
        assert(valid);
    }
    std::cout << "  ✅ All " << initial_leaves.size() << " original proofs valid after rollback" << std::endl;

    // Verify new leaves are NOT in restored forest
    auto new_leaf_pos = restored.findLeafPosition(new_leaves[0]);
    assert(!new_leaf_pos.has_value());
    std::cout << "  ✅ New leaves correctly not present after rollback" << std::endl;

    std::cout << "✅ Test 7 passed: Reorg rollback works correctly" << std::endl;
}

//=============================================================================
// Test 8: Spent Output Data
//=============================================================================

void testSpentOutputData() {
    std::cout << "\n[Test 8] Spent output data..." << std::endl;

    // Create spent output
    uint64_t value = 100000000; // 1 DIN
    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14}; // P2PKH
    for (int i = 0; i < 20; i++) scriptPubKey.push_back(0xAB);
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    SpentOutputData spent(value, scriptPubKey);
    std::cout << "  Created spent output: value=" << value
              << ", scriptPubKey size=" << scriptPubKey.size() << std::endl;

    // Serialize
    std::vector<uint8_t> serialized = spent.serialize();
    std::cout << "  Serialized to " << serialized.size() << " bytes" << std::endl;

    // Deserialize
    size_t offset = 0;
    SpentOutputData restored = SpentOutputData::deserialize(serialized, offset);

    // Verify
    assert(restored.value == value);
    assert(restored.scriptPubKey == scriptPubKey);
    std::cout << "  ✅ Deserialized correctly" << std::endl;

    std::cout << "✅ Test 8 passed: Spent output data serialization works" << std::endl;
}

//=============================================================================
// Test 9: Block Utreexo Proof
//=============================================================================

void testBlockUtreexoProof() {
    std::cout << "\n[Test 9] Block Utreexo proof structures..." << std::endl;

    // Create block proof
    BlockUtreexoProof proof;
    proof.numLeaves = 100;  // Set numLeaves for stateless format

    // Add targets and their positions (version 4 requires positions)
    for (int i = 0; i < 5; i++) {
        proof.targets.push_back(makeTestHash(800 + i));
        proof.positions.push_back(i * 10);  // Example positions
    }

    // Add proof hashes
    for (int i = 0; i < 10; i++) {
        proof.proof_hashes.push_back(makeTestHash(900 + i));
    }

    std::cout << "  Created proof: " << proof.targets.size() << " targets, "
              << proof.proof_hashes.size() << " proof hashes" << std::endl;

    // Serialize
    std::vector<uint8_t> serialized = proof.serialize();
    std::cout << "  Serialized to " << serialized.size() << " bytes" << std::endl;

    // Deserialize
    BlockUtreexoProof restored = BlockUtreexoProof::deserialize(serialized);

    // Verify
    assert(restored.targets.size() == proof.targets.size());
    assert(restored.proof_hashes.size() == proof.proof_hashes.size());

    for (size_t i = 0; i < proof.targets.size(); i++) {
        assert(restored.targets[i] == proof.targets[i]);
    }

    for (size_t i = 0; i < proof.proof_hashes.size(); i++) {
        assert(restored.proof_hashes[i] == proof.proof_hashes[i]);
    }

    std::cout << "  ✅ Proof correctly serialized and deserialized" << std::endl;

    std::cout << "✅ Test 9 passed: Block Utreexo proof structures work" << std::endl;
}

//=============================================================================
// Test 10: Utreexo Activation
//=============================================================================

void testUtreexoActivation() {
    std::cout << "\n[Test 10] Utreexo activation..." << std::endl;

    // Utreexo active from genesis (height 0)
    assert(IsUtreexoActive(0));
    assert(IsUtreexoActive(1));
    assert(IsUtreexoActive(100));
    assert(IsUtreexoActive(1000000));

    std::cout << "  ✅ Utreexo active from genesis (all heights)" << std::endl;

    std::cout << "✅ Test 10 passed: Utreexo activation works correctly" << std::endl;
}

//=============================================================================
// Test 11: Edge Cases
//=============================================================================

void testEdgeCases() {
    std::cout << "\n[Test 11] Edge cases..." << std::endl;

    // Single leaf forest
    {
        UtreexoForest forest;
        UtreexoHash leaf = makeTestHash(1000);
        forest.add(leaf);

        assert(forest.getNumLeaves() == 1);
        assert(forest.getNumRoots() == 1);

        auto proof_opt = forest.prove(0);
        assert(proof_opt.has_value());

        UtreexoHash commitment = forest.getCommitment();
        assert(!commitment.empty());

        std::cout << "  ✅ Single leaf forest works" << std::endl;
    }

    // Large forest (stress test)
    {
        UtreexoForest forest;
        const int LARGE_COUNT = 1000;

        for (int i = 0; i < LARGE_COUNT; i++) {
            UtreexoHash leaf = makeTestHash(2000 + i);
            forest.add(leaf);
        }

        assert(forest.getNumLeaves() == LARGE_COUNT);

        // Verify a few random proofs
        for (int i = 0; i < 10; i++) {
            size_t pos = (i * 97) % LARGE_COUNT; // Pseudo-random positions
            auto proof_opt = forest.prove(pos);
            assert(proof_opt.has_value());
        }

        std::cout << "  ✅ Large forest (" << LARGE_COUNT << " leaves) works" << std::endl;
    }

    // Duplicate hash must be rejected (leaf-position ambiguity hardening)
    {
        UtreexoForest forest;
        UtreexoHash leaf = makeTestHash(3000);

        uint64_t pos0 = forest.add(leaf);
        uint64_t pos1 = forest.add(leaf); // Add same hash twice

        assert(pos0 == 0);
        assert(pos1 == UINT64_MAX);
        assert(forest.getNumLeaves() == 1);
        std::cout << "  ✅ Duplicate live leaf hash insertion is rejected" << std::endl;
    }

    std::cout << "✅ Test 11 passed: Edge cases handled correctly" << std::endl;
}

//=============================================================================
// Test 12: UTXO Hash Function
//=============================================================================

void testUTXOHashFunction() {
    std::cout << "\n[Test 12] UTXO hash function..." << std::endl;

    // Create test UTXO
    uint256 txid;
    std::memset(txid.data, 0xAB, 32);

    uint32_t vout = 0;
    uint64_t amount = 50000000; // 0.5 DIN
    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) scriptPubKey.push_back(0xCD);
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    // Hash the UTXO
    UtreexoHash hash1 = HashUTXOLegacy(txid, vout, amount, scriptPubKey);
    assert(hash1.size() == 32);
    assert(!hash1.empty());

    std::cout << "  UTXO hash: ";
    printHash(hash1);
    std::cout << std::endl;

    // Same UTXO should produce same hash (deterministic)
    UtreexoHash hash2 = HashUTXOLegacy(txid, vout, amount, scriptPubKey);
    assert(hash1 == hash2);
    std::cout << "  ✅ Hash is deterministic" << std::endl;

    // Different amount should produce different hash
    UtreexoHash hash3 = HashUTXOLegacy(txid, vout, amount + 1, scriptPubKey);
    assert(hash1 != hash3);
    std::cout << "  ✅ Different amount produces different hash" << std::endl;

    // Different scriptPubKey should produce different hash
    std::vector<uint8_t> scriptPubKey2 = scriptPubKey;
    scriptPubKey2[5] = 0xFF;
    UtreexoHash hash4 = HashUTXOLegacy(txid, vout, amount, scriptPubKey2);
    assert(hash1 != hash4);
    std::cout << "  ✅ Different scriptPubKey produces different hash" << std::endl;

    std::cout << "✅ Test 12 passed: UTXO hash function works correctly" << std::endl;
}

void testUTXORemoval() {
    std::cout << "\n[Test 13] UTXO removal..." << std::endl;

    UtreexoForest forest;

    // Add 8 leaves
    std::vector<UtreexoHash> leaves;
    for (int i = 0; i < 8; i++) {
        UtreexoHash leaf = makeTestHash(800 + i);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    std::cout << "  Added 8 leaves" << std::endl;
    assert(forest.getNumLeaves() == 8);
    assert(forest.getActiveLeaves() == 8);

    UtreexoHash commitment_before = forest.getCommitment();

    // Generate proof for leaf at position 3
    auto proof_opt = forest.prove(3);
    assert(proof_opt.has_value());
    UtreexoProof proof = proof_opt.value();

    // Verify proof before removal
    assert(proof.verify(leaves[3], forest.getRoots()));
    std::cout << "  ✅ Proof valid before removal" << std::endl;

    // Remove the leaf
    bool removed = forest.remove(leaves[3], proof);
    assert(removed);
    std::cout << "  ✅ Successfully removed UTXO at position 3" << std::endl;

    // Verify state after removal
    assert(forest.getNumLeaves() == 8);  // Total leaves doesn't decrease
    assert(forest.getActiveLeaves() == 7);  // Active leaves decreases
    assert(forest.isDeleted(3));
    std::cout << "  ✅ Active leaves: 8 → 7" << std::endl;

    // Commitment should change
    UtreexoHash commitment_after = forest.getCommitment();
    assert(commitment_before != commitment_after);
    std::cout << "  ✅ Commitment changed after removal" << std::endl;

    // Cannot prove deleted position
    auto proof_deleted = forest.prove(3);
    assert(!proof_deleted.has_value());
    std::cout << "  ✅ Cannot generate proof for deleted position" << std::endl;

    // Cannot remove again (already deleted)
    bool removed_again = forest.remove(leaves[3], proof);
    assert(!removed_again);
    std::cout << "  ✅ Cannot remove already-deleted UTXO" << std::endl;

    // Other positions still work
    for (uint64_t pos : {0, 1, 2, 4, 5, 6, 7}) {
        auto p = forest.prove(pos);
        assert(p.has_value());
        assert(p->verify(leaves[pos], forest.getRoots()));
    }
    std::cout << "  ✅ Other positions still have valid proofs" << std::endl;

    // Test serialization with deleted positions
    auto serialized = forest.serialize();
    std::cout << "  Serialized " << serialized.size() << " bytes with 1 deleted position" << std::endl;
    auto restored = UtreexoForest::deserialize(serialized);
    std::cout << "  Restored: numLeaves=" << restored.getNumLeaves()
              << ", activeLeaves=" << restored.getActiveLeaves()
              << ", isDeleted(3)=" << restored.isDeleted(3) << std::endl;
    assert(restored.getNumLeaves() == 8);
    assert(restored.getActiveLeaves() == 7);
    assert(restored.isDeleted(3));
    assert(restored.getCommitment() == commitment_after);
    std::cout << "  ✅ Serialization preserves deleted positions" << std::endl;

    // Verify other positions still work after restore
    for (uint64_t pos : {0, 1, 2, 4, 5, 6, 7}) {
        auto p = restored.prove(pos);
        assert(p.has_value());
        assert(p->verify(leaves[pos], restored.getRoots()));
    }
    std::cout << "  ✅ All non-deleted proofs work after restore" << std::endl;

    // Test removing multiple UTXOs
    auto proof2 = forest.prove(5);
    assert(proof2.has_value());
    assert(forest.remove(leaves[5], proof2.value()));

    auto proof3 = forest.prove(7);
    assert(proof3.has_value());
    assert(forest.remove(leaves[7], proof3.value()));

    assert(forest.getActiveLeaves() == 5);  // 8 - 3 deletions = 5 active
    std::cout << "  ✅ Multiple removals work correctly (5/8 active)" << std::endl;

    // Test batch proof with some deleted
    std::vector<UtreexoHash> targets = {leaves[0], leaves[2], leaves[4], leaves[6]};
    auto batch_proof = forest.generateBatchProof(targets);
    assert(batch_proof.size() > 0);
    std::cout << "  ✅ Batch proof generation works with deleted positions" << std::endl;

    std::cout << "✅ Test 13 passed: UTXO removal works correctly" << std::endl;
}

void testBlockValidationIntegration() {
    std::cout << "\n[Test 14] Block validation integration..." << std::endl;

    UtreexoForest forest;

    // Simulate adding UTXOs from a previous block
    std::vector<UtreexoHash> utxos;
    for (int i = 0; i < 5; i++) {
        // Create dummy UTXO hash (would normally be HashUTXOLegacy(txid, vout, amount, scriptPubKey))
        UtreexoHash utxo = makeTestHash(900 + i);
        forest.add(utxo);
        utxos.push_back(utxo);
    }

    assert(forest.getNumLeaves() == 5);
    assert(forest.getActiveLeaves() == 5);
    UtreexoHash commitment_before = forest.getCommitment();
    std::cout << "  Initial state: 5 UTXOs in accumulator" << std::endl;

    // Simulate spending 2 UTXOs in a new block
    // IMPORTANT: Must generate all proofs BEFORE any modifications
    // OR apply removals in order (higher positions first to avoid invalidation)

    // Step 1: Remove higher position first (position 2)
    auto proof2_opt = forest.prove(2);
    assert(proof2_opt.has_value());
    bool removed2 = forest.remove(utxos[2], proof2_opt.value());
    assert(removed2);

    // Step 2: Remove lower position (position 0)
    auto proof0_opt = forest.prove(0);
    assert(proof0_opt.has_value());
    bool removed0 = forest.remove(utxos[0], proof0_opt.value());
    assert(removed0);

    std::cout << "  ✅ Spent 2 UTXOs" << std::endl;

    // Step 3: Add new UTXOs created in the block
    for (int i = 0; i < 3; i++) {
        UtreexoHash new_utxo = makeTestHash(1000 + i);
        forest.add(new_utxo);
    }
    std::cout << "  ✅ Created 3 new UTXOs" << std::endl;

    // Verify final state
    assert(forest.getNumLeaves() == 8);  // 5 initial + 3 new
    assert(forest.getActiveLeaves() == 6);  // 5 - 2 spent + 3 new
    assert(forest.isDeleted(0));
    assert(forest.isDeleted(2));
    assert(!forest.isDeleted(1));
    assert(!forest.isDeleted(3));
    assert(!forest.isDeleted(4));

    UtreexoHash commitment_after = forest.getCommitment();
    assert(commitment_before != commitment_after);
    std::cout << "  ✅ Final state: 8 total leaves, 6 active" << std::endl;
    std::cout << "  ✅ Commitment changed correctly" << std::endl;

    // Test snapshot/restore for reorg (simulating DisconnectBlock)
    auto snapshot_after_block = forest.serialize();
    std::cout << "  ✅ Created snapshot after block" << std::endl;

    // Add more UTXOs (simulating another block)
    forest.add(makeTestHash(2000));
    forest.add(makeTestHash(2001));
    assert(forest.getNumLeaves() == 10);
    assert(forest.getActiveLeaves() == 8);

    // Restore to previous state (simulating reorg)
    forest = UtreexoForest::deserialize(snapshot_after_block);
    assert(forest.getNumLeaves() == 8);
    assert(forest.getActiveLeaves() == 6);
    assert(forest.getCommitment() == commitment_after);
    std::cout << "  ✅ Snapshot restore works (reorg simulation)" << std::endl;

    std::cout << "✅ Test 14 passed: Block validation integration works correctly" << std::endl;
}

void testDeltaUndoReorgRoundTrip() {
    std::cout << "\n[Test 15] Delta undo reorg round-trip..." << std::endl;

    UtreexoForest baseline;
    std::vector<UtreexoHash> base_leaves;

    for (int i = 0; i < 6; i++) {
        UtreexoHash leaf = makeTestHash(3000 + i);
        baseline.add(leaf);
        base_leaves.push_back(leaf);
    }

    const UtreexoHash baseline_commitment = baseline.getCommitment();
    const std::vector<uint8_t> baseline_snapshot = baseline.serialize();

    UtreexoForest branch = UtreexoForest::deserialize(baseline_snapshot);
    assert(branch.getCommitment() == baseline_commitment);

    // Simulate a connected block that spends two existing UTXOs and creates three new ones.
    auto spend_high = branch.prove(4);
    assert(spend_high.has_value());
    assert(branch.remove(base_leaves[4], spend_high.value()));

    auto spend_low = branch.prove(1);
    assert(spend_low.has_value());
    assert(branch.remove(base_leaves[1], spend_low.value()));

    std::vector<UtreexoHash> added_leaves;
    for (int i = 0; i < 3; i++) {
        UtreexoHash leaf = makeTestHash(3100 + i);
        uint64_t pos = branch.add(leaf);
        assert(pos != UINT64_MAX);
        added_leaves.push_back(leaf);
    }

    const UtreexoHash connected_commitment = branch.getCommitment();
    assert(connected_commitment != baseline_commitment);

    std::vector<UtreexoHash> connected_targets = {
        base_leaves[0],
        base_leaves[2],
        base_leaves[5],
        added_leaves[1],
    };
    BlockUtreexoProof connected_proof = branch.generateBlockProof(connected_targets);
    assert(branch.verifyBatchProofStateless(
        connected_proof.targets,
        connected_proof.positions,
        connected_proof.proof_hashes,
        connected_proof.numLeaves,
        branch.getRoots()));
    std::cout << "  ✅ Proof generation works on connected branch" << std::endl;

    // Simulate DisconnectBlock: remove new outputs, then restore spent leaves.
    assert(branch.removeLastNLeaves(added_leaves.size()));
    assert(branch.restoreDeletedLeaf(1, base_leaves[1]));
    assert(branch.restoreDeletedLeaf(4, base_leaves[4]));

    UtreexoForest restored = UtreexoForest::deserialize(baseline_snapshot);
    assert(branch.getNumLeaves() == restored.getNumLeaves());
    assert(branch.getActiveLeaves() == restored.getActiveLeaves());
    assert(branch.getCommitment() == restored.getCommitment());
    assert(branch.getCommitment() == baseline_commitment);
    std::cout << "  ✅ Delta undo restored the pre-block commitment" << std::endl;

    for (size_t i = 0; i < base_leaves.size(); i++) {
        auto branch_pos = branch.findLeafPosition(base_leaves[i]);
        auto restored_pos = restored.findLeafPosition(base_leaves[i]);
        assert(branch_pos.has_value());
        assert(restored_pos.has_value());
        assert(branch_pos.value() == restored_pos.value());

        auto branch_proof = branch.prove(branch_pos.value());
        auto restored_proof = restored.prove(restored_pos.value());
        assert(branch_proof.has_value());
        assert(restored_proof.has_value());
        assert(branch_proof.value().siblings == restored_proof.value().siblings);
        assert(branch_proof.value().verify(base_leaves[i], branch.getRoots()));
    }

    std::vector<UtreexoHash> rollback_targets = {
        base_leaves[0],
        base_leaves[1],
        base_leaves[4],
        base_leaves[5],
    };
    BlockUtreexoProof branch_proof = branch.generateBlockProof(rollback_targets);
    BlockUtreexoProof restored_proof = restored.generateBlockProof(rollback_targets);
    assert(branch_proof.targets == restored_proof.targets);
    assert(branch_proof.positions == restored_proof.positions);
    assert(branch_proof.proof_hashes == restored_proof.proof_hashes);
    assert(branch_proof.numLeaves == restored_proof.numLeaves);
    std::cout << "  ✅ Proof generation matches snapshot-restored state after rollback" << std::endl;

    std::cout << "✅ Test 15 passed: Delta undo reorg round-trip works correctly" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    // Initialize chain parameters for Utreexo activation checks
    SelectParams(Chain::REGTEST);

    std::cout << "========================================" << std::endl;
    std::cout << "  Utreexo Comprehensive Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testBasicForestOperations();
        testProofGeneration();
        testProofVerification();
        testBatchedProofGeneration();
        testBatchedProofVerification();
        testSerialization();
        testCanonicalEmptyRootsRoundtrip();
        testCanonicalEmptyRootsLegacyV2Recovery();
        testCanonicalEmptyRootsLegacyV2RecoveryShapes();
        testReorgSafety();
        testSpentOutputData();
        testBlockUtreexoProof();
        testUtreexoActivation();
        testEdgeCases();
        testUTXOHashFunction();
        testUTXORemoval();
        testBlockValidationIntegration();
        testDeltaUndoReorgRoundTrip();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  ✅ ALL TESTS PASSED (" << 18 << "/18)" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nUtreexo implementation is FULLY OPERATIONAL! 🎉" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cout << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "\n❌ TEST FAILED: Unknown exception" << std::endl;
        return 1;
    }
}
