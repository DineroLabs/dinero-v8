// Test/operator harness for an existing, isolated OFFLINE DATADIR COPY.
// Intentionally not installed and never wired into a daemon or live scheduler.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

#include <sqlite3.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "dinero/core/consensus/chainparams.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "storage/checkpoint_retention.h"

namespace {
namespace fs = std::filesystem;
using dinero::Status;
using dinero::storage::CheckpointRetentionPass;
using dinero::storage::CheckpointRetentionPolicy;
using dinero::storage::CheckpointRetentionProgress;

struct Options {
    fs::path source, copy;
    std::string network;
    CheckpointRetentionPolicy policy;
    uint32_t pause_ms = 10;
    uint32_t crash_after_delete_batches = 0;
    bool apply = false, compact = false, help = false;
};

struct SnapshotBase {
    std::string metadata_key;
    uint32_t height;
    std::optional<dinero::uint256> hash;
};

std::string Quote(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

[[noreturn]] void Reject(const std::string& reason) {
    throw std::runtime_error(reason);
}

uint32_t Number(const std::string& value, const std::string& name,
                bool allow_zero = false) {
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || (!allow_zero && result == 0)) {
        Reject("invalid unsigned integer for " + name + ": " + value);
    }
    return result;
}

void Usage() {
    std::cout <<
        "Usage: utreexo_retention_copy --source-datadir SRC --copy-datadir COPY\n"
        "  --network mainnet|testnet|regtest [options]\n"
        "\n"
        "SRC and COPY must already exist, resolve to disjoint directories, and\n"
        "represent a source and an offline copy prepared by the operator. The\n"
        "source is used only for path/inode comparisons; it is never opened as\n"
        "a database. This program does not create or certify a source copy.\n"
        "COPY/blockchain/chaindb must contain CURRENT and its MANIFEST. Symlinks\n"
        "and hardlinked regular database files are rejected. The normal RocksDB\n"
        "lock must be available. COPY/blockchain/utxo must be a readable SQLite\n"
        "database with utxo_metadata; snapshot markers are protected and checked.\n"
        "\n"
        "Audit is the default: it verifies candidates without deleting U/C keys.\n"
        "Opening the copy can still update RocksDB logs or schema metadata.\n"
        "\n"
        "  --apply                     Delete verified interior checkpoints\n"
        "  --compact                   Explicit compaction after clean --apply\n"
        "  --recent-blocks N           Default 2000 (positive)\n"
        "  --historical-interval N     Default 5000 (positive)\n"
        "  --replay-per-step N         Default 16 (positive)\n"
        "  --delete-per-batch N        Default 128 (positive)\n"
        "  --protect-height H          Repeatable extra retained anchor\n"
        "  --pause-ms N                Default 10; range 0..60000\n"
        "  --crash-after-delete-batches N\n"
        "                              Test only: _Exit(86) after N committed batches\n"
        "  --help                      Print this usage\n"
        "\n"
        "Output uses key=value records. Eligible/deleted counts are integer\n"
        "height SLOTS, including absent keys, never reclaimed-byte counts.\n"
        "Exit: 0 complete, 2 rejected/failed, 3 skipped intervals, 86 injected crash.\n";
}

Options Parse(int argc, char** argv) {
    Options options;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") { options.help = true; continue; }
        if (arg != "--protect-height" && !seen.insert(arg).second)
            Reject("duplicate option: " + arg);
        if (arg == "--apply") { options.apply = true; continue; }
        if (arg == "--compact") { options.compact = true; continue; }
        const bool takes_value = arg == "--source-datadir" || arg == "--copy-datadir" ||
            arg == "--network" || arg == "--recent-blocks" ||
            arg == "--historical-interval" || arg == "--replay-per-step" ||
            arg == "--delete-per-batch" || arg == "--protect-height" ||
            arg == "--pause-ms" || arg == "--crash-after-delete-batches";
        if (!takes_value) Reject("unknown option: " + arg);
        if (++i == argc) Reject("missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--source-datadir") options.source = value;
        else if (arg == "--copy-datadir") options.copy = value;
        else if (arg == "--network") options.network = value;
        else if (arg == "--recent-blocks") options.policy.recent_blocks = Number(value, arg);
        else if (arg == "--historical-interval") options.policy.historical_interval = Number(value, arg);
        else if (arg == "--replay-per-step") options.policy.replay_blocks_per_step = Number(value, arg);
        else if (arg == "--delete-per-batch") options.policy.delete_heights_per_batch = Number(value, arg);
        else if (arg == "--protect-height") {
            const auto height = Number(value, arg, true);
            if (height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
                Reject("protected height exceeds ChainDB height range");
            options.policy.protected_heights.push_back(height);
        } else if (arg == "--pause-ms") {
            options.pause_ms = Number(value, arg, true);
            if (options.pause_ms > 60000) Reject("pause-ms exceeds 60000");
        } else if (arg == "--crash-after-delete-batches") {
            options.crash_after_delete_batches = Number(value, arg);
        }
    }
    if (options.help) return options;
    if (options.source.empty() || options.copy.empty())
        Reject("both --source-datadir and --copy-datadir are required");
    if (options.network != "mainnet" && options.network != "testnet" &&
        options.network != "regtest") Reject("explicit valid --network is required");
    if (options.compact && !options.apply) Reject("--compact requires --apply");
    if (options.crash_after_delete_batches && !options.apply)
        Reject("--crash-after-delete-batches requires --apply");
    return options;
}

bool IsWithin(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) return false;
    }
    return true;
}

