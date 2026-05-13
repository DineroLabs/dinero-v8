/**
 * Header status-bits regression test (May 2026 fleet-wide HAVE_UNDO bug).
 *
 * Bug: HeaderSyncManager::MarkBlockReceived (and MarkBlockFailed) called
 * ChainDB::updateHeaderStatus(token, hash, node->status, ...) — which
 * OVERWRITES on-disk status_flags with the passed value. node->status
 * only tracks the bits HeaderSyncManager cares about (HAVE_DATA, FAILED).
 * Consensus-layer bits like BLOCK_HAVE_UNDO that ConnectTip had set on
 * the chaindb were silently stripped on every gossiped block.
 *
 * Fix: ChainDB now has bit-level mutation APIs:
 *   - setHeaderStatusBits() OR-merges bits without stripping anything.
 *   - clearHeaderStatusBits() clears only requested bits without stripping
 *     unrelated bits.
 * Partial production updates must use those APIs; updateHeaderStatus is
 * reserved for test-covered, full-status overwrite cases only.
 *
 * This test pins the contract:
 *   1. updateHeaderStatus still allows callers to OVERWRITE status_flags
 *      (the legitimate use case, e.g. updateBlockIndex passing the full
 *      authoritative pindex->status). Bug-class behavior preserved
 *      because the API name and comment loudly warn about it.
 *   2. setHeaderStatusBits never strips. Bits already on disk survive.
 *   3. clearHeaderStatusBits clears only requested bits.
 *   4. setHeaderStatusBits is a no-op write when nothing would change
 *      (idempotency).
 *   5. setHeaderStatusBits returns NotFound when there's no metadata to
 *      OR-merge into (caller decides what to do).
 *   6. putHeaderMetadataPreservingExistingUndo preserves canonical undo
 *      metadata when a duplicate side-chain/relay writer re-stores the same
 *      header without owning undo fields.
 *   7. updateUndoLocator updates only undo_file/pos/size and ORs
 *      BLOCK_HAVE_UNDO without touching topology, block body position, or
 *      unrelated status bits.
 */

#include "storage/chain_db.h"
#include "primitives/uint256.h"
#include "consensus/chainwork.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

using namespace dinero;

namespace {

const char* TEST_DATADIR = "/tmp/dinero_header_status_bits_test";

void cleanTestDatadir() {
    std::error_code ec;
    std::filesystem::remove_all(TEST_DATADIR, ec);
    std::filesystem::create_directories(TEST_DATADIR, ec);
}

ChainDB::PersistedHeaderMetadata makeHealthyHeader(uint32_t status_flags) {
    ChainDB::PersistedHeaderMetadata m;
    m.parent_hash = uint256::FromHexUnsafe(
        "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f");
    m.height = 12345;
    m.chainwork = arith_uint256();  // default zero — test doesn't depend on value
    m.status_flags = status_flags;
    m.file_number = 7;
    m.data_pos = 1234;
    m.data_size = 5678;
    m.undo_file = 3;
    m.undo_pos = 91011;
    m.undo_size = 4242;
    return m;
}

// Helper: combine bits from BlockStatus + lifecycle enums without
// triggering bugprone-suspicious-enum-usage. The two enums share the
// same uint32_t status_flags space by design.
constexpr uint32_t bits(uint32_t a, uint32_t b = 0, uint32_t c = 0,
                        uint32_t d = 0, uint32_t e = 0, uint32_t f = 0,
                        uint32_t g = 0) {
    return a | b | c | d | e | f | g;
}

void pass(const std::string& msg) { std::cout << "  [✓] " << msg << std::endl; }
void fail(const std::string& msg) { std::cerr << "  [FAIL] " << msg << std::endl; std::abort(); }

// ─────────────────────────────────────────────────────────────────
// #1 — setHeaderStatusBits OR-merges; never strips
// ─────────────────────────────────────────────────────────────────
void test01_OrMergeNeverStrips() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");

    // Seed with a "fully connected, with undo" status — every bit set
    // that ConnectTip would have set after a successful connect.
    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_VALID_SCRIPTS,
        BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO);
    auto put_status = db.putHeaderMetadata(token, hash, makeHealthyHeader(initial));
    assert(put_status == Status::Ok);

    // Now simulate the buggy MarkBlockReceived path: caller knows about
    // BLOCK_HAVE_DATA only. With setHeaderStatusBits, this must NOT
    // strip the consensus-layer bits (HAVE_UNDO especially).
    auto set_status = db.setHeaderStatusBits(token, hash, BLOCK_HAVE_DATA);
    assert(set_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    assert(read.ok());
    if (read.value().status_flags != initial) {
        fail("setHeaderStatusBits stripped bits: got 0x" +
             std::to_string(read.value().status_flags) +
             " want 0x" + std::to_string(initial));
    }
    if (!(read.value().status_flags & BLOCK_HAVE_UNDO)) {
        fail("BLOCK_HAVE_UNDO got stripped (this IS the May 2026 bug)");
    }
    pass("OR-merge preserved every bit (HAVE_UNDO survived MarkBlockReceived-class call)");
}

