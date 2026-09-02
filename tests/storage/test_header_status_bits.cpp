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

// Issue #453: updateUndoLocator() gained an extra_status_bits parameter so
// ConnectTip can persist BLOCK_VALID_SCRIPTS in the SAME read/merge/write that
// stamps the undo locator.
//
// This exists because the obvious alternative — staging a separate
// setHeaderStatusBits() call into the same WriteBatch — silently loses data:
// both helpers read via getHeaderMetadata(), which sees only COMMITTED state,
// so the second helper re-reads the pre-batch row and drops the bits the first
// one staged. That is the same lost-update class as the May 2026 HAVE_UNDO
// incident this file guards.
void test09_UpdateUndoLocatorMergesExtraStatusBits() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "9999999999999999999999999999999999999999999999999999999999999999");

    // Deliberately WITHOUT BLOCK_VALID_SCRIPTS — this mirrors the row
    // BlockAcceptor persists, whose status literal omits it.
    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_HAVE_DATA);
    auto metadata = makeHealthyHeader(initial);
    metadata.undo_file = 0;
    metadata.undo_pos = 0;
    metadata.undo_size = 0;
    metadata.file_number = 77;
    metadata.data_pos = 1234;
    metadata.data_size = 4321;
    db.putHeaderMetadata(token, hash, metadata);

    auto update_status = db.updateUndoLocator(token, hash, 9, 8888, 777,
                                              nullptr, BLOCK_VALID_SCRIPTS);
    if (update_status != Status::Ok) {
        fail("updateUndoLocator with extra_status_bits failed");
    }

    auto read = db.getHeaderMetadata(hash);
    if (!read.ok()) {
        fail("getHeaderMetadata failed after extra-bits update");
    }

    // Both the caller-supplied bit and BLOCK_HAVE_UNDO must land in ONE write.
    const uint32_t expected_status =
        bits(initial, BLOCK_HAVE_UNDO, BLOCK_VALID_SCRIPTS);
    if (read.value().status_flags != expected_status) {
        fail("updateUndoLocator did not merge extra_status_bits + HAVE_UNDO "
             "(got " + std::to_string(read.value().status_flags) +
             ", want " + std::to_string(expected_status) + ")");
    }
    if ((read.value().status_flags & BLOCK_HAVE_UNDO) == 0) {
        fail("extra_status_bits path dropped BLOCK_HAVE_UNDO");
    }
    if (read.value().undo_file != 9 || read.value().undo_pos != 8888 ||
        read.value().undo_size != 777) {
        fail("extra_status_bits path did not write the undo locator");
    }
    if (read.value().file_number != 77 || read.value().data_pos != 1234 ||
        read.value().data_size != 4321) {
        fail("extra_status_bits path clobbered block body position");
    }
    pass("updateUndoLocator merges extra_status_bits without stripping");
}

// SCOPE: this is a CALLER-CONTRACT test, not a script-validity test.
//
// It proves only that updateUndoLocator() with the default extra_status_bits
// argument adds no status bits beyond BLOCK_HAVE_UNDO, so the other two
// callers (CommitConnectedBlockBookkeeping, PromoteValidatedHistory) cannot
// silently begin stamping BLOCK_VALID_SCRIPTS.
//
// It does NOT prove that a block whose scripts actually FAIL validation is
// denied the bit — nothing here runs script validation at all. That property
// is covered end-to-end by the ScriptFailureNeverValidates integration test,
// which submits a block containing a genuinely unspendable input and inspects
// the stored status afterwards.
void test10_UpdateUndoLocatorDefaultAddsNoExtraBits() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "aaaa000000000000000000000000000000000000000000000000000000000000");

    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_HAVE_DATA);
    auto metadata = makeHealthyHeader(initial);
    metadata.undo_size = 0;
    db.putHeaderMetadata(token, hash, metadata);

    // No extra bits argument — must behave exactly as before #453.
    auto update_status = db.updateUndoLocator(token, hash, 3, 44, 55);
    if (update_status != Status::Ok) {
        fail("updateUndoLocator (default extra bits) failed");
    }

    auto read = db.getHeaderMetadata(hash);
    if (!read.ok()) {
        fail("getHeaderMetadata failed after default update");
    }
    if (read.value().status_flags != bits(initial, BLOCK_HAVE_UNDO)) {
        fail("default updateUndoLocator changed more than BLOCK_HAVE_UNDO");
    }
    if ((read.value().status_flags & BLOCK_VALID_SCRIPTS) != 0) {
        fail("default updateUndoLocator granted BLOCK_VALID_SCRIPTS — callers "
             "that pass no extra bits must never stamp script validity");
    }
    pass("updateUndoLocator default adds no extra bits (caller contract only)");
}