void CheckUnsharedEntry(const fs::path& path, bool require_directory = false) {
    struct stat entry{};
    if (::lstat(path.c_str(), &entry) != 0) Reject("cannot lstat copy entry: " + path.string());
    if (S_ISLNK(entry.st_mode)) Reject("copy symlink refused: " + path.string());
    if (require_directory && !S_ISDIR(entry.st_mode))
        Reject("expected copy directory: " + path.string());
    if (S_ISREG(entry.st_mode)) {
        if (entry.st_nlink > 1) Reject("copy hardlink refused: " + path.string());
    } else if (!S_ISDIR(entry.st_mode)) {
        Reject("nonregular copy entry refused: " + path.string());
    }
}

void CheckPaths(Options& options) {
    if (!fs::is_directory(options.source) || !fs::is_directory(options.copy))
        Reject("source and copy datadir roots must already be directories");
    options.source = fs::canonical(options.source);
    options.copy = fs::canonical(options.copy);
    if (IsWithin(options.source, options.copy) || IsWithin(options.copy, options.source) ||
        fs::equivalent(options.source, options.copy))
        Reject("source and copy aliases or nested roots refused");
    const auto blockchain = options.copy / "blockchain";
    const auto chaindb = blockchain / "chaindb";
    CheckUnsharedEntry(blockchain, true);
    CheckUnsharedEntry(chaindb, true);
    // Inode comparison also catches unusual aliases not apparent in paths.
    const auto source_db = options.source / "blockchain" / "chaindb";
    if (fs::exists(source_db) && fs::equivalent(source_db, chaindb))
        Reject("source and copy chaindb inode aliases refused");
    for (const auto& entry : fs::recursive_directory_iterator(chaindb))
        CheckUnsharedEntry(entry.path());
    if (!fs::is_regular_file(chaindb / "CURRENT"))
        Reject("copy chaindb CURRENT is missing");
    if (fs::file_size(chaindb / "CURRENT") > 4096)
        Reject("copy chaindb CURRENT is oversized");
    std::ifstream current(chaindb / "CURRENT");
    std::string manifest;
    if (!std::getline(current, manifest) || manifest.rfind("MANIFEST-", 0) != 0 ||
        manifest.size() <= 9 || !std::all_of(manifest.begin() + 9, manifest.end(),
            [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
        !fs::is_regular_file(chaindb / manifest))
        Reject("copy chaindb CURRENT does not name an existing MANIFEST");

    const auto utxo = blockchain / "utxo";
    CheckUnsharedEntry(utxo);
    if (!fs::is_regular_file(utxo)) Reject("copy blockchain/utxo must be a SQLite file");
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        const fs::path companion = utxo.string() + suffix;
        if (fs::symlink_status(companion).type() != fs::file_type::not_found)
            CheckUnsharedEntry(companion);
    }
}

std::vector<SnapshotBase> ReadSnapshotMetadata(const fs::path& utxo) {
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(utxo.c_str(), &raw,
                                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sqlite(raw, sqlite3_close);
    if (opened != SQLITE_OK)
        Reject("copy UTXO SQLite read-only open failed: " +
               std::string(raw ? sqlite3_errmsg(raw) : "no connection"));
    if (sqlite3_db_readonly(raw, "main") != 1)
        Reject("copy UTXO SQLite connection is not read-only");

    const char* query =
        "SELECT key,value FROM utxo_metadata WHERE key IN ("
        "'assumeutxo_base_height','assumeutxo_base_block',"
        "'assumeutxo_lc_base_height','assumeutxo_lc_base_block',"
        "'wallet_snapshot_recovery_base_height')";
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(raw, query, -1, &statement, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(statement, sqlite3_finalize);
    if (prepared != SQLITE_OK)
        Reject("copy UTXO metadata query failed: " + std::string(sqlite3_errmsg(raw)));
    std::map<std::string, std::string> metadata;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        if (sqlite3_column_type(statement, 0) == SQLITE_NULL ||
            sqlite3_column_type(statement, 1) == SQLITE_NULL)
            Reject("null copy UTXO snapshot metadata refused");
        const auto column = [&](int i) {
            const auto* value = sqlite3_column_text(statement, i);
            return std::string(reinterpret_cast<const char*>(value),
                               sqlite3_column_bytes(statement, i));
        };
        if (!metadata.emplace(column(0), column(1)).second)
            Reject("duplicate copy UTXO snapshot metadata refused");
    }
    if (status != SQLITE_DONE)
        Reject("copy UTXO metadata read failed: " + std::string(sqlite3_errmsg(raw)));

    std::vector<SnapshotBase> bases;
    for (const auto* prefix : {"assumeutxo_base_", "assumeutxo_lc_base_"}) {
        const std::string height_key = std::string(prefix) + "height";
        const std::string block_key = std::string(prefix) + "block";
        const auto height = metadata.find(height_key), block = metadata.find(block_key);
        if (height == metadata.end() && block == metadata.end()) continue;
        if (height == metadata.end() || block == metadata.end())
            Reject("incomplete copy UTXO snapshot metadata: " + height_key);
        const uint32_t parsed_height = Number(height->second, height_key);
        if (parsed_height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            Reject("copy UTXO snapshot height exceeds ChainDB height range");
        dinero::uint256 parsed_hash;
        if (block->second.size() != 64 ||
            !dinero::uint256::FromHex(block->second, parsed_hash) || parsed_hash.IsNull())
            Reject("invalid copy UTXO snapshot hash: " + block_key);
        bases.push_back({height_key, parsed_height, parsed_hash});
    }
    const std::string recovery_key = "wallet_snapshot_recovery_base_height";
    const auto recovery = metadata.find(recovery_key);
    if (recovery != metadata.end()) {
        const auto height = Number(recovery->second, recovery_key);
        if (height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            Reject("copy wallet recovery height exceeds ChainDB height range");
        bases.push_back({recovery_key, height, std::nullopt});
    }
    return bases;
}

void ValidateSnapshotMetadata(const dinero::ChainDB& db,
                              const std::vector<SnapshotBase>& bases,
                              uint32_t tip_height) {
    for (const auto& base : bases) {
        if (base.height > tip_height) Reject("copy UTXO snapshot base is above chain tip");
        const auto hash = db.getBlockHashByHeight(static_cast<int>(base.height));
        if (!hash.ok() || (base.hash && hash.value() != *base.hash))
            Reject("copy UTXO snapshot hash disagrees with height index: " + base.metadata_key);
        if (!db.getHeader(hash.value()).ok())
            Reject("copy UTXO snapshot base header is missing: " + base.metadata_key);
        if (!db.getLatestUtreexoCheckpointAtOrBelow(static_cast<int>(base.height)).ok())
            Reject("copy UTXO snapshot base has no checkpoint at or below its height: " +
                   base.metadata_key);
        // Retention independently validates the full ancestry and index
        // before deleting; this binds SQLite's marker to that checked index.
        std::cout << "event=snapshot_base key=" << Quote(base.metadata_key)
                  << " height=" << base.height << " hash=" << Quote(hash.value().GetHex()) << '\n';
    }
}

void PrintProgress(const char* event, const CheckpointRetentionProgress& progress,
                   uint64_t delete_batches) {
    std::cout << "event=" << event
              << " phase=" << static_cast<unsigned>(progress.phase)
              << " interval_start=" << progress.interval_start
              << " interval_end=" << progress.interval_end
              << " replayed_height=" << progress.replayed_height
              << " replayed_blocks=" << progress.replayed_blocks
              << " eligible_height_slots=" << progress.eligible_height_keys
              << " deleted_height_slots=" << progress.deleted_height_keys
              << " delete_batches=" << delete_batches
              << " verified_intervals=" << progress.verified_intervals
              << " skipped_intervals=" << progress.skipped_intervals
              << " last_skip_reason=" << Quote(progress.last_skip_reason) << '\n';
}

int Execute(Options options) {
#if defined(__APPLE__) && TARGET_OS_IOS
    Reject("copy retention harness requires the normal desktop/server RocksDB lock");
#endif
    CheckPaths(options);
    const auto bases = ReadSnapshotMetadata(options.copy / "blockchain" / "utxo");
    for (const auto& base : bases) options.policy.protected_heights.push_back(base.height);
    std::sort(options.policy.protected_heights.begin(), options.policy.protected_heights.end());
    options.policy.protected_heights.erase(std::unique(options.policy.protected_heights.begin(),
        options.policy.protected_heights.end()), options.policy.protected_heights.end());
    std::string protected_heights;
    for (uint32_t height : options.policy.protected_heights) {
        if (!protected_heights.empty()) protected_heights += ',';
        protected_heights += std::to_string(height);
    }
    std::cout << "event=policy mode=" << (options.apply ? "apply" : "audit")
              << " source_datadir=" << Quote(options.source.string())
              << " copy_datadir=" << Quote(options.copy.string())
              << " network=" << Quote(options.network)
              << " recent_blocks=" << options.policy.recent_blocks
              << " historical_interval=" << options.policy.historical_interval
              << " replay_per_step=" << options.policy.replay_blocks_per_step
              << " delete_per_batch=" << options.policy.delete_heights_per_batch
              << " pause_ms=" << options.pause_ms
              << " compact=" << (options.compact ? "true" : "false")
              << " protected_heights=" << Quote(protected_heights)
              << " sqlite_metadata=loaded_read_only\n";

    dinero::SelectParams(options.network == "mainnet" ? dinero::Chain::MAINNET :
        options.network == "testnet" ? dinero::Chain::TESTNET : dinero::Chain::REGTEST);
    dinero::ChainDB db;
    const auto opened = db.init(options.copy / "blockchain" / "chaindb");
    if (opened != Status::Ok)
        Reject("copy ChainDB open failed (including unavailable lock): " +
               std::string(dinero::StatusToString(opened)));
    const auto tip = db.getTip();
    if (!tip.ok() || tip.value().height < 0) Reject("copy ChainDB tip missing or invalid");
    ValidateSnapshotMetadata(db, bases, static_cast<uint32_t>(tip.value().height));
    std::cout << "event=sqlite_metadata status=verified snapshot_base_count="
              << bases.size() << '\n';
    std::cout << "event=tip height=" << tip.value().height
              << " hash=" << Quote(tip.value().hash.GetHex()) << '\n';

    const auto token = dinero::ChainWriteToken::CreateForTesting();
    CheckpointRetentionPass pass(db, options.policy, options.apply);
    uint64_t batches = 0, deleted = 0, skipped = 0, steps = 0;
    auto last_report = std::chrono::steady_clock::now();
    while (true) {
        std::string error;
        const uint32_t interval_start_before_step = pass.progress().interval_start;
        const auto status = pass.step(token, error);
        // Failures poison done(); inspect Status first so a rejected step
        // can never be accidentally reported as successful completion.
        if (status != Status::Ok) {
            PrintProgress("failed_progress", pass.progress(), batches);
            Reject("retention step failed: " + std::string(dinero::StatusToString(status)) +
                   ": " + error);
        }
        const auto& progress = pass.progress();
        ++steps;
        if (progress.deleted_height_keys > deleted) {
            ++batches;
            deleted = progress.deleted_height_keys;
            if (options.crash_after_delete_batches && batches >= options.crash_after_delete_batches) {
                PrintProgress("injected_crash", progress, batches);
                std::cout << "event=result status=injected_crash exit_code=86\n" << std::flush;
                std::cerr.flush();
                std::_Exit(86);
            }
        }
        if (progress.skipped_intervals > skipped) {
            // The engine advances interval_start when skipping. Preserve the
            // pre-step start so the reported range identifies the failed span.
            std::cout << "event=skipped_interval interval_start=" << interval_start_before_step
                      << " interval_end=" << progress.interval_end
                      << " skipped_intervals=" << progress.skipped_intervals
                      << " reason=" << Quote(progress.last_skip_reason) << '\n';
            skipped = progress.skipped_intervals;
        }
        if (pass.done()) break;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(10)) {
            PrintProgress("progress", progress, batches);
            std::cout.flush();
            last_report = now;
        }
        if (options.pause_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(options.pause_ms));
    }
    PrintProgress("summary", pass.progress(), batches);
    if (pass.progress().skipped_intervals) {
        std::cout << "event=result status=skipped_intervals exit_code=3 steps=" << steps
                  << " compaction=not_performed\n";
        return 3;
    }
    if (options.compact) {
        std::cout << "event=compaction status=started\n" << std::flush;
        const auto compacted = db.compactUtreexoForTesting();
        if (compacted != Status::Ok)
            Reject("explicit copy compaction failed: " +
                   std::string(dinero::StatusToString(compacted)));
        std::cout << "event=compaction status=complete\n";
    }
    std::cout << "event=result status=complete exit_code=0 steps=" << steps
              << " mode=" << (options.apply ? "apply" : "audit") << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto options = Parse(argc, argv);
        if (options.help) { Usage(); return 0; }
        return Execute(std::move(options));
    } catch (const std::exception& error) {
        std::cerr << "event=result status=rejected_or_failed exit_code=2 reason="
                  << Quote(error.what()) << '\n';
        return 2;
    }
}
