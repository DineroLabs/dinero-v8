// Qualification-only CLI driver for the reviewed shielded-state migration
// stack (MigrateShieldedDatadirCopy). This is a thin caller: it owns no
// migration logic, no lease/lock semantics and no recovery behavior — all of
// that lives in src/storage/shielded_migration*.cpp, which this file does
// not modify. It exists only because that engine currently has no CLI or
// operator entry point (see docs/design/shielded-state-migration-engine.md:
// "There is no daemon, NodeCore, RPC or operator CLI entry point"), and the
// combined-migration-release-qualification harness needs one to drive the
// real, reviewed engine against real regtest-produced datadirs instead of
// synthetic fixtures.
//
// Not installed, not linked into dinerod/NodeCore, and not a recommendation
// that this become a shipped operator tool — see the design docs above for
// the actual remaining gates (rollback enforcement, binary/datadir pairing,
// disk-headroom policy) before any such tool would be safe to ship.
//
// Usage:
//   migrate_shielded_datadir <original_datadir> <candidate_datadir> [--apply]
//
// The candidate datadir must already exist as a byte-identical copy of the
// original (see shielded-migration-cohort.md: the wrapper verifies, it does
// not create, the candidate's companion inventory) — this tool does not copy
// anything itself. Without --apply, runs an inspect-only pass (apply=false).
//
// Prints one line of machine-readable key=value pairs from the resulting
// ShieldedMigrationResult, then a human-readable summary. Exit code 0 only
// when result.ok is true; nonzero (with the failing field printed) otherwise.

#include "consensus/chainparams.h"
#include "storage/shielded_migration_cohort.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s <original_datadir> <candidate_datadir> [--apply]\n"
        "  Calls dinero::storage::MigrateShieldedDatadirCopy on two existing,\n"
        "  already-populated, disjoint native datadirs. Without --apply, runs\n"
        "  inspect-only (apply=false). See tools/migrate_shielded_datadir.cpp\n"
        "  for the exact ownership boundary this tool sits inside.\n",
        argv0);
}

// Matches the fixed defaults already reviewed and exercised in
// tests/storage/test_shielded_migration_cohort.cpp, scaled up from that
// file's tiny synthetic-fixture values to comfortably cover a real regtest
// chain's actual row counts. These are explicit qualification-harness
// choices, not a measured device/production resource budget — the engine's
// own docs are explicit that these limits are not a memory or disk-headroom
// guarantee (docs/design/shielded-state-migration-engine.md).
dinero::storage::ShieldedMigrationLimits DatabaseLimits() {
    return dinero::storage::ShieldedMigrationLimits{
        /*batch_bytes=*/4ull * 1024 * 1024,
        /*batch_rows=*/2000,
        /*max_record_bytes=*/1ull * 1024 * 1024,
        /*max_nullifiers=*/1000000,
    };
}

dinero::storage::ShieldedCompanionLimits CompanionLimits() {
    return dinero::storage::ShieldedCompanionLimits{
        /*max_entries=*/100000,
        /*max_bytes=*/8ull * 1024 * 1024 * 1024,
        /*max_metadata_rows=*/100000,
        /*max_metadata_value_bytes=*/1ull * 1024 * 1024,
        /*max_sqlite_steps=*/50000000,
        /*max_ancestry_headers=*/1000000,
        /*max_forest_record_bytes=*/16ull * 1024 * 1024,
        /*max_forest_leaves=*/10000000,
        /*max_replay_blocks=*/1000000,
        /*max_checkpoints=*/1000000,
    };
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::filesystem::path original(argv[1]);
    const std::filesystem::path candidate(argv[2]);
    bool apply = false;
    if (argc == 4) {
        const std::string flag(argv[3]);
        if (flag != "--apply") {
            PrintUsage(argv[0]);
            return 2;
        }
        apply = true;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(original, ec) || ec) {
        std::fprintf(stderr, "original datadir does not exist or is not a directory: %s\n",
                      original.string().c_str());
        return 2;
    }
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        std::fprintf(stderr,
            "candidate datadir does not exist or is not a directory: %s\n"
            "(this tool does not create the candidate — copy the original's\n"
            "full datadir there first; see this file's own usage comment)\n",
            candidate.string().c_str());
        return 2;
    }

    // This qualification-only tool exists solely to drive
    // MigrateShieldedDatadirCopy against real REGTEST-produced datadirs (see
    // this file's header comment). The forest audit inside the migration
    // engine uses network-specific consensus parameters and now explicitly
    // requires SelectParams() to have been called first (previously this
    // driver selected none, which the corrected engine surfaces as "Chain
    // parameters not selected. Call SelectParams() first." rather than
    // silently defaulting). REGTEST is hardcoded deliberately: this is a
    // qualification harness for disposable regtest datadirs only, never
    // production data, and a future operator tool needs its own explicit,
    // separately reviewed network/profile selection contract — not this one.
    dinero::SelectParams(dinero::Chain::REGTEST);

    const auto checkpoint = [](const char* phase) {
        std::fprintf(stderr, "[migrate_shielded_datadir] phase=%s\n", phase);
    };

    const auto result = dinero::storage::MigrateShieldedDatadirCopy(
        original, candidate, DatabaseLimits(), CompanionLimits(), apply, checkpoint);

    std::printf(
        "ok=%s ready=%s selected_rows=%llu retired_rows=%llu phase=%s operation=%s "
        "source_digest=%s error=%s\n",
        result.ok ? "true" : "false",
        result.ready ? "true" : "false",
        static_cast<unsigned long long>(result.selected_rows),
        static_cast<unsigned long long>(result.retired_rows),
        result.phase.c_str(),
        result.operation.c_str(),
        result.source_digest.c_str(),
        result.error.c_str());

    if (!result.ok) {
        std::fprintf(stderr, "migration did not complete: %s\n", result.error.c_str());
        return 1;
    }
    return 0;
}