// ─────────────────────────────────────────────────────────────────
// #2 — setHeaderStatusBits actually adds new bits
// ─────────────────────────────────────────────────────────────────
void test02_OrMergeAddsBits() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "1111111111111111111111111111111111111111111111111111111111111111");

    // Seed with VALID_HEADER only (header just received, no body yet).
    const uint32_t initial = bits(BLOCK_VALID_HEADER);
    db.putHeaderMetadata(token, hash, makeHealthyHeader(initial));

    // MarkBlockReceived adds BLOCK_HAVE_DATA.
    auto set_status = db.setHeaderStatusBits(token, hash, BLOCK_HAVE_DATA);
    assert(set_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    const uint32_t expected = bits(BLOCK_VALID_HEADER, BLOCK_HAVE_DATA);
    if (read.value().status_flags != expected) {
        fail("expected " + std::to_string(expected) +
             " got " + std::to_string(read.value().status_flags));
    }
    pass("OR-merge added BLOCK_HAVE_DATA on top of BLOCK_VALID_HEADER");
}

// ─────────────────────────────────────────────────────────────────
// #3 — clearHeaderStatusBits clears only requested bits; never strips
//      unrelated persisted status like HAVE_UNDO.
// ─────────────────────────────────────────────────────────────────
void test03_ClearMergeNeverStrips() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "2222222222222222222222222222222222222222222222222222222222222222");

    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_CHAIN,
        BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO,
        BLOCK_FAILED_VALID, BLOCK_FAILED_CHILD);
    db.putHeaderMetadata(token, hash, makeHealthyHeader(initial));

    auto clear_status = db.clearHeaderStatusBits(
        token, hash, BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD);
    assert(clear_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    const uint32_t expected = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_CHAIN,
        BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO);
    if (read.value().status_flags != expected) {
        fail("clearHeaderStatusBits stripped unrelated bits: got " +
             std::to_string(read.value().status_flags) +
             " want " + std::to_string(expected));
    }
    pass("clearHeaderStatusBits cleared failed bits while preserving HAVE_UNDO");
}

// ─────────────────────────────────────────────────────────────────
// #4 — Idempotent no-op write when bits already present
// ─────────────────────────────────────────────────────────────────
void test04_IdempotentNoOp() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "3333333333333333333333333333333333333333333333333333333333333333");

    const uint32_t initial = bits(BLOCK_VALID_HEADER, BLOCK_HAVE_DATA);
    db.putHeaderMetadata(token, hash, makeHealthyHeader(initial));

    // Calling setHeaderStatusBits with bits that are already set must
    // succeed and leave the metadata unchanged.
    auto set_status = db.setHeaderStatusBits(token, hash, BLOCK_HAVE_DATA);
    assert(set_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    if (read.value().status_flags != initial) {
        fail("idempotent call mutated state");
    }
    pass("calling setHeaderStatusBits with bits already set is a clean no-op");
}

// ─────────────────────────────────────────────────────────────────
// #5 — Returns error when no metadata exists for hash
// ─────────────────────────────────────────────────────────────────
void test05_ErrorOnMissing() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 missing = uint256::FromHexUnsafe(
        "5555555555555555555555555555555555555555555555555555555555555555");

    // No putHeaderMetadata first; setHeaderStatusBits must fail rather
    // than silently inventing metadata from partial bits.
    auto set_status = db.setHeaderStatusBits(token, missing, BLOCK_HAVE_DATA);
    if (set_status == Status::Ok) {
        fail("setHeaderStatusBits should fail on missing metadata, not silently succeed");
    }
    pass("setHeaderStatusBits correctly errors on missing metadata");
}

