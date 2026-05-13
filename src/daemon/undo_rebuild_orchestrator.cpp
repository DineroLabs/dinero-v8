// Implementation of the offline `--rebuild-undo-range` orchestrator. See the
// header for lifecycle semantics + guardrails. This file is consensus-adjacent
// but lives in `daemon/` because it owns ChainDB lifecycle and filesystem
// resources, not consensus state.

#include "daemon/undo_rebuild_orchestrator.h"

#include "common/logger.h"
#include "consensus/block_lifecycle.h"  // BLOCK_HAVE_UNDO
#include "primitives/block.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace dinero {
namespace daemon {

namespace {

// Hex-encoded fixed-width helper that writes 32-byte block hashes in the
// same flavor as `uint256::GetHex()` — kept inline so the manifest format
// stays stable independent of any changes to that helper.
std::string HashHex(const uint256& h) {
    return h.GetHex();
}

uint256 HashFromHex(const std::string& hex) {
    uint256 out;
    if (hex.size() != 64) {
        out.SetNull();
        return out;
    }
    // GetHex emits little-endian display; uint256 has its own SetHex
    // through the inverse, but inline a parser to avoid a dependency
    // shuffle:
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[2 * (31 - i) + 0]);
        const int lo = nibble(hex[2 * (31 - i) + 1]);
        if (hi < 0 || lo < 0) {
            out.SetNull();
            return out;
        }
        out.data[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Per-height preflight read of the LIVE chain DB + block storage.
// Sets entry.status to one of {AlreadyOk, Hole, MissingMetadata, Blocked}
// and populates entry.block_hash + entry.reason where applicable.
void ClassifyOneHeight(uint32_t height,
                       ChainDB& live_chain_db,
                       BlockStorage& live_block_storage,
                       UndoRebuildManifestEntry& entry) {
    entry.height = height;

    // Step A: canonical hash at height.
    auto hash_result = live_chain_db.getBlockHashByHeight(static_cast<int>(height));
    if (!hash_result.ok()) {
        entry.status = UndoRebuildStatus::MissingMetadata;
        entry.reason = "live-chain-db-no-hash-at-height " + std::to_string(height);
        return;
    }
    entry.block_hash = hash_result.value();

    // Step B: header metadata exists for that hash.
    auto meta_result = live_chain_db.getHeaderMetadata(entry.block_hash);
    if (!meta_result.ok()) {
        entry.status = UndoRebuildStatus::MissingMetadata;
        entry.reason = "live-header-metadata-absent-for-hash " + HashHex(entry.block_hash);
        return;
    }
    const auto& meta = meta_result.value();

    // Step C: block body file readable. We don't deserialize — just
    // attempt the read. If it fails, we cannot re-apply the block in
    // the temp DB and the rebuild must refuse.
    if (meta.data_size == 0) {
        entry.status = UndoRebuildStatus::Blocked;
        entry.reason = "live-header-metadata-data_size-zero";
        return;
    }
    FilePosition body_pos;
    body_pos.file_number = meta.file_number;
    body_pos.offset = meta.data_pos;
    body_pos.size = meta.data_size;
    auto body_result = live_block_storage.readBlock(body_pos);
    if (!body_result.ok()) {
        entry.status = UndoRebuildStatus::Blocked;
        entry.reason = "live-block-body-unreadable file=" +
                       std::to_string(meta.file_number) +
                       " pos=" + std::to_string(meta.data_pos) +
                       " size=" + std::to_string(meta.data_size);
        return;
    }

    // Step D: classify undo presence.
    const bool has_undo_flag = (meta.status_flags & BLOCK_HAVE_UNDO) != 0;
    if (!has_undo_flag || meta.undo_size == 0) {
        entry.status = UndoRebuildStatus::Hole;
        entry.reason.clear();
        return;
    }

    // Step E: undo bytes readable. The metadata flag is not enough —
    // we want to confirm the rev*.dat slot decodes. If not, this is
    // also a Hole (rebuild candidate); the orchestrator will overwrite
    // the metadata pointer with a fresh, verified rebuild.
    FilePosition undo_pos;
    undo_pos.file_number = meta.undo_file;
    undo_pos.offset = meta.undo_pos;
    undo_pos.size = meta.undo_size;
    // BlockStorage::readUndo would be the symmetric API but commit #4
    // doesn't take a hard dependency on it — we trust the metadata flag
    // + size > 0 as the sufficient signal that prior code wrote a real
    // entry. If a future audit shows undo bytes silently corrupted, a
    // follow-up commit can deepen this check.
    (void)undo_pos;
    entry.status = UndoRebuildStatus::AlreadyOk;
    entry.reason.clear();
}

// Validate the basic shape of opts. Returns empty string on success,
// otherwise a reason string. Does NOT touch any I/O — purely an API
// boundary check.
std::string ValidateOptions(const UndoRebuildOptions& opts) {
    if (opts.live_chain_db == nullptr) {
        return "live_chain_db is null";
    }
    if (opts.live_block_storage == nullptr) {
        return "live_block_storage is null";
    }
    if (opts.window_start == 0) {
        return "window_start must be >= 1 (genesis cannot be rebuilt)";
    }
    if (opts.window_end < opts.window_start) {
        return "window_end (" + std::to_string(opts.window_end) +
               ") < window_start (" + std::to_string(opts.window_start) + ")";
    }
    if (opts.anchor.height >= opts.window_start) {
        return "anchor.height (" + std::to_string(opts.anchor.height) +
               ") must be < window_start (" + std::to_string(opts.window_start) + ")";
    }
    return {};
}

int64_t NowUnix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string UndoRebuildStatusToString(UndoRebuildStatus status) {
    switch (status) {
        case UndoRebuildStatus::AlreadyOk:       return "already_ok";
        case UndoRebuildStatus::Hole:            return "hole";
        case UndoRebuildStatus::MissingMetadata: return "missing_metadata";
        case UndoRebuildStatus::Blocked:         return "blocked";
        case UndoRebuildStatus::Rebuilt:         return "rebuilt";
        case UndoRebuildStatus::VerifyFailed:    return "verify_failed";
        case UndoRebuildStatus::Skipped:         return "skipped";
    }
    return "unknown";
}

std::optional<UndoRebuildStatus> ParseUndoRebuildStatus(const std::string& s) {
    if (s == "already_ok")       return UndoRebuildStatus::AlreadyOk;
    if (s == "hole")             return UndoRebuildStatus::Hole;
    if (s == "missing_metadata") return UndoRebuildStatus::MissingMetadata;
    if (s == "blocked")          return UndoRebuildStatus::Blocked;
    if (s == "rebuilt")          return UndoRebuildStatus::Rebuilt;
    if (s == "verify_failed")    return UndoRebuildStatus::VerifyFailed;
    if (s == "skipped")          return UndoRebuildStatus::Skipped;
    return std::nullopt;
}

std::string UndoRebuildManifest::ToJson() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"window_start\": " << window_start << ",\n";
    ss << "  \"window_end\": " << window_end << ",\n";
    ss << "  \"anchor_height\": " << anchor_height << ",\n";
    ss << "  \"anchor_hash\": \"" << HashHex(anchor_hash) << "\",\n";
    ss << "  \"dry_run\": " << (dry_run ? "true" : "false") << ",\n";
    ss << "  \"emitted_at_unix\": " << emitted_at_unix << ",\n";
    ss << "  \"final_status\": \"" << EscapeJson(final_status) << "\",\n";
    ss << "  \"counts\": {\n";
    ss << "    \"already_ok\": " << already_ok_count << ",\n";
    ss << "    \"holes\": " << holes_count << ",\n";
    ss << "    \"rebuilt\": " << rebuilt_count << ",\n";
    ss << "    \"verify_failed\": " << verify_failed_count << ",\n";
    ss << "    \"skipped\": " << skipped_count << ",\n";
    ss << "    \"missing_metadata\": " << missing_metadata_count << ",\n";
    ss << "    \"blocked\": " << blocked_count << "\n";
    ss << "  },\n";
    ss << "  \"entries\": [\n";
    // Stable ordering: sort by height ascending. Caller should already
    // pass a sorted vector but we sort defensively so the JSON is
    // diffable across runs regardless of construction order.
    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const UndoRebuildManifestEntry& a, const UndoRebuildManifestEntry& b) {
                  return a.height < b.height;
              });
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& e = sorted[i];
        ss << "    {";
        ss << "\"height\": " << e.height;
        ss << ", \"hash\": \"" << HashHex(e.block_hash) << "\"";
        ss << ", \"status\": \"" << UndoRebuildStatusToString(e.status) << "\"";
        if (!e.reason.empty()) {
            ss << ", \"reason\": \"" << EscapeJson(e.reason) << "\"";
        }
        ss << "}";
        if (i + 1 < sorted.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

std::optional<UndoRebuildManifest> UndoRebuildManifest::FromJson(const std::string& json) {
    // Minimal hand-rolled parser. The full project ships jsoncpp but
    // pulling it into this orchestrator increases the test-binary
    // dependency surface. The manifest schema is fixed and small;
    // parsing is bounded and deterministic.
    auto find_uint = [&](const std::string& key, uint64_t& out) -> bool {
        const auto needle = "\"" + key + "\"";
        const auto idx = json.find(needle);
        if (idx == std::string::npos) return false;
        const auto colon = json.find(':', idx + needle.size());
        if (colon == std::string::npos) return false;
        size_t pos = colon + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\n')) {
            ++pos;
        }
        size_t end = pos;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
        if (end == pos) return false;
        out = std::stoull(json.substr(pos, end - pos));
        return true;
    };
    auto find_str = [&](const std::string& key, std::string& out) -> bool {
        const auto needle = "\"" + key + "\"";
        const auto idx = json.find(needle);
        if (idx == std::string::npos) return false;
        const auto colon = json.find(':', idx + needle.size());
        if (colon == std::string::npos) return false;
        const auto open = json.find('"', colon + 1);
        if (open == std::string::npos) return false;
        const auto close = json.find('"', open + 1);
        if (close == std::string::npos) return false;
        out = json.substr(open + 1, close - (open + 1));
        return true;
    };
    auto find_bool = [&](const std::string& key, bool& out) -> bool {
        std::string s;
        const auto needle = "\"" + key + "\"";
        const auto idx = json.find(needle);
        if (idx == std::string::npos) return false;
        const auto colon = json.find(':', idx + needle.size());
        if (colon == std::string::npos) return false;
        const auto t = json.find("true", colon);
        const auto f = json.find("false", colon);
        const auto comma = json.find(',', colon);
        const auto brace = json.find('}', colon);
        const auto end = std::min(comma == std::string::npos ? brace : comma,
                                  brace == std::string::npos ? comma : brace);
        if (t != std::string::npos && t < end) { out = true; return true; }
        if (f != std::string::npos && f < end) { out = false; return true; }
        return false;
    };

    UndoRebuildManifest m;
    uint64_t v = 0;
    if (find_uint("window_start", v))            m.window_start = static_cast<uint32_t>(v);
    if (find_uint("window_end", v))              m.window_end = static_cast<uint32_t>(v);
    if (find_uint("anchor_height", v))           m.anchor_height = static_cast<uint32_t>(v);
    if (find_uint("emitted_at_unix", v))         m.emitted_at_unix = static_cast<int64_t>(v);
    if (find_uint("already_ok", v))              m.already_ok_count = v;
    if (find_uint("holes", v))                   m.holes_count = v;
    if (find_uint("rebuilt", v))                 m.rebuilt_count = v;
    if (find_uint("verify_failed", v))           m.verify_failed_count = v;
    if (find_uint("skipped", v))                 m.skipped_count = v;
    if (find_uint("missing_metadata", v))        m.missing_metadata_count = v;
    if (find_uint("blocked", v))                 m.blocked_count = v;

    std::string anchor_hex;
    if (find_str("anchor_hash", anchor_hex))     m.anchor_hash = HashFromHex(anchor_hex);
    find_str("final_status", m.final_status);
    find_bool("dry_run", m.dry_run);

    // Parse entries by walking object boundaries within the entries
    // array. Each entry is a single-line `{"height": ..., "hash": ..., ...}`.
    const auto entries_idx = json.find("\"entries\"");
    if (entries_idx == std::string::npos) {
        return m;  // No entries — valid for empty windows.
    }
    const auto arr_open = json.find('[', entries_idx);
    if (arr_open == std::string::npos) return m;
    const auto arr_close = json.find(']', arr_open);
    if (arr_close == std::string::npos) return std::nullopt;
    size_t cursor = arr_open + 1;
    while (cursor < arr_close) {
        const auto obj_open = json.find('{', cursor);
        if (obj_open == std::string::npos || obj_open > arr_close) break;
        const auto obj_close = json.find('}', obj_open);
        if (obj_close == std::string::npos || obj_close > arr_close) break;
        const auto obj = json.substr(obj_open, obj_close - obj_open + 1);

        UndoRebuildManifestEntry e;
        const auto height_pos = obj.find("\"height\"");
        if (height_pos != std::string::npos) {
            const auto colon = obj.find(':', height_pos);
            if (colon != std::string::npos) {
                size_t p = colon + 1;
                while (p < obj.size() && (obj[p] == ' ')) ++p;
                size_t end = p;
                while (end < obj.size() && obj[end] >= '0' && obj[end] <= '9') ++end;
                if (end > p) e.height = static_cast<uint32_t>(std::stoul(obj.substr(p, end - p)));
            }
        }
        std::string hash_hex;
        const auto hash_pos = obj.find("\"hash\"");
        if (hash_pos != std::string::npos) {
            const auto open = obj.find('"', hash_pos + 6);
            if (open != std::string::npos) {
                const auto close = obj.find('"', open + 1);
                if (close != std::string::npos) {
                    hash_hex = obj.substr(open + 1, close - (open + 1));
                }
            }
        }
        e.block_hash = HashFromHex(hash_hex);
        std::string status_str;
        const auto status_pos = obj.find("\"status\"");
        if (status_pos != std::string::npos) {
            const auto open = obj.find('"', status_pos + 8);
            if (open != std::string::npos) {
                const auto close = obj.find('"', open + 1);
                if (close != std::string::npos) {
                    status_str = obj.substr(open + 1, close - (open + 1));
                }
            }
        }
        if (auto parsed = ParseUndoRebuildStatus(status_str)) {
            e.status = *parsed;
        }
        const auto reason_pos = obj.find("\"reason\"");
        if (reason_pos != std::string::npos) {
            const auto open = obj.find('"', reason_pos + 8);
            if (open != std::string::npos) {
                const auto close = obj.find('"', open + 1);
                if (close != std::string::npos) {
                    e.reason = obj.substr(open + 1, close - (open + 1));
                }
            }
        }
        m.entries.push_back(std::move(e));
        cursor = obj_close + 1;
    }
    return m;
}

StatusOr<UndoRebuildManifest> RunOfflineUndoRebuild(const UndoRebuildOptions& opts) {
    if (const auto err = ValidateOptions(opts); !err.empty()) {
        g_logger.error("[undo-rebuild] options invalid: " + err);
        return Status::Invalid;
    }

    UndoRebuildManifest manifest;
    manifest.window_start = opts.window_start;
    manifest.window_end = opts.window_end;
    manifest.anchor_height = opts.anchor.height;
    manifest.anchor_hash = opts.anchor.hash;
    manifest.dry_run = opts.dry_run;

    g_logger.info("[undo-rebuild] starting orchestrator window=[" +
                  std::to_string(opts.window_start) + ", " +
                  std::to_string(opts.window_end) + "] anchor=" +
                  std::to_string(opts.anchor.height) +
                  (opts.dry_run ? " (DRY RUN)" : ""));

    // Phase 0: preflight classification (read-only on LIVE).
    bool any_blocked = false;
    bool any_missing_metadata = false;
    manifest.entries.reserve(opts.window_end - opts.window_start + 1);
    for (uint32_t h = opts.window_start; h <= opts.window_end; ++h) {
        UndoRebuildManifestEntry entry;
        ClassifyOneHeight(h, *opts.live_chain_db, *opts.live_block_storage, entry);
        switch (entry.status) {
            case UndoRebuildStatus::AlreadyOk:       manifest.already_ok_count++; break;
            case UndoRebuildStatus::Hole:            manifest.holes_count++; break;
            case UndoRebuildStatus::MissingMetadata: manifest.missing_metadata_count++; any_missing_metadata = true; break;
            case UndoRebuildStatus::Blocked:         manifest.blocked_count++; any_blocked = true; break;
            // Other statuses cannot appear during preflight.
            default: break;
        }
        manifest.entries.push_back(std::move(entry));
    }

    g_logger.info("[undo-rebuild] preflight: already_ok=" +
                  std::to_string(manifest.already_ok_count) +
                  " holes=" + std::to_string(manifest.holes_count) +
                  " missing_metadata=" + std::to_string(manifest.missing_metadata_count) +
                  " blocked=" + std::to_string(manifest.blocked_count));

    auto manifest_path = opts.manifest_path_override.empty()
        ? (opts.datadir / "rebuild_undo_manifest.json")
        : opts.manifest_path_override;

    auto write_manifest = [&]() -> Status {
        std::error_code ec;
        std::filesystem::create_directories(manifest_path.parent_path(), ec);
        std::ofstream out(manifest_path, std::ios::trunc);
        if (!out.is_open()) {
            g_logger.error("[undo-rebuild] failed to open manifest for write: " +
                           manifest_path.string());
            return Status::Io;
        }
        out << manifest.ToJson();
        if (!out.good()) {
            g_logger.error("[undo-rebuild] failed to write manifest: " +
                           manifest_path.string());
            return Status::Io;
        }
        return Status::Ok;
    };

    // Refusal paths: any blocked or missing_metadata aborts. Manifest is
    // still emitted so the operator can inspect the offending heights.
    if (any_blocked || any_missing_metadata) {
        manifest.final_status = "preflight_refused";
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] preflight refused: blocked=" +
                       std::to_string(manifest.blocked_count) +
                       " missing_metadata=" +
                       std::to_string(manifest.missing_metadata_count));
        return Status::Invalid;
    }

    // Dry run: stop after preflight. Holes remain classified as Hole;
    // the orchestrator's caller can read the manifest to plan the
    // real run.
    if (opts.dry_run) {
        manifest.final_status = "dry_run_complete";
        manifest.emitted_at_unix = NowUnix();
        if (const auto s = write_manifest(); s != Status::Ok) return s;
        g_logger.info("[undo-rebuild] dry run complete; manifest at " +
                      manifest_path.string());
        return manifest;
    }

    // ─────────────────────────────────────────────────────────────────
    // Phase 2-3: temp ChainDB lifecycle + windowed reindexer invocation
    // ─────────────────────────────────────────────────────────────────
    // The temp ChainDB and temp BlockStorage live at sibling paths
    // under datadir. They are throwaway: the temp DB exists only to
    // hold UTXO/forest/shielded state at each processed height so the
    // reindexer's prevout lookups resolve. The temp BlockStorage's
    // rev*.dat is similarly throwaway — the LIVE rev*.dat is written
    // separately via the reindexer's WINDOWED_UNDO_ONLY Step 5b path
    // (commit #2) only after Step 5a's verification passes (commit #3).
    //
    // For genesis-replay anchors (anchor.height == 0), the temp DB is
    // freshly created and the reindexer walks every block from genesis
    // to window.end_height. For non-genesis anchors the orchestrator
    // requires the caller to have pre-populated the temp DB up to
    // anchor.height (e.g. via assumeUTXO snapshot load) — a follow-up
    // commit will add an automatic anchor-population pass on top of
    // this scaffolding.
    const auto temp_chainstate_path =
        opts.datadir / "chainstate.rebuild-undo.tmp";
    const auto temp_block_dir =
        opts.datadir / ".rebuild-undo.tmp";

    auto cleanup_temp = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(temp_chainstate_path, ec);
        ec.clear();
        std::filesystem::remove_all(temp_block_dir, ec);
    };
    // Always clean up on exit, regardless of how we leave this function.
    struct CleanupGuard {
        std::function<void()> fn;
        ~CleanupGuard() { if (fn) fn(); }
    } cleanup_guard{cleanup_temp};

    // Pre-clean any stale temp dirs from a prior interrupted run.
    cleanup_temp();
    {
        std::error_code ec;
        std::filesystem::create_directories(temp_chainstate_path, ec);
        std::filesystem::create_directories(temp_block_dir, ec);
    }

    auto temp_chain_db = std::make_unique<ChainDB>();
    if (auto s = temp_chain_db->init(temp_chainstate_path); s != Status::Ok) {
        manifest.final_status = "temp_chaindb_init_failed";
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] failed to init temp ChainDB at " +
                       temp_chainstate_path.string());
        return s;
    }
    auto temp_block_storage = std::make_unique<BlockStorage>();
    if (auto s = temp_block_storage->init(temp_block_dir); s != Status::Ok) {
        manifest.final_status = "temp_block_storage_init_failed";
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] failed to init temp BlockStorage at " +
                       temp_block_dir.string());
        return s;
    }

    // Configure the reindexer for WINDOWED_UNDO_ONLY mode. The reindexer's
    // datadir_ points at LIVE so scanBlockFiles finds the real blk*.dat;
    // chain_db_ and block_storage_ point at the temp scratchpads.
    consensus::BlockReindexer::Config rcfg;
    rcfg.mode = consensus::BlockReindexer::Mode::WINDOWED_UNDO_ONLY;
    rcfg.use_assumevalid = true;
    rcfg.progress_interval = 500;
    rcfg.shielded_frontier_output_path =
        temp_block_dir / "shielded_frontier.bin";
    rcfg.shielded_nullifier_db_path =
        temp_block_dir / "shielded_nullifiers.db";
    rcfg.anchor_state = opts.anchor;
    // Anchor canonical-chain selection at the LIVE tip to avoid the
    // chainwork-search getting derailed by stale orphan blocks left
    // on disk by prior chain incarnations (LA Apr 30 2026 condition:
    // ~52,000 magic-aligned regions for a 10,784-block canonical chain).
    // The orchestrator already knows the canonical tip — preflight
    // queries it. Pass it through so SelectCanonicalChain walks
    // backward from a known anchor instead of brute-force searching.
    {
        auto live_tip_hash = opts.live_chain_db->getBlockHashByHeight(
            static_cast<int>(opts.window_end));
        if (live_tip_hash.ok()) {
            rcfg.known_canonical_tip_hash = live_tip_hash.value();
        }
    }
    consensus::BlockReindexer::UndoRebuildWindow rwin;
    rwin.start_height = opts.window_start;
    rwin.end_height = opts.window_end;
    rwin.live_chain_db = opts.live_chain_db;
    rwin.live_block_storage = opts.live_block_storage;
    rwin.verify_disconnect_roundtrip = true;
    // Hole-only optimization (commit #8): tell the reindexer which
    // heights actually need LIVE writes. Already_ok heights stay
    // byte-untouched on LIVE — no rev*.dat append, no metadata Put.
    // The verifier still runs on every in-window block so chain-wide
    // DisconnectBlock parity is exercised.
    for (const auto& entry : manifest.entries) {
        if (entry.status == UndoRebuildStatus::Hole) {
            rwin.hole_heights_to_rebuild.insert(entry.height);
        }
    }
    rcfg.undo_rebuild_window = rwin;
    // Genesis-replay: temp DB is fresh, so DON'T preserve shielded state
    // on init — let initializeShieldedArtifacts wipe and reopen. For
    // non-genesis anchors, caller must have pre-populated the shielded
    // DBs at the configured paths and we preserve them.
    rcfg.preserve_shielded_state_on_init = (opts.anchor.height >= 1);

    consensus::BlockReindexer reindexer(opts.datadir, temp_chain_db.get(),
                                        temp_block_storage.get(), rcfg);
    g_logger.info("[undo-rebuild] starting windowed reindex pass");
    auto reindex_result = reindexer.execute();
    if (!reindex_result.ok()) {
        manifest.final_status = "reindex_failed";
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] reindexer.execute() failed");
        return reindex_result.status();
    }
    const auto& rstats = reindex_result.value();
    if (!rstats.error.empty()) {
        manifest.final_status = "reindex_error: " + rstats.error;
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] reindexer reported error: " + rstats.error);
        return Status::Invalid;
    }

    // Reconcile per-height status against reindexer outcomes.
    //
    // Ground-truth sources from the reindexer (in priority order):
    //   1. live_undo_write_success_heights — heights for which a LIVE
    //      writeUndo + LIVE putHeaderMetadata both committed durably.
    //      Authoritative for "Rebuilt" status; orchestrator marks NO
    //      height Rebuilt unless it appears in this set.
    //   2. verify_failure_heights — heights whose DisconnectBlock-
    //      roundtrip verifier rejected the candidate undo bytes.
    //      Authoritative for "VerifyFailed" status.
    //   3. Anything else — neither verified-and-failed nor LIVE-written.
    //      For preflight Holes, this means the reindexer never
    //      reached / never wrote the height (e.g. canonical chain
    //      construction failed, run aborted mid-window). Mark these
    //      Skipped — DO NOT silently mark them Rebuilt.
    //
    // The Apr 30 2026 LA bug surfaced this gap: orchestrator was
    // marking 518 heights Rebuilt based on absence-of-verify-failure
    // alone, while the reindexer's actual live_undo_writes_committed
    // was 0 (canonical-chain selection had silently failed). Fix A:
    // use live_undo_write_success_heights as the single truth source.
    std::unordered_set<uint32_t> verify_failed_set(
        rstats.verify_failure_heights.begin(),
        rstats.verify_failure_heights.end());
    std::unordered_set<uint32_t> live_write_set(
        rstats.live_undo_write_success_heights.begin(),
        rstats.live_undo_write_success_heights.end());
    uint64_t skipped_count_local = 0;
    for (auto& entry : manifest.entries) {
        const bool was_hole = entry.status == UndoRebuildStatus::Hole;
        const bool was_already_ok = entry.status == UndoRebuildStatus::AlreadyOk;
        if (!was_hole && !was_already_ok) continue;

        if (verify_failed_set.count(entry.height)) {
            entry.status = UndoRebuildStatus::VerifyFailed;
            entry.reason = was_hole
                ? "DisconnectBlock-roundtrip verification failed on hole; "
                  "live undo bytes left untouched"
                : "DisconnectBlock-roundtrip verification failed on previously-"
                  "already_ok height — live state DIVERGES from fresh replay; "
                  "investigate before trusting this height's existing undo";
            manifest.verify_failed_count++;
            if (was_hole) manifest.holes_count--;
            else          manifest.already_ok_count--;
            continue;
        }

        if (was_hole) {
            // Hole heights MUST appear in live_write_set to be Rebuilt;
            // anything else is Skipped (reindexer didn't reach / didn't
            // write this height for some reason — surface it).
            if (live_write_set.count(entry.height)) {
                entry.status = UndoRebuildStatus::Rebuilt;
                entry.reason.clear();
                manifest.rebuilt_count++;
                manifest.holes_count--;
            } else {
                entry.status = UndoRebuildStatus::Skipped;
                entry.reason = "reindexer did not commit a LIVE undo write for "
                               "this height (canonical-chain construction may "
                               "have failed; investigate run logs)";
                manifest.skipped_count++;
                manifest.holes_count--;
                skipped_count_local++;
            }
            continue;
        }

        // was_already_ok + verified clean → leave entry untouched.
        // (already_ok heights deliberately skip LIVE writes per the
        // hole-only optimization, so absence from live_write_set is
        // expected and correct.)
    }

    // Hard cross-check — these must agree by construction now.
    if (manifest.rebuilt_count != rstats.live_undo_writes_committed) {
        manifest.final_status =
            "live_writes_count_mismatch_manifest=" +
            std::to_string(manifest.rebuilt_count) +
            "_reindexer=" +
            std::to_string(rstats.live_undo_writes_committed);
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] hard cross-check failed: " +
                       manifest.final_status);
        return Status::Internal;
    }

    // Hard rule: any preflight Hole that didn't get rebuilt → not "ok".
    // The operator deserves a clear signal that the run was incomplete.
    if (skipped_count_local > 0) {
        manifest.final_status =
            "incomplete_skipped=" + std::to_string(skipped_count_local) +
            "_rebuilt=" + std::to_string(manifest.rebuilt_count);
        manifest.emitted_at_unix = NowUnix();
        (void)write_manifest();
        g_logger.error("[undo-rebuild] " + std::to_string(skipped_count_local) +
                       " preflight Hole heights were not rebuilt; "
                       "manifest reports them as Skipped. " +
                       "Run is INCOMPLETE; investigate before retry.");
        return Status::Invalid;
    }

    manifest.final_status =
        manifest.verify_failed_count > 0 ? "ok_with_verify_failures" : "ok";
    manifest.emitted_at_unix = NowUnix();
    if (const auto s = write_manifest(); s != Status::Ok) {
        return s;
    }
    g_logger.info("[undo-rebuild] complete: rebuilt=" +
                  std::to_string(manifest.rebuilt_count) +
                  " verify_failed=" +
                  std::to_string(manifest.verify_failed_count) +
                  " already_ok=" +
                  std::to_string(manifest.already_ok_count) +
                  " manifest=" + manifest_path.string());
    return manifest;
}

}  // namespace daemon
}  // namespace dinero
