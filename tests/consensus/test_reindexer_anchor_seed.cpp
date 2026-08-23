// Commit #1 of the offline `--rebuild-undo-range` series.
//
// Verifies the new `BlockReindexer::seedFromAnchor()` primitive:
//   - Loads forest state from a serialized blob into the reindexer's
//     in-memory `forest_` (Utreexo-active heights only)
//   - Loads shielded frontier state into `shielded_tree_` (shielded-active
//     heights only; ignored otherwise with a warning)
//   - Sets `accumulated_chainwork_`, `final_tip_hash_`, `final_tip_height_`
//     to the anchor's values
//   - Refuses height==0 (caller must use `seedGenesis` for genesis)
//   - Refuses Utreexo-active height with empty `forest_serialized`
//
// `seedFromAnchor` does NOT touch ChainDB; the test passes nullptr for
// chain_db and block_storage. The execute() wiring lands in commit #2.

#include "consensus/chainparams.h"
#include "consensus/reindexer.h"
#include "consensus/undo.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_activation.h"
#include "consensus/utreexo_delta.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

UtreexoHash MakeUtreexoHash(uint8_t seed) {
    UtreexoHash h(32);
    for (size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<uint8_t>(seed + i);
    }
    return h;
}

uint256 MakeUint256(uint8_t seed) {
    uint256 out;
    for (size_t i = 0; i < 32; ++i) {
        out.data[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

BlockReindexer MakeBareReindexer(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    // Steer shielded artifact paths to the test's tmp dir even though
    // seedFromAnchor doesn't touch them — keeps the constructor's defaults
    // from polluting cwd if a future refactor accidentally references them.
    config.shielded_frontier_output_path = tmp / "shielded_frontier.bin";
    config.shielded_nullifier_db_path = tmp / "shielded_nullifiers.db";
    return BlockReindexer(tmp, /*chain_db=*/nullptr, /*block_storage=*/nullptr,
                          config);
}

// Test 1: anchor at a Utreexo-active height with a non-empty forest blob
// loads correctly and exposes matching commitment + leaf count.
void testForestSeedRoundTrip(const std::filesystem::path& tmp) {
    std::cout << "\n[Test 1] seedFromAnchor — forest round-trip" << std::endl;

    // Build a reference forest with three leaves.
    UtreexoForest reference;
    reference.add(MakeUtreexoHash(0x10));
    reference.add(MakeUtreexoHash(0x20));
    reference.add(MakeUtreexoHash(0x30));
    const auto reference_commitment = reference.getCommitment();
    const auto reference_num_leaves = reference.getNumLeaves();
    const std::vector<uint8_t> serialized = reference.serialize();
    assert(!serialized.empty() && "serialize() must produce non-empty bytes for a populated forest");

    BlockReindexer::AnchorState anchor;
    anchor.height = 5;  // regtest has Utreexo active from height 0
    anchor.hash = MakeUint256(0xa0);
    anchor.chainwork = arith_uint256(123456);
    anchor.forest_serialized = serialized;
    // shielded inactive on regtest by default → frontier left empty

    auto reindexer = MakeBareReindexer(tmp);
    const Status status = reindexer.seedFromAnchor(anchor);
    assert(status == Status::Ok && "seedFromAnchor must accept a valid anchor");

    const auto snap = reindexer.snapshotInternalStateForTesting();
    assert(snap.forest_num_leaves == reference_num_leaves &&
           "seeded forest leaf count must match reference");
    assert(snap.forest_commitment == reference_commitment &&
           "seeded forest commitment must match reference");
    assert(snap.accumulated_chainwork == anchor.chainwork &&
           "accumulated_chainwork_ must reflect anchor");
    assert(snap.final_tip_hash == anchor.hash &&
           "final_tip_hash_ must reflect anchor");
    assert(snap.final_tip_height == static_cast<int32_t>(anchor.height) &&
           "final_tip_height_ must reflect anchor");

    std::cout << "  [PASS] forest commitment, leaf count, chainwork, tip hash/height all match"
              << std::endl;
}

// Test 2: anchor.height == 0 must be refused — genesis path is seedGenesis,
// not seedFromAnchor.
void testHeightZeroRefused(const std::filesystem::path& tmp) {
    std::cout << "\n[Test 2] seedFromAnchor — height==0 refused" << std::endl;

    BlockReindexer::AnchorState anchor;
    anchor.height = 0;
    anchor.hash = MakeUint256(0x01);
    anchor.chainwork = arith_uint256(1);

    auto reindexer = MakeBareReindexer(tmp);
    const Status status = reindexer.seedFromAnchor(anchor);
    assert(status == Status::Invalid &&
           "height==0 must return Status::Invalid (caller should use seedGenesis)");

    std::cout << "  [PASS] height==0 refused with Status::Invalid" << std::endl;
}

// Test 3: anchor at a Utreexo-active height with empty forest_serialized
// must be refused — empty forest at active height is consensus-invalid.
void testUtreexoActiveEmptyForestRefused(const std::filesystem::path& tmp) {
    std::cout << "\n[Test 3] seedFromAnchor — Utreexo-active height with empty forest refused"
              << std::endl;

    BlockReindexer::AnchorState anchor;
    anchor.height = 7;  // Utreexo active on regtest
    assert(IsUtreexoActive(anchor.height) && "regtest must have Utreexo active at height 7");
    anchor.hash = MakeUint256(0x07);
    anchor.chainwork = arith_uint256(7);
    // forest_serialized intentionally empty

    auto reindexer = MakeBareReindexer(tmp);
    const Status status = reindexer.seedFromAnchor(anchor);
    assert(status == Status::Invalid &&
           "Utreexo-active height with empty forest_serialized must return Status::Invalid");

    std::cout << "  [PASS] empty forest at Utreexo-active height refused" << std::endl;
}

// Test 4: anchor with garbage forest blob is rejected as a serialization error.
void testForestDeserializeFailureRefused(const std::filesystem::path& tmp) {
    std::cout << "\n[Test 4] seedFromAnchor — non-empty garbage forest refused" << std::endl;

    BlockReindexer::AnchorState anchor;
    anchor.height = 3;
    anchor.hash = MakeUint256(0x03);
    anchor.chainwork = arith_uint256(3);
    // 32 bytes is too small to be a valid populated forest serialization
    // (header + at least one root section requires more) and is not the
    // legal empty-forest 8-byte form, so deserialize() will fail to
    // reconstruct any leaves. The seed primitive must reject this.
    anchor.forest_serialized.assign(32, 0xAB);

    auto reindexer = MakeBareReindexer(tmp);
    const Status status = reindexer.seedFromAnchor(anchor);
    assert(status == Status::Serialization &&
           "non-empty garbage forest_serialized must return Status::Serialization");

    std::cout << "  [PASS] garbage forest_serialized refused" << std::endl;
}

// Test 5: snapshotInternalStateForTesting returns identity values on a
// freshly-constructed reindexer (no seed call).
void testSnapshotInitialState(const std::filesystem::path& tmp) {
    std::cout << "\n[Test 5] snapshotInternalStateForTesting — initial state" << std::endl;

    auto reindexer = MakeBareReindexer(tmp);
    const auto snap = reindexer.snapshotInternalStateForTesting();
    assert(snap.forest_num_leaves == 0 &&
           "fresh reindexer must have an empty forest");
    assert(snap.final_tip_height == -1 &&
           "fresh reindexer must have final_tip_height_ == -1");
    assert(snap.accumulated_chainwork == arith_uint256(0) &&
           "fresh reindexer must have zero accumulated chainwork");

    std::cout << "  [PASS] fresh reindexer reports empty/initial state" << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────
// Commit #2: WINDOWED_UNDO_ONLY config validation
// ─────────────────────────────────────────────────────────────────────────
// These tests exercise the validation block at the top of execute() that
// hard-fails on misconfiguration BEFORE any filesystem or ChainDB access.
// We construct the reindexer with nullptr for primary chain_db and block
// storage (the validation runs before either is dereferenced) and pass a
// dummy non-null pointer for live targets when needed (the validator only
// checks for nullptr; it does not dereference).
//
// This batch defers the end-to-end equivalence test to commit #3, where
// the verification harness arrives.

ChainDB* DummyChainDBPtr() {
    static int sentinel = 0;
    return reinterpret_cast<ChainDB*>(&sentinel);
}

BlockStorage* DummyBlockStoragePtr() {
    static int sentinel = 0;
    return reinterpret_cast<BlockStorage*>(&sentinel);
}

BlockReindexer::AnchorState ValidAnchorAtHeight5() {
    BlockReindexer::AnchorState anchor;
    anchor.height = 5;
    anchor.hash = MakeUint256(0xa0);
    anchor.chainwork = arith_uint256(123456);

    UtreexoForest reference;
    reference.add(MakeUtreexoHash(0x10));
    reference.add(MakeUtreexoHash(0x20));
    anchor.forest_serialized = reference.serialize();
    return anchor;
}

void runConfigValidationCase(const std::filesystem::path& tmp,
                             const std::string& test_name,
                             const BlockReindexer::Config& config,
                             const std::string& expected_error_substring) {
    std::cout << "\n[Test] WINDOWED_UNDO_ONLY config — " << test_name << std::endl;

    BlockReindexer reindexer(tmp, /*chain_db=*/nullptr, /*block_storage=*/nullptr,
                             config);
    auto result = reindexer.execute();
    // We only assert on stats.error — execute() either returns Ok with
    // success=false or returns an error status; both surface the message
    // via stats.error which is what we assert on.
    const std::string err = result.ok() ? result.value().error : std::string{};
    assert(!err.empty() &&
           "WINDOWED_UNDO_ONLY validation must populate stats_.error");
    assert(err.find(expected_error_substring) != std::string::npos &&
           "stats_.error must contain the expected substring");
    std::cout << "  [PASS] error: " << err << std::endl;
}

void testWindowedConfigMissingAnchor(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 6;
    window.end_height = 10;
    window.live_chain_db = DummyChainDBPtr();
    window.live_block_storage = DummyBlockStoragePtr();
    config.undo_rebuild_window = window;
    // No anchor_state.

    runConfigValidationCase(tmp, "missing anchor_state", config,
                            "requires Config::anchor_state");
}

void testWindowedConfigMissingWindow(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    config.anchor_state = ValidAnchorAtHeight5();
    // No undo_rebuild_window.

    runConfigValidationCase(tmp, "missing undo_rebuild_window", config,
                            "requires Config::undo_rebuild_window");
}

void testWindowedConfigNullLiveChainDB(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    config.anchor_state = ValidAnchorAtHeight5();
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 6;
    window.end_height = 10;
    window.live_chain_db = nullptr;  // ← bad
    window.live_block_storage = DummyBlockStoragePtr();
    config.undo_rebuild_window = window;

    runConfigValidationCase(tmp, "null live_chain_db", config,
                            "live_chain_db");
}

void testWindowedConfigNullLiveBlockStorage(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    config.anchor_state = ValidAnchorAtHeight5();
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 6;
    window.end_height = 10;
    window.live_chain_db = DummyChainDBPtr();
    window.live_block_storage = nullptr;  // ← bad
    config.undo_rebuild_window = window;

    runConfigValidationCase(tmp, "null live_block_storage", config,
                            "live_block_storage");
}

void testWindowedConfigStartLEAnchor(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    config.anchor_state = ValidAnchorAtHeight5();  // height 5
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 5;  // ← must be > 5
    window.end_height = 10;
    window.live_chain_db = DummyChainDBPtr();
    window.live_block_storage = DummyBlockStoragePtr();
    config.undo_rebuild_window = window;

    runConfigValidationCase(tmp, "start_height <= anchor.height", config,
                            "must be > anchor.height");
}

void testWindowedConfigEndLTStart(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    config.preserve_shielded_state_on_init = true;
    config.anchor_state = ValidAnchorAtHeight5();
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 10;
    window.end_height = 9;  // ← bad
    window.live_chain_db = DummyChainDBPtr();
    window.live_block_storage = DummyBlockStoragePtr();
    config.undo_rebuild_window = window;

    runConfigValidationCase(tmp, "end_height < start_height", config,
                            "end_height < window.start_height");
}

void testWindowedConfigMissingPreserveFlag(const std::filesystem::path& tmp) {
    BlockReindexer::Config config;
    config.mode = BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    // preserve_shielded_state_on_init left default-false
    config.anchor_state = ValidAnchorAtHeight5();
    BlockReindexer::UndoRebuildWindow window;
    window.start_height = 6;
    window.end_height = 10;
    window.live_chain_db = DummyChainDBPtr();
    window.live_block_storage = DummyBlockStoragePtr();
    config.undo_rebuild_window = window;

    runConfigValidationCase(tmp, "missing preserve_shielded_state_on_init", config,
                            "preserve_shielded_state_on_init");
}

// ─────────────────────────────────────────────────────────────────────────
// Commit #3: DisconnectBlock-roundtrip verification harness
// ─────────────────────────────────────────────────────────────────────────
// The user's stated rule: "rebuilt undo is accepted only if DisconnectBlock
// round-trips cleanly." These tests pin that property by exercising the
// verifier directly on hand-crafted post-apply forest state + UndoRecord
// fixtures.
//
// Test shape per case:
//   1. Construct a synthetic block (coinbase only, configurable output count)
//   2. Build the post-apply forest by adding output leaves
//   3. Seed the reindexer's forest_ to that post-apply state via
//      seedFromAnchor (uses the public AnchorState path)
//   4. Build a matching UndoRecord (no spent, N created markers,
//      no shielded frontier — regtest has shielded inactive)
//   5. Build a matching UtreexoDelta describing the N adds
//   6. Capture pre_state with empty-forest commitment (pre-apply)
//   7. Call the test wrapper; assert expected status

UtreexoHash LeafHashFor(const TxId& txid, uint32_t vout, uint32_t height) {
    // Match what UtreexoForest expects but for the test we just produce a
    // deterministic-but-arbitrary 32-byte hash; the verifier's forest
    // reverse-apply only requires (position, leafHash) pairs to match the
    // ones used at add() time.
    UtreexoHash h(32, 0);
    const auto txid_bytes = txid.AsUint256();
    for (size_t i = 0; i < 32; ++i) {
        h[i] = static_cast<uint8_t>(txid_bytes.data[i] ^
                                    (vout * 17u + i) ^
                                    (height & 0xFF));
    }
    return h;
}

Block BuildCoinbaseOnlyBlock(uint32_t output_count) {
    Block block;
    block.header.version = 1;
    block.header.timestamp = 1700000000;
    block.header.difficulty = 0x207fffff;  // regtest min
    block.header.nonce = 0;
    block.header.utreexo_root.SetNull();

    Transaction coinbase;
    coinbase.version = Transaction::TX_VERSION_LEGACY;
    coinbase.lockTime = 0;
    // Coinbase has a synthetic vin entry (no real prevout) — but the verifier
    // skips tx_idx==0 when counting expected spent inputs, so we leave it empty.
    for (uint32_t i = 0; i < output_count; ++i) {
        TxOutput out;
        out.value = AmountUna::Una(50'000'000ULL + i);
        out.scriptPubKey = {0x51, 0x20};  // OP_1 (Taproot v1) prefix; size irrelevant for the test
        for (size_t j = 0; j < 32; ++j) {
            out.scriptPubKey.push_back(static_cast<uint8_t>(i * 13 + j));
        }
        coinbase.vout.push_back(out);
    }
    block.vtx.push_back(coinbase);
    return block;
}

struct VerifyFixture {
    Block block;
    UndoRecord undo;
    UtreexoDelta delta;
    BlockReindexer::PreApplyStateForVerification pre_state;
    std::vector<uint8_t> candidate_undo_bytes;
    UtreexoForest post_forest;
    uint64_t height = 6;
};

VerifyFixture BuildHappyPathFixture(uint32_t output_count) {
    VerifyFixture f;
    f.block = BuildCoinbaseOnlyBlock(output_count);

    // Pre-apply forest = empty
    UtreexoForest pre_forest;
    f.pre_state.utreexo_active_at_height = true;
    f.pre_state.shielded_active_at_height = false;
    f.pre_state.forest_commitment = pre_forest.getCommitment();
    f.pre_state.forest_num_leaves = 0;

    // Apply the block's outputs to the forest (post-apply state)
    f.delta.numLeavesBefore = 0;
    const auto& coinbase = f.block.vtx[0];
    const auto txid = coinbase.GetTxid();
    for (uint32_t vout = 0; vout < coinbase.vout.size(); ++vout) {
        const auto leaf = LeafHashFor(txid, vout, static_cast<uint32_t>(f.height));
        const auto position = f.post_forest.add(leaf);
        AddedLeaf added;
        added.hash = leaf;
        added.position = position;
        f.delta.addedLeaves.push_back(added);
        f.undo.created.emplace_back(txid.AsUint256(), vout);
    }

    f.candidate_undo_bytes = f.undo.Serialize();
    return f;
}

void RunVerifyCase(const std::filesystem::path& tmp,
                   const std::string& test_name,
                   VerifyFixture& f,
                   bool expect_ok,
                   const std::string& expected_error_substring = {}) {
    std::cout << "\n[Test] verify — " << test_name << std::endl;

    auto reindexer = MakeBareReindexer(tmp);

    // Seed reindexer's forest_ to post-apply state via the public AnchorState path.
    BlockReindexer::AnchorState anchor;
    anchor.height = static_cast<uint32_t>(f.height);
    anchor.hash = MakeUint256(0xa0);
    anchor.chainwork = arith_uint256(99);
    anchor.forest_serialized = f.post_forest.serialize();
    const Status seed_s = reindexer.seedFromAnchor(anchor);
    assert(seed_s == Status::Ok);

    std::string err;
    const Status s = reindexer.verifyRebuiltUndoRoundTripForTesting(
        f.block, f.height, f.candidate_undo_bytes, f.undo, f.pre_state, f.delta, err);

    if (expect_ok) {
        assert(s == Status::Ok && "expected verification to succeed");
        assert(err.empty() && "no error string on success");
        std::cout << "  [PASS] verification accepted clean rebuild" << std::endl;
    } else {
        assert(s != Status::Ok && "expected verification to fail");
        assert(!err.empty() && "error string must be set on failure");
        if (!expected_error_substring.empty()) {
            assert(err.find(expected_error_substring) != std::string::npos &&
                   "error message must contain expected substring");
        }
        std::cout << "  [PASS] verification rejected: " << err << std::endl;
    }
}

void testVerifyHappyPath(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/3);
    RunVerifyCase(tmp, "happy path (clean rebuild round-trips)", f, /*expect_ok=*/true);
}

void testVerifyCorruptedBytes(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/2);
    // Add a trailing byte that the decoder does not consume. Deserialize still
    // succeeds, but re-Serialize cannot reproduce the non-canonical input.
    assert(!f.candidate_undo_bytes.empty());
    f.candidate_undo_bytes.push_back(0);
    RunVerifyCase(tmp, "corrupted undo bytes (roundtrip-not-byte-stable)", f,
                  /*expect_ok=*/false,
                  "serialize-roundtrip-not-byte-stable");
}

void testVerifySpentCountMismatch(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/2);
    // Inject a phantom spent entry not present in the block.
    SpentCoin phantom;
    phantom.prev_txid = MakeUint256(0xee);
    phantom.prev_vout = 0;
    phantom.value = 12345;
    phantom.scriptPubKey = {0x51};
    phantom.is_coinbase = false;
    phantom.height = 3;
    f.undo.spent.push_back(phantom);
    f.candidate_undo_bytes = f.undo.Serialize();
    RunVerifyCase(tmp, "spent count mismatch", f, /*expect_ok=*/false,
                  "spent-count-mismatch");
}

void testVerifyCreatedCountMismatch(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/2);
    // Drop one created marker.
    f.undo.created.pop_back();
    f.candidate_undo_bytes = f.undo.Serialize();
    RunVerifyCase(tmp, "created count mismatch", f, /*expect_ok=*/false,
                  "created-count-mismatch");
}