// Issue #453 — FAULT INJECTION: a failed stage must persist NOTHING.
//
// This is the durability-ordering guarantee the #453 fix depends on: script
// validity may only become durable as part of a stage that succeeded. If the
// stage fails, the row must be byte-identical to its prior state — no
// BLOCK_VALID_SCRIPTS, no BLOCK_HAVE_UNDO, no undo locator.
//
// The injection needs no test hook and no production change: updateUndoLocator
// returns Status::Invalid when undo_size == 0. That matters for ConnectTip
// specifically, because Invalid is NOT NotFound, so the updateBlockIndex
// fallback does not fire and control reaches the fail-closed branch
// (Abort() -> DisconnectBlock() -> return fail("persist-undo-metadata-stage-
// failed-status-...")), meaning acceptance cannot report success.
//
// SCOPE: this covers "stage failure returns failure", "the scripts-valid bit is
// not persisted", and "no partial undo metadata is left behind". It does NOT
// cover the daemon-level properties (active tip not advanced; retry after
// clearing the failure yields a durable 415) — those need a running node.
void test11_FailedStagePersistsNothing() {
    cleanTestDatadir();
    ChainDB db;
    auto init_status = db.init(TEST_DATADIR);
    if (init_status != Status::Ok) {
        fail("ChainDB::init failed (status=" +
             std::to_string(static_cast<int>(init_status)) + ")");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 hash = uint256::FromHexUnsafe(
        "bbbb000000000000000000000000000000000000000000000000000000000000");

    // The acceptance-time row: no BLOCK_VALID_SCRIPTS, no BLOCK_HAVE_UNDO.
    const uint32_t initial = bits(
        BLOCK_VALID_HEADER, BLOCK_VALID_TREE, BLOCK_VALID_TRANSACTIONS,
        BLOCK_VALID_CHAIN, BLOCK_HAVE_DATA);
    auto metadata = makeHealthyHeader(initial);
    metadata.undo_file = 0;
    metadata.undo_pos = 0;
    metadata.undo_size = 0;
    metadata.file_number = 21;
    metadata.data_pos = 909;
    metadata.data_size = 808;
    db.putHeaderMetadata(token, hash, metadata);

    auto before = db.getHeaderMetadata(hash);
    if (!before.ok()) {
        fail("getHeaderMetadata failed before injected-failure stage");
    }

    // Inject: undo_size == 0 forces Status::Invalid.
    auto injected = db.updateUndoLocator(token, hash, 5, 6, 0, nullptr,
                                          BLOCK_VALID_SCRIPTS);
    if (injected == Status::Ok) {
        fail("injected updateUndoLocator failure did not fail (undo_size=0 must "
             "return Invalid)");
    }
    if (injected == Status::NotFound) {
        fail("injected failure returned NotFound — ConnectTip would take the "
             "updateBlockIndex fallback instead of the fail-closed branch, so "
             "this injection would not exercise the path under test");
    }

    auto after = db.getHeaderMetadata(hash);
    if (!after.ok()) {
        fail("getHeaderMetadata failed after injected-failure stage");
    }

    if ((after.value().status_flags & BLOCK_VALID_SCRIPTS) != 0) {
        fail("failed stage still persisted BLOCK_VALID_SCRIPTS — script "
             "validity must never become durable through a stage that failed");
    }
    if ((after.value().status_flags & BLOCK_HAVE_UNDO) != 0) {
        fail("failed stage persisted BLOCK_HAVE_UNDO");
    }
    if (after.value().status_flags != before.value().status_flags) {
        fail("failed stage changed status_flags (" +
             std::to_string(before.value().status_flags) + " -> " +
             std::to_string(after.value().status_flags) + ")");
    }
    if (after.value().undo_file != 0 || after.value().undo_pos != 0 ||
        after.value().undo_size != 0) {
        fail("failed stage left partial undo metadata behind");
    }
    if (after.value().file_number != 21 || after.value().data_pos != 909 ||
        after.value().data_size != 808) {
        fail("failed stage clobbered block body position");
    }
    pass("failed stage persists nothing (no SCRIPTS, no HAVE_UNDO, no locator)");

    // Retry with the failure condition removed must succeed and produce the
    // durable, fully-validated status.
    auto retry = db.updateUndoLocator(token, hash, 5, 6, 7, nullptr,
                                       BLOCK_VALID_SCRIPTS);
    if (retry != Status::Ok) {
        fail("retry after clearing the injected failure did not succeed");
    }
    auto retried = db.getHeaderMetadata(hash);
    if (!retried.ok()) {
        fail("getHeaderMetadata failed after retry");
    }
    const uint32_t expected =
        bits(initial, BLOCK_HAVE_UNDO, BLOCK_VALID_SCRIPTS);
    if (retried.value().status_flags != expected) {
        fail("retry did not produce the durable validated status (got " +
             std::to_string(retried.value().status_flags) + ", want " +
             std::to_string(expected) + ")");
    }
    if (retried.value().undo_size != 7) {
        fail("retry did not write the undo locator");
    }
    pass("retry after clearing the failure persists the validated status");
}

// A canonical header-only row must not be mistaken for corrupt legacy
// metadata when its first byte happens to equal metadata schema version 1/2.
void test12_HeaderFirstRowAcceptsMetadata() {
    cleanTestDatadir();
    ChainDB db;
    if (db.init(TEST_DATADIR) != Status::Ok) {
        fail("ChainDB::init failed");
    }

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    BlockHeader header;
    header.version = 2;
    const uint256 hash = header.GetHash();
    if (db.putHeader(token, hash, header, 77, arith_uint256()) != Status::Ok) {
        fail("putHeader failed");
    }
    if (db.getHeaderMetadata(hash).status() != Status::NotFound) {
        fail("canonical header-only row was misclassified as legacy metadata");
    }

    auto metadata = makeHealthyHeader(BLOCK_HAVE_DATA);
    metadata.height = 77;
    if (db.putHeaderMetadataPreservingExistingUndo(token, hash, metadata) != Status::Ok) {
        fail("metadata write rejected a legitimate header-first row");
    }
    auto read = db.getHeaderMetadata(hash);
    if (!read.ok() || read.value().height != 77) {
        fail("metadata was not readable after header-first update");
    }
    if (!db.getHeader(hash).ok()) {
        fail("metadata update clobbered the canonical header");
    }
    pass("header-first row accepts independent metadata without collision");
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
    test09_UpdateUndoLocatorMergesExtraStatusBits();
    test10_UpdateUndoLocatorDefaultAddsNoExtraBits();
    test11_FailedStagePersistsNothing();
    test12_HeaderFirstRowAcceptsMetadata();

    std::cout << "\n✅ Header status bits: 12/12 properties hold" << std::endl;
    cleanTestDatadir();
    return 0;
}