// ─────────────────────────────────────────────────────────────────
// #6 — Duplicate metadata writes preserve existing undo.
// ─────────────────────────────────────────────────────────────────
void test06_PreserveExistingUndoOnDuplicateMetadataWrite() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "6666666666666666666666666666666666666666666666666666666666666666");

    const uint32_t connected = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_VALID_SCRIPTS,
        BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO);
    auto healthy = makeHealthyHeader(connected);
    healthy.undo_file = 9;
    healthy.undo_pos = 101112;
    healthy.undo_size = 3333;
    db.putHeaderMetadata(token, hash, healthy);

    // Simulate the production recurrence: after ConnectTip wrote undo,
    // BlockAcceptor sees the same block again through the side-chain/relay
    // path. That path stores block body metadata but does not own undo.
    const uint32_t relay_status = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_HAVE_DATA);
    auto duplicate = makeHealthyHeader(relay_status);
    duplicate.file_number = 11;
    duplicate.data_pos = 202122;
    duplicate.data_size = 4444;
    duplicate.undo_file = 0;
    duplicate.undo_pos = 0;
    duplicate.undo_size = 0;

    auto put_status = db.putHeaderMetadataPreservingExistingUndo(token, hash, duplicate);
    assert(put_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    assert(read.ok());
    if (!(read.value().status_flags & BLOCK_HAVE_UNDO)) {
        fail("duplicate metadata write stripped BLOCK_HAVE_UNDO");
    }
    if (read.value().undo_file != 9 || read.value().undo_pos != 101112 ||
        read.value().undo_size != 3333) {
        fail("duplicate metadata write clobbered undo file position");
    }
    if (read.value().file_number != 11 || read.value().data_pos != 202122 ||
        read.value().data_size != 4444) {
        fail("duplicate metadata write did not update block body position");
    }
    pass("duplicate metadata write preserved existing undo while updating block body position");
}

// ─────────────────────────────────────────────────────────────────
// #7 — ConnectTip-class undo locator update is surgical.
// ─────────────────────────────────────────────────────────────────
void test07_UpdateUndoLocatorIsSurgical() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "7777777777777777777777777777777777777777777777777777777777777777");

    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_VALID_SCRIPTS,
        BLOCK_HAVE_DATA);
    auto metadata = makeHealthyHeader(initial);
    metadata.undo_file = 0;
    metadata.undo_pos = 0;
    metadata.undo_size = 0;
    metadata.file_number = 44;
    metadata.data_pos = 5555;
    metadata.data_size = 6666;
    db.putHeaderMetadata(token, hash, metadata);

    auto update_status = db.updateUndoLocator(token, hash, 12, 34567, 890);
    assert(update_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    assert(read.ok());
    const uint32_t expected_status = bits(initial, BLOCK_HAVE_UNDO);
    if (read.value().status_flags != expected_status) {
        fail("updateUndoLocator did not OR only BLOCK_HAVE_UNDO");
    }
    if (read.value().undo_file != 12 || read.value().undo_pos != 34567 ||
        read.value().undo_size != 890) {
        fail("updateUndoLocator did not write undo locator");
    }
    if (read.value().file_number != 44 || read.value().data_pos != 5555 ||
        read.value().data_size != 6666) {
        fail("updateUndoLocator clobbered block body position");
    }
    if (read.value().parent_hash != metadata.parent_hash ||
        read.value().height != metadata.height ||
        read.value().chainwork != metadata.chainwork) {
        fail("updateUndoLocator clobbered topology/chainwork metadata");
    }

    pass("updateUndoLocator stamped undo while preserving unrelated metadata");
}

// ─────────────────────────────────────────────────────────────────
// #8 — Old updateHeaderStatus still OVERWRITES in tests only.
// ─────────────────────────────────────────────────────────────────
void test08_UpdateHeaderStatusStillOverwrites() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "4444444444444444444444444444444444444444444444444444444444444444");

    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_CHAIN,
        BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO);
    db.putHeaderMetadata(token, hash, makeHealthyHeader(initial));

    // updateHeaderStatus is still a full-overwrite primitive. Production
    // partial mutations are guarded by HeaderStatusOverwriteAllowlist and
    // should use setHeaderStatusBits / clearHeaderStatusBits instead.
    const uint32_t new_status = bits(BLOCK_VALID_HEADER, BLOCK_HAVE_DATA);
    auto upd_status = db.updateHeaderStatus(token, hash, new_status);
    assert(upd_status == Status::Ok);

    auto read = db.getHeaderMetadata(hash);
    if (read.value().status_flags != new_status) {
        fail("updateHeaderStatus didn't overwrite as expected");
    }
    if (read.value().status_flags & BLOCK_HAVE_UNDO) {
        fail("updateHeaderStatus unexpectedly preserved HAVE_UNDO");
    }
    pass("updateHeaderStatus still overwrites (intentional behavior preserved)");
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Header Status-Bits Test (May 2026 regression guard)" << std::endl;
    std::cout << "========================================" << std::endl;

    test01_OrMergeNeverStrips();
    test02_OrMergeAddsBits();
    test03_ClearMergeNeverStrips();
    test04_IdempotentNoOp();
    test05_ErrorOnMissing();
    test06_PreserveExistingUndoOnDuplicateMetadataWrite();
    test07_UpdateUndoLocatorIsSurgical();
    test08_UpdateHeaderStatusStillOverwrites();

    std::cout << "\n✅ Header status bits: 8/8 properties hold" << std::endl;
    cleanTestDatadir();
    return 0;
}