void testVerifyCreatedNotInBlock(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/2);
    // Replace one created marker's txid with a phantom one.
    f.undo.created.back().txid = MakeUint256(0xfa);
    f.candidate_undo_bytes = f.undo.Serialize();
    RunVerifyCase(tmp, "created entry not in block", f, /*expect_ok=*/false,
                  "created-entry-not-in-block");
}

void testVerifyForestReverseFails(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/2);
    // Append a phantom added leaf that wasn't really added — reverse-apply
    // will try to `removeLastNLeaves(N+1)` on a clone with N leaves and
    // fail.
    AddedLeaf phantom;
    phantom.hash = UtreexoHash(32, 0xCC);
    phantom.position = 999;
    f.delta.addedLeaves.push_back(phantom);
    RunVerifyCase(tmp, "forest reverse fails", f, /*expect_ok=*/false,
                  "forest-reverse-add-failed");
}

void testVerifyShieldedFrontierMismatch(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/1);
    // Pretend shielded was active and the captured pre-frontier was X,
    // but the candidate undo's pre_block_shielded_frontier is missing.
    f.pre_state.shielded_active_at_height = true;
    f.pre_state.shielded_frontier_serialized = {0xde, 0xad, 0xbe, 0xef};
    // f.undo.pre_block_shielded_frontier left absent — verifier should
    // catch the missing-field case at a shielded-active height.
    f.candidate_undo_bytes = f.undo.Serialize();
    RunVerifyCase(tmp, "missing pre_block_shielded_frontier when shielded active",
                  f, /*expect_ok=*/false,
                  "missing-pre_block_shielded_frontier");
}

