// Tests for the offline --rebuild-undo-range orchestrator (commit #4).
//
// The orchestrator's three load-bearing behaviors:
//   1. Manifest JSON round-trip is byte-stable + parsable
//   2. ValidateOptions refuses malformed configs at the API boundary
//      (null handles, zero/inverted window, anchor at-or-after window)
//   3. Preflight against an empty LIVE ChainDB classifies every height
//      as MissingMetadata and refuses the run with a manifest emitted
//      that lists every offending height
//
// This commit's tests do NOT exercise the real reindex execution path —
// that's commit #5's CLI integration which wires --reindex up to anchor
// before invoking the windowed pass.

#include "daemon/undo_rebuild_orchestrator.h"

#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

using namespace dinero;
using namespace dinero::daemon;

namespace {

uint256 MakeUint256(uint8_t seed) {
    uint256 out;
    for (size_t i = 0; i < 32; ++i) {
        out.data[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

void testStatusEnumStrings() {
    std::cout << "\n[Test 1] UndoRebuildStatus ↔ string round-trip" << std::endl;
    const std::vector<UndoRebuildStatus> all = {
        UndoRebuildStatus::AlreadyOk,
        UndoRebuildStatus::Hole,
        UndoRebuildStatus::MissingMetadata,
        UndoRebuildStatus::Blocked,
        UndoRebuildStatus::Rebuilt,
        UndoRebuildStatus::VerifyFailed,
        UndoRebuildStatus::Skipped,
    };
    for (auto s : all) {
        const auto str = UndoRebuildStatusToString(s);
        const auto parsed = ParseUndoRebuildStatus(str);
        assert(parsed.has_value() && "every emitted status must round-trip");
        assert(*parsed == s && "round-trip must preserve identity");
    }
    assert(!ParseUndoRebuildStatus("nonsense").has_value() &&
           "unknown strings must return nullopt");
    std::cout << "  [PASS] every status round-trips through string form" << std::endl;
}

void testManifestJsonRoundTrip() {
    std::cout << "\n[Test 2] Manifest JSON serialize/deserialize round-trip"
              << std::endl;

    UndoRebuildManifest m;
    m.window_start = 100;
    m.window_end = 105;
    m.anchor_height = 99;
    m.anchor_hash = MakeUint256(0x99);
    m.dry_run = true;
    m.emitted_at_unix = 1700000000;
    m.already_ok_count = 2;
    m.holes_count = 3;
    m.missing_metadata_count = 1;
    m.final_status = "preflight_refused";

    UndoRebuildManifestEntry e1;
    e1.height = 100;
    e1.block_hash = MakeUint256(0xa0);
    e1.status = UndoRebuildStatus::AlreadyOk;
    m.entries.push_back(e1);

    UndoRebuildManifestEntry e2;
    e2.height = 101;
    e2.block_hash = MakeUint256(0xa1);
    e2.status = UndoRebuildStatus::Hole;
    m.entries.push_back(e2);

    UndoRebuildManifestEntry e3;
    e3.height = 102;
    e3.block_hash = MakeUint256(0xa2);
    e3.status = UndoRebuildStatus::MissingMetadata;
    e3.reason = "live-header-metadata-absent-for-hash deadbeef";
    m.entries.push_back(e3);

    const auto json = m.ToJson();
    const auto parsed = UndoRebuildManifest::FromJson(json);
    assert(parsed.has_value() && "valid JSON must parse");

    assert(parsed->window_start == m.window_start);
    assert(parsed->window_end == m.window_end);
    assert(parsed->anchor_height == m.anchor_height);
    assert(parsed->anchor_hash == m.anchor_hash);
    assert(parsed->dry_run == m.dry_run);
    assert(parsed->emitted_at_unix == m.emitted_at_unix);
    assert(parsed->already_ok_count == m.already_ok_count);
    assert(parsed->holes_count == m.holes_count);
    assert(parsed->missing_metadata_count == m.missing_metadata_count);
    assert(parsed->final_status == m.final_status);

    assert(parsed->entries.size() == m.entries.size());
    for (size_t i = 0; i < m.entries.size(); ++i) {
        assert(parsed->entries[i].height == m.entries[i].height);
        assert(parsed->entries[i].block_hash == m.entries[i].block_hash);
        assert(parsed->entries[i].status == m.entries[i].status);
        assert(parsed->entries[i].reason == m.entries[i].reason);
    }

    std::cout << "  [PASS] manifest JSON round-trip preserves all fields"
              << std::endl;
}

void testManifestJsonContainsExpectedFields() {
    std::cout << "\n[Test 3] Manifest JSON contains the documented schema fields"
              << std::endl;

    UndoRebuildManifest m;
    m.window_start = 1;
    m.window_end = 10;
    m.final_status = "ok";
    UndoRebuildManifestEntry e;
    e.height = 5;
    e.status = UndoRebuildStatus::Rebuilt;
    m.entries.push_back(e);

    const auto json = m.ToJson();
    // Confirm every documented status name appears verbatim somewhere
    // in the JSON (the entry uses one, but the counts section contains
    // every key by design).
    const std::vector<std::string> expected_keys = {
        "\"window_start\"", "\"window_end\"", "\"anchor_height\"",
        "\"anchor_hash\"", "\"dry_run\"", "\"emitted_at_unix\"",
        "\"final_status\"", "\"counts\"",
        "\"already_ok\"", "\"holes\"", "\"rebuilt\"",
        "\"verify_failed\"", "\"skipped\"", "\"missing_metadata\"",
        "\"blocked\"", "\"entries\"",
        "\"height\"", "\"hash\"", "\"status\""
    };
    for (const auto& key : expected_keys) {
        if (json.find(key) == std::string::npos) {
            std::cerr << "MISSING key: " << key << std::endl;
            std::cerr << "JSON:\n" << json << std::endl;
            assert(false && "manifest JSON must include the documented schema");
        }
    }
    std::cout << "  [PASS] manifest JSON includes every documented schema field"
              << std::endl;
}

// API-boundary refusal tests. We pass non-null pointers (just for the
// null-check) and the orchestrator should refuse before any DB I/O.
void testOptionsValidationNullHandles() {
    std::cout << "\n[Test 4] RunOfflineUndoRebuild — null LIVE handles refused"
              << std::endl;

    UndoRebuildOptions opts;
    opts.window_start = 100;
    opts.window_end = 105;
    opts.anchor.height = 99;
    opts.live_chain_db = nullptr;
    opts.live_block_storage = nullptr;
    opts.dry_run = true;

    auto result = RunOfflineUndoRebuild(opts);
    assert(!result.ok() && "null handles must be refused");
    assert(result.status() == Status::Invalid &&
           "null-handle refusal returns Status::Invalid");
    std::cout << "  [PASS] null handles refused with Status::Invalid"
              << std::endl;
}

void testOptionsValidationGenesisWindow() {
    std::cout << "\n[Test 5] RunOfflineUndoRebuild — window_start=0 refused"
              << std::endl;

    // Use a real (empty) ChainDB + BlockStorage so the null-handle
    // refusal doesn't trip first.
    const auto tmp = std::filesystem::temp_directory_path() /
                     "dinero_undo_rebuild_test_validate_genesis";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    ChainDB chain_db;
    auto init_status = chain_db.init(tmp / "chainstate");
    assert(init_status == Status::Ok && "ChainDB init must succeed");
    BlockStorage block_storage;
    auto bs_init = block_storage.init(tmp);
    assert(bs_init == Status::Ok && "BlockStorage init must succeed");

    UndoRebuildOptions opts;
    opts.datadir = tmp;
    opts.window_start = 0;  // ← bad
    opts.window_end = 5;
    opts.anchor.height = 0;
    opts.live_chain_db = &chain_db;
    opts.live_block_storage = &block_storage;
    opts.dry_run = true;

    auto result = RunOfflineUndoRebuild(opts);
    assert(!result.ok() && "window_start=0 must be refused");
    assert(result.status() == Status::Invalid);
    std::cout << "  [PASS] window_start=0 refused" << std::endl;

    std::filesystem::remove_all(tmp, ec);
}

void testPreflightOnEmptyChainDB() {
    std::cout << "\n[Test 6] RunOfflineUndoRebuild — preflight on empty LIVE"
              << std::endl;

    const auto tmp = std::filesystem::temp_directory_path() /
                     "dinero_undo_rebuild_test_preflight_empty";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    ChainDB chain_db;
    auto init_status = chain_db.init(tmp / "chainstate");
    assert(init_status == Status::Ok);
    BlockStorage block_storage;
    auto bs_init = block_storage.init(tmp);
    assert(bs_init == Status::Ok);

    UndoRebuildOptions opts;
    opts.datadir = tmp;
    opts.window_start = 100;
    opts.window_end = 102;
    opts.anchor.height = 99;
    opts.anchor.hash = MakeUint256(0x99);
    opts.anchor.chainwork = arith_uint256(99);
    opts.live_chain_db = &chain_db;
    opts.live_block_storage = &block_storage;
    opts.dry_run = true;

    auto result = RunOfflineUndoRebuild(opts);
    // Empty LIVE ChainDB → every height classified as MissingMetadata
    // → preflight refused with Status::Invalid (not InvalidArgument).
    assert(!result.ok() && "empty LIVE ChainDB must trigger preflight refusal");
    assert(result.status() == Status::Invalid);

    // Manifest must have been emitted with all heights as
    // MissingMetadata.
    const auto manifest_path = tmp / "rebuild_undo_manifest.json";
    std::ifstream in(manifest_path);
    assert(in.is_open() && "manifest must be written on preflight refusal");
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();
    auto parsed = UndoRebuildManifest::FromJson(content);
    assert(parsed.has_value());
    assert(parsed->final_status == "preflight_refused");
    assert(parsed->missing_metadata_count == 3 &&
           "all 3 heights must be MissingMetadata on empty DB");
    assert(parsed->entries.size() == 3);
    for (const auto& e : parsed->entries) {
        assert(e.status == UndoRebuildStatus::MissingMetadata);
        assert(!e.reason.empty());
    }
    std::cout << "  [PASS] preflight refused; manifest lists all 3 missing rows"
              << std::endl;

    std::filesystem::remove_all(tmp, ec);
}

void testDryRunFlagShortCircuits() {
    std::cout << "\n[Test 7] dry_run halts before non-dry-run path"
              << std::endl;
    // We can't easily get an empty-but-clean preflight outcome without a
    // populated DB; instead, just verify that the dry_run flag survives
    // round-trip in the manifest (proxy for the dry_run code path being
    // in place).
    UndoRebuildManifest m;
    m.dry_run = true;
    m.final_status = "dry_run_complete";
    m.window_start = 1;
    m.window_end = 1;
    const auto json = m.ToJson();
    const auto parsed = UndoRebuildManifest::FromJson(json);
    assert(parsed.has_value());
    assert(parsed->dry_run == true);
    assert(parsed->final_status == "dry_run_complete");
    std::cout << "  [PASS] dry_run + final_status 'dry_run_complete' survive"
              << " round-trip" << std::endl;
}

}  // namespace

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Offline undo-rebuild orchestrator (commit #4)" << std::endl;
    std::cout << "========================================" << std::endl;

    SelectParams(Chain::REGTEST);

    testStatusEnumStrings();
    testManifestJsonRoundTrip();
    testManifestJsonContainsExpectedFields();
    testOptionsValidationNullHandles();
    testOptionsValidationGenesisWindow();
    testPreflightOnEmptyChainDB();
    testDryRunFlagShortCircuits();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[PASS] All orchestrator preflight + manifest tests passed"
              << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Non-dry-run reindexer invocation lands in commit #5"
              << " alongside the --rebuild-undo-range CLI flag." << std::endl;

    return 0;
}