void testVerifyShieldedFrontierBytesMismatch(const std::filesystem::path& tmp) {
    auto f = BuildHappyPathFixture(/*output_count=*/1);
    f.pre_state.shielded_active_at_height = true;
    f.pre_state.shielded_frontier_serialized = {0xde, 0xad, 0xbe, 0xef};
    // Provide a frontier blob that doesn't match the captured one.
    f.undo.pre_block_shielded_frontier = std::vector<uint8_t>{0xc0, 0xff, 0xee};
    f.candidate_undo_bytes = f.undo.Serialize();
    RunVerifyCase(tmp, "pre_block_shielded_frontier bytes mismatch",
                  f, /*expect_ok=*/false,
                  "pre_block_shielded_frontier-bytes-mismatch");
}

}  // namespace

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Block Reindexer — seedFromAnchor (commit #1)" << std::endl;
    std::cout << "========================================" << std::endl;

    SelectParams(Chain::REGTEST);

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "dinero_reindexer_anchor_seed";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    testSnapshotInitialState(tmp);
    testForestSeedRoundTrip(tmp);
    testHeightZeroRefused(tmp);
    testUtreexoActiveEmptyForestRefused(tmp);
    testForestDeserializeFailureRefused(tmp);

    // Commit #2: WINDOWED_UNDO_ONLY config validation
    testWindowedConfigMissingAnchor(tmp);
    testWindowedConfigMissingWindow(tmp);
    testWindowedConfigNullLiveChainDB(tmp);
    testWindowedConfigNullLiveBlockStorage(tmp);
    testWindowedConfigStartLEAnchor(tmp);
    testWindowedConfigEndLTStart(tmp);
    testWindowedConfigMissingPreserveFlag(tmp);

    // Commit #3: DisconnectBlock-roundtrip verification harness
    testVerifyHappyPath(tmp);
    testVerifyCorruptedBytes(tmp);
    testVerifySpentCountMismatch(tmp);
    testVerifyCreatedCountMismatch(tmp);
    testVerifyCreatedNotInBlock(tmp);
    testVerifyForestReverseFails(tmp);
    testVerifyShieldedFrontierMismatch(tmp);
    testVerifyShieldedFrontierBytesMismatch(tmp);

    std::cout << "\n========================================" << std::endl;
    std::cout << "[PASS] seedFromAnchor + WINDOWED_UNDO_ONLY + verify-roundtrip green"
              << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Commit #3 invariant pinned: rebuilt undo bytes are accepted only" << std::endl;
    std::cout << "if a DisconnectBlock-equivalent reverse pass round-trips them" << std::endl;
    std::cout << "back to the captured pre-apply state. Live writes are gated on" << std::endl;
    std::cout << "this property in production." << std::endl << std::endl;

    std::filesystem::remove_all(tmp, ec);
    return 0;
}
