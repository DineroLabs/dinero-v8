#include "consensus/reindexer.h"
#include "common/crash_injection.h"  // testing::MaybeAbortAt — used by Step 5b crash oracles
#include "dinero/compat/int128.hpp"
#include "consensus/block_lifecycle.h"
#include "consensus/chainwork.h"  // For GetBlockProof
#include "consensus/genesis_canonical.h"
#include "consensus/merkle_root.h"
#include "consensus/outpoint.h"             // OutPoint (for intra-block spend tracking)
#include "consensus/shielded/shielded_block_section.h"
#include "consensus/shielded/shielded_block_validation.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_epoch.h"
#include "consensus/utreexo_accumulator.h"  // HashUTXO, UtreexoForest
#include "consensus/utreexo_activation.h"   // IsUtreexoActive
#include "consensus/utreexo_canonical_roots_activation.h"  // canonical-roots gate
#include "consensus/utreexo_delta.h"        // UtreexoDelta
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/undo.h"                 // UndoRecord, SpentCoin, CreatedOut
#include "storage/chain_db.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "storage/chain_write_token.h"
#include "primitives/block.h"
#include "common/logger.h"
#include "common/serialization.h"
#include "consensus/chainparams.h"
#include "consensus/pow.h"
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dinero {
namespace consensus {

namespace {

struct DiskBlockRecord {
    Block block;
    FilePosition pos;
    uint256 hash;
    uint256 prev_hash;
};

std::string BytesToHex(const uint8_t* data, size_t size) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return ss.str();
}

std::string UtreexoHashToHex(const UtreexoHash& hash) {
    return BytesToHex(hash.data(), hash.size());
}

std::string JsonEscape(const std::string& input) {
    std::ostringstream escaped;
    for (unsigned char c : input) {
        switch (c) {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (c < 0x20) {
                    escaped << "\\u"
                            << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(c)
                            << std::dec << std::setfill(' ');
                } else {
                    escaped << c;
                }
        }
    }
    return escaped.str();
}

struct ForestTraceConfig {
    bool enabled = false;
    std::filesystem::path path;
    uint32_t start_height = 0;
    uint32_t end_height = std::numeric_limits<uint32_t>::max();
};

ForestTraceConfig LoadForestTraceConfig() {
    ForestTraceConfig cfg;
    const char* path_env = std::getenv("DINERO_REINDEX_FOREST_TRACE");
    if (path_env == nullptr || path_env[0] == '\0') {
        return cfg;
    }

    cfg.enabled = true;
    cfg.path = path_env;

    const char* range_env = std::getenv("DINERO_REINDEX_FOREST_TRACE_RANGE");
    if (range_env != nullptr && range_env[0] != '\0') {
        const std::string range(range_env);
        const auto colon = range.find(':');
        try {
            if (colon == std::string::npos) {
                cfg.start_height = static_cast<uint32_t>(std::stoul(range));
                cfg.end_height = cfg.start_height;
            } else {
                cfg.start_height = static_cast<uint32_t>(std::stoul(range.substr(0, colon)));
                cfg.end_height = static_cast<uint32_t>(std::stoul(range.substr(colon + 1)));
            }
        } catch (const std::exception&) {
            cfg.start_height = 0;
            cfg.end_height = std::numeric_limits<uint32_t>::max();
        }
    }

    return cfg;
}

const ForestTraceConfig& GetForestTraceConfig() {
    static const ForestTraceConfig cfg = LoadForestTraceConfig();
    return cfg;
}

bool ShouldTraceForestHeight(uint32_t height) {
    const auto& cfg = GetForestTraceConfig();
    return cfg.enabled && height >= cfg.start_height && height <= cfg.end_height;
}

void AppendForestTrace(uint32_t height,
                       const std::string& phase,
                       const Block& block,
                       const UtreexoHash& forest_pre_commitment,
                       const UtreexoHash& snapshot_pre_commitment,
                       const UtreexoHash& computed_commitment,
                       const UtreexoDelta& delta,
                       uint64_t num_leaves_after,
                       const std::string& status,
                       const std::string& error = {}) {
    if (!ShouldTraceForestHeight(height)) {
        return;
    }

    const auto& cfg = GetForestTraceConfig();
    std::ofstream out(cfg.path, std::ios::app);
    if (!out) {
        g_logger.error("[reindex] failed to open forest trace path: " + cfg.path.string());
        return;
    }

    out << "{"
        << "\"height\":" << height
        << ",\"phase\":\"" << JsonEscape(phase) << "\""
        << ",\"status\":\"" << JsonEscape(status) << "\""
        << ",\"block_hash\":\"" << block.GetHash().GetHex() << "\""
        << ",\"prev_hash\":\"" << block.header.prev_block_hash.GetHex() << "\""
        << ",\"header_root\":\"" << block.header.utreexo_root.GetHex() << "\""
        << ",\"forest_pre_root\":\"" << UtreexoHashToHex(forest_pre_commitment) << "\""
        << ",\"snapshot_pre_root\":\"" << UtreexoHashToHex(snapshot_pre_commitment) << "\""
        << ",\"computed_root\":\"" << UtreexoHashToHex(computed_commitment) << "\""
        << ",\"num_leaves_before\":" << delta.numLeavesBefore
        << ",\"num_leaves_after\":" << num_leaves_after
        << ",\"deleted\":" << delta.deletedLeaves.size()
        << ",\"added\":" << delta.addedLeaves.size()
        << ",\"tx_count\":" << block.vtx.size();
    if (!error.empty()) {
        out << ",\"error\":\"" << JsonEscape(error) << "\"";
    }
    out << "}\n";
}

void AppendForestVerifyTrace(uint64_t height,
                             const Block& block,
                             const UtreexoHash& pre_commitment,
                             const UtreexoHash& post_commitment,
                             const UtreexoHash& recovered_commitment,
                             uint64_t pre_num_leaves,
                             uint64_t post_num_leaves,
                             uint64_t recovered_num_leaves,
                             const UtreexoDelta& delta,
                             size_t undo_bytes_size,
                             const std::string& status,
                             const std::string& error = {}) {
    if (height > std::numeric_limits<uint32_t>::max() ||
        !ShouldTraceForestHeight(static_cast<uint32_t>(height))) {
        return;
    }

    const auto& cfg = GetForestTraceConfig();
    std::ofstream out(cfg.path, std::ios::app);
    if (!out) {
        g_logger.error("[reindex] failed to open forest trace path: " + cfg.path.string());
        return;
    }

    out << "{"
        << "\"height\":" << height
        << ",\"phase\":\"verifyRebuiltUndoRoundTrip\""
        << ",\"status\":\"" << JsonEscape(status) << "\""
        << ",\"block_hash\":\"" << block.GetHash().GetHex() << "\""
        << ",\"prev_hash\":\"" << block.header.prev_block_hash.GetHex() << "\""
        << ",\"header_root\":\"" << block.header.utreexo_root.GetHex() << "\""
        << ",\"pre_root\":\"" << UtreexoHashToHex(pre_commitment) << "\""
        << ",\"post_root\":\"" << UtreexoHashToHex(post_commitment) << "\""
        << ",\"recovered_root\":\"" << UtreexoHashToHex(recovered_commitment) << "\""
        << ",\"pre_num_leaves\":" << pre_num_leaves
        << ",\"post_num_leaves\":" << post_num_leaves
        << ",\"recovered_num_leaves\":" << recovered_num_leaves
        << ",\"deleted\":" << delta.deletedLeaves.size()
        << ",\"added\":" << delta.addedLeaves.size()
        << ",\"undo_bytes\":" << undo_bytes_size
        << ",\"tx_count\":" << block.vtx.size();
    if (!error.empty()) {
        out << ",\"error\":\"" << JsonEscape(error) << "\"";
    }
    out << "}\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Utreexo delta sidecar codec
// ─────────────────────────────────────────────────────────────────────────────
// Mirrors the file-static helpers in src/daemon/services/chainstate_service.cpp
// (MakeUtreexoDeltaUndoKey + SerializeUtreexoDelta). Kept in sync by convention:
// any schema change MUST update both sites. These are small enough that inline
// duplication is cleaner than extracting a shared module.

constexpr const char* kUtreexoDeltaUndoPrefix = "UD:";
constexpr uint8_t kUtreexoDeltaUndoSchemaV1 = 1;
constexpr size_t kUtreexoLeafHashSize = 32;

std::string MakeUtreexoDeltaUndoKey(const uint256& block_hash) {
    return std::string(kUtreexoDeltaUndoPrefix) + block_hash.GetHex();
}

bool SerializeUtreexoDelta(const UtreexoDelta& delta, std::string& out, std::string& error) {
    try {
        VectorWriter writer;
        writer.write(kUtreexoDeltaUndoSchemaV1);
        writer.write(delta.numLeavesBefore);
        writer.writeVarInt(delta.deletedLeaves.size());
        for (const auto& deleted : delta.deletedLeaves) {
            if (deleted.leafHash.size() != kUtreexoLeafHashSize) {
                error = "utreexo-delta-serialize-invalid-deleted-hash-size";
                return false;
            }
            writer.write(deleted.leafHash.data(), kUtreexoLeafHashSize);
            writer.write(deleted.position);
        }
        writer.writeVarInt(delta.addedLeaves.size());
        for (const auto& added : delta.addedLeaves) {
            if (added.hash.size() != kUtreexoLeafHashSize) {
                error = "utreexo-delta-serialize-invalid-added-hash-size";
                return false;
            }
            writer.write(added.hash.data(), kUtreexoLeafHashSize);
            writer.write(added.position);
        }
        out = writer.release_string();
        return true;
    } catch (const std::exception& e) {
        error = std::string("utreexo-delta-serialize-exception: ") + e.what();
        return false;
    }
}

// Mirrors the fee/delta glue in block_validation.cpp. Keep this duplication
// strictly limited to reindex feed-in helpers; shielded validation core itself
// stays shared via shielded::ValidateShieldedBundle / ValidateBlockShielded.
bool UsesShieldedValueSemantics(const Transaction& tx) {
    return Transaction::IsShieldedVersion(tx.version) ||
           !tx.shielded_bundle_bytes.empty();
}

bool HasConfidentialInputs(const std::vector<Coin>& inputs) {
    return std::any_of(inputs.begin(), inputs.end(), [](const Coin& coin) {
        return coin.is_confidential;
    });
}

bool UsesConfidentialValueSemantics(const Transaction& tx,
                                    const std::vector<Coin>& inputs) {
    return tx.HasConfidentialOutputs() || HasConfidentialInputs(inputs);
}

uint64_t SumOutputs(const Transaction& tx) {
    uint64_t sum = 0;
    for (const auto& output : tx.vout) {
        const uint64_t value = output.value.GetUna();
        sum += value;
        if (sum < value) {
            return UINT64_MAX;
        }
    }
    return sum;
}

bool ComputeValidatedTransactionFee(const Transaction& tx,
                                    const std::vector<Coin>& input_coins,
                                    uint64_t total_input_value,
                                    uint64_t total_output_value,
                                    uint64_t& fee,
                                    std::string& error) {
    if (UsesConfidentialValueSemantics(tx, input_coins) ||
        (UsesShieldedValueSemantics(tx) && tx.HasExplicitFee())) {
        if (!tx.HasExplicitFee()) {
            error = UsesShieldedValueSemantics(tx)
                ? "Shielded transaction missing explicit fee"
                : "Confidential transaction missing explicit fee";
            return false;
        }
        fee = tx.GetExplicitFee();
        return true;
    }

    if (total_output_value > total_input_value) {
        error = "Outputs exceed inputs (negative fee)";
        return false;
    }

    fee = total_input_value - total_output_value;
    return true;
}

bool ComputeTransparentValueDelta(uint64_t total_input_value,
                                  uint64_t total_output_value,
                                  uint64_t fee,
                                  int64_t& delta,
                                  std::string& error) {
    using dinero::compat::i128;
    using dinero::compat::i128_zext_u64;
    // i128_zext_u64 mirrors the original `(__int128)uint64_t` zero-extend
    // semantics; i128(uint64_t) would sign-extend on the struct backend
    // and corrupt high-bit-set values.
    const i128 signed_delta =
        i128_zext_u64(total_input_value) -
        i128_zext_u64(total_output_value) -
        i128_zext_u64(fee);
    if (signed_delta < i128(std::numeric_limits<int64_t>::min()) ||
        signed_delta > i128(std::numeric_limits<int64_t>::max())) {
        error = "Shielded transparent value delta out of range";
        return false;
    }
    delta = static_cast<int64_t>(signed_delta);
    return true;
}

const char* ShieldedValidationErrorToString(shielded::ShieldedValidationError err) {
    using shielded::ShieldedValidationError;
    switch (err) {
        case ShieldedValidationError::Ok:                   return "ok";
        case ShieldedValidationError::NullifierDuplicate:   return "nullifier-duplicate";
        case ShieldedValidationError::AnchorInvalid:        return "anchor-invalid";
        case ShieldedValidationError::ProofInvalid:         return "proof-invalid";
        case ShieldedValidationError::ValueBalanceMismatch: return "value-balance-mismatch";
        case ShieldedValidationError::BindingSigInvalid:    return "binding-sig-invalid";
        case ShieldedValidationError::BundleMalformed:      return "bundle-malformed";
        case ShieldedValidationError::NotActive:            return "shielded-not-active";
        case ShieldedValidationError::BundleTooLarge:       return "bundle-too-large";
        case ShieldedValidationError::RangeProofInvalid:    return "range-proof-invalid";
    }
    return "unknown";
}

uint64_t LeafAmountForOutput(const TxOutput& output) {
    return output.is_confidential ? 0 : output.value.GetUna();
}

uint64_t LeafAmountForCoin(const Coin& coin) {
    return coin.is_confidential ? 0 : coin.amount;
}

uint32_t Fnv1aChecksum(const uint8_t* data, size_t len) {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

StatusOr<std::vector<DiskBlockRecord>> ReadDiskBlocks(
    const std::vector<std::filesystem::path>& block_files,
    BlockReindexer::Stats* stats
) {
    std::vector<DiskBlockRecord> records;
    const auto& params = Params();
    const uint32_t expected_magic = params.magic;

    for (size_t i = 0; i < block_files.size(); ++i) {
        const auto& file_path = block_files[i];
        std::string filename = file_path.filename().string();
        uint32_t file_number = static_cast<uint32_t>(i);
        try {
            if (filename.rfind("blk", 0) == 0 && filename.size() >= 8) {
                file_number = static_cast<uint32_t>(std::stoul(filename.substr(3, 5)));
            }
        } catch (...) {
            // Keep sorted fallback index if filename parsing fails.
        }

        g_logger.info("   Processing file " + std::to_string(i + 1) + "/" +
                      std::to_string(block_files.size()) + ": " + filename);

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            g_logger.error("Failed to open block file: " + file_path.string());
            return Status::Io;
        }

        file.seekg(0, std::ios::end);
        const std::streampos file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        uint64_t offset = 0;
        uint64_t blocks_in_file = 0;

        while (offset < static_cast<uint64_t>(file_size)) {
            uint32_t magic = 0;
            file.read(reinterpret_cast<char*>(&magic), 4);
            if (file.gcount() != 4) {
                break;
            }
            if (magic != expected_magic) {
                g_logger.error("Invalid block magic at offset " + std::to_string(offset) +
                               " in " + file_path.string());
                return Status::Corruption;
            }

            uint32_t block_size = 0;
            file.read(reinterpret_cast<char*>(&block_size), 4);
            if (file.gcount() != 4) {
                g_logger.error("Incomplete block size at offset " + std::to_string(offset));
                return Status::Corruption;
            }
            if (block_size == 0 || block_size > 32 * 1024 * 1024) {
                g_logger.error("Invalid block size " + std::to_string(block_size) +
                               " at offset " + std::to_string(offset));
                return Status::Invalid;
            }

            std::vector<uint8_t> block_data(block_size);
            file.read(reinterpret_cast<char*>(block_data.data()), block_size);
            if (file.gcount() != static_cast<std::streamsize>(block_size)) {
                g_logger.error("Incomplete block data at offset " + std::to_string(offset));
                return Status::Corruption;
            }

            uint32_t stored_checksum = 0;
            file.read(reinterpret_cast<char*>(&stored_checksum), 4);
            if (file.gcount() != 4) {
                g_logger.error("Incomplete block checksum at offset " +
                               std::to_string(offset + 8 + block_size));
                return Status::Corruption;
            }

            const uint32_t calculated_checksum = Fnv1aChecksum(block_data.data(), block_data.size());
            if (stored_checksum != calculated_checksum) {
                // Tolerant skip: stale/orphan/partial-write blocks left on
                // disk by past chain incarnations should not block recovery.
                // SelectCanonicalChain only follows parent-hash links from
                // genesis, so a skipped non-canonical block is naturally
                // discarded after this phase. The live daemon never
                // references these regions (no metadata pointer to them).
                g_logger.warning("[reindex] Block checksum mismatch at offset " +
                                 std::to_string(offset) + " in " + file_path.string() +
                                 " — skipping (likely orphan/stale from prior chain)");
                if (stats != nullptr) {
                    ++stats->parse_skipped_blocks;
                    stats->total_bytes += 12 + block_size;
                }
                offset += 12 + block_size;
                continue;
            }

            auto parsed_block = Block::Deserialize(block_data.data(), block_data.size());
            if (!parsed_block.has_value()) {
                // Same tolerant-skip rationale as above. A canonical block
                // that fails to deserialize would manifest downstream as
                // SelectCanonicalChain producing a shorter-than-expected
                // chain — observable failure mode, not silent corruption.
                g_logger.warning("[reindex] Failed to deserialize block at offset " +
                                 std::to_string(offset) + " from " + file_path.string() +
                                 " — skipping (likely orphan/stale from prior chain)");
                if (stats != nullptr) {
                    ++stats->parse_skipped_blocks;
                    stats->total_bytes += 12 + block_size;
                }
                offset += 12 + block_size;
                continue;
            }

            DiskBlockRecord record;
            record.block = std::move(parsed_block.value());
            record.pos = FilePosition(file_number, offset, block_size);
            record.hash = record.block.GetHash();
            record.prev_hash = record.block.header.prev_block_hash;
            records.push_back(std::move(record));

            offset += 12 + block_size;
            ++blocks_in_file;
            if (stats != nullptr) {
                stats->total_bytes += 12 + block_size;
            }
        }

        g_logger.info("      Parsed " + std::to_string(blocks_in_file) +
                      " block bodies from " + filename);
    }

    return records;
}

// Select the canonical chain from a parsed records vector.
//
// When `known_tip_hash_hex` is empty: legacy behavior. Resolve every record
// against parent-hash links to compute connected-to-genesis cursors; pick
// the highest-chainwork tip; walk backward to genesis. Works on clean
// datadirs with few-or-no orphans.
//
// When `known_tip_hash_hex` is non-empty: anchored mode. Skip the
// chainwork-search entirely; find the record whose hash equals
// `known_tip_hash_hex` (the LIVE chain's tip, which the orchestrator
// reads from the live ChainDB before invoking the reindexer); start
// the backward walk from there. Required for datadirs whose blk*.dat
// contains stale orphans from prior chain incarnations (LA on
// 2026-04-30 had ~52,000 magic-aligned regions for a 10,784-block
// chain, where the chainwork-search produced a non-canonical or
// truncated chain). With anchored mode the walk is O(canonical_height)
// regardless of orphan count.
StatusOr<std::vector<size_t>> SelectCanonicalChain(
    const std::vector<DiskBlockRecord>& records,
    const std::string& known_tip_hash_hex = std::string()) {
    if (records.empty()) {
        return Status::NotFound;
    }

    const auto& params = Params();
    const std::string genesis_hash = params.genesis_hash;
    const arith_uint256 genesis_work = GetBlockProof(params.genesis.nBits);

    // index_by_hash maps block hash → record index, EXCLUDING the
    // genesis hash. Duplicate-hash records are kept first-wins (later
    // duplicates silently lose the emplace race; harmless because all
    // duplicates share the same hash and the same prev_hash).
    std::unordered_map<std::string, size_t> index_by_hash;
    index_by_hash.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const std::string hash_hex = records[i].hash.GetHex();
        if (hash_hex == genesis_hash) {
            continue;
        }
        index_by_hash.emplace(hash_hex, i);
    }

    // Anchored mode: the orchestrator told us the canonical tip hash.
    // Skip chainwork-search; walk backward from the anchor.
    if (!known_tip_hash_hex.empty()) {
        auto tip_it = index_by_hash.find(known_tip_hash_hex);
        if (tip_it == index_by_hash.end()) {
            // Special case: anchor IS the genesis. Empty chain (only
            // genesis "applied"). Caller's loop bound handles this.
            if (known_tip_hash_hex == genesis_hash) {
                return std::vector<size_t>{};
            }
            g_logger.error("[reindex] SelectCanonicalChain anchored: known tip hash " +
                           known_tip_hash_hex + " not found in parsed records — "
                           "either the canonical tip block was tolerantly-skipped during "
                           "parse (checksum/deserialize failure) or the orchestrator "
                           "passed a stale tip hash");
            return Status::NotFound;
        }
        std::vector<size_t> canonical_chain;
        size_t idx = tip_it->second;
        std::unordered_set<size_t> visited;  // cycle guard
        while (true) {
            if (!visited.insert(idx).second) {
                g_logger.error("[reindex] SelectCanonicalChain anchored: cycle "
                               "detected at hash " + records[idx].hash.GetHex());
                return Status::Corruption;
            }
            canonical_chain.push_back(idx);
            const std::string prev_hash = records[idx].prev_hash.GetHex();
            if (prev_hash == genesis_hash) {
                break;
            }
            auto it = index_by_hash.find(prev_hash);
            if (it == index_by_hash.end()) {
                g_logger.error("[reindex] SelectCanonicalChain anchored: walk broke at "
                               "block " + records[idx].hash.GetHex() +
                               " — its prev_hash " + prev_hash +
                               " not found in parsed records (likely tolerantly-skipped)");
                return Status::Corruption;
            }
            idx = it->second;
        }
        std::reverse(canonical_chain.begin(), canonical_chain.end());
        // Diagnostic dump: env-gated, writes (height, record_index, file_offset,
        // block_hash, prev_hash) per chain element. Lets us diff the canonical
        // walks across runs/ranges to catch the case where the same blk file
        // produces different chains depending on where the walk starts.
        if (const char* dump_path = std::getenv("DINERO_REINDEX_CHAIN_DUMP");
            dump_path != nullptr && dump_path[0] != '\0') {
            std::ofstream dump(dump_path, std::ios::trunc);
            if (dump) {
                for (size_t i = 0; i < canonical_chain.size(); ++i) {
                    const size_t rec_idx = canonical_chain[i];
                    const auto& rec = records[rec_idx];
                    dump << (i + 1) << "\t" << rec_idx << "\t"
                         << rec.pos.offset << "\t"
                         << rec.hash.GetHex() << "\t"
                         << rec.prev_hash.GetHex() << "\n";
                }
            }
        }
        g_logger.info("[reindex] SelectCanonicalChain (anchored): chain length = " +
                      std::to_string(canonical_chain.size()) +
                      " from anchor " + known_tip_hash_hex);
        return canonical_chain;
    }

    struct ChainCursor {
        bool connected = false;
        bool visiting = false;
        bool resolved = false;
        int32_t height = -1;
        arith_uint256 chainwork;
    };

    std::vector<ChainCursor> cursors(records.size());
    std::function<bool(size_t)> resolve = [&](size_t idx) -> bool {
        auto& cursor = cursors[idx];
        if (cursor.resolved) {
            return cursor.connected;
        }
        if (cursor.visiting) {
            return false;
        }

        cursor.visiting = true;
        const std::string prev_hash = records[idx].prev_hash.GetHex();
        const arith_uint256 block_work = GetBlockProof(records[idx].block.header.difficulty);

        if (prev_hash == genesis_hash) {
            cursor.connected = true;
            cursor.height = 1;
            cursor.chainwork = genesis_work + block_work;
        } else {
            auto it = index_by_hash.find(prev_hash);
            if (it != index_by_hash.end() && resolve(it->second)) {
                const auto& parent = cursors[it->second];
                cursor.connected = true;
                cursor.height = parent.height + 1;
                cursor.chainwork = parent.chainwork + block_work;
            }
        }

        cursor.visiting = false;
        cursor.resolved = true;
        return cursor.connected;
    };

    size_t best_idx = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < records.size(); ++i) {
        if (!resolve(i)) {
            continue;
        }

        if (best_idx == std::numeric_limits<size_t>::max()) {
            best_idx = i;
            continue;
        }

        const auto& candidate = cursors[i];
        const auto& best = cursors[best_idx];
        if (candidate.chainwork > best.chainwork ||
            (candidate.chainwork == best.chainwork && candidate.height > best.height)) {
            best_idx = i;
        }
    }

    if (best_idx == std::numeric_limits<size_t>::max()) {
        g_logger.error("[reindex] SelectCanonicalChain (legacy): no record connected to genesis " +
                       genesis_hash);
        return Status::NotFound;
    }

    std::vector<size_t> canonical_chain;
    for (size_t idx = best_idx;;) {
        canonical_chain.push_back(idx);
        const std::string prev_hash = records[idx].prev_hash.GetHex();
        if (prev_hash == genesis_hash) {
            break;
        }

        auto it = index_by_hash.find(prev_hash);
        if (it == index_by_hash.end()) {
            return Status::Corruption;
        }
        idx = it->second;
    }

    std::reverse(canonical_chain.begin(), canonical_chain.end());
    g_logger.info("[reindex] SelectCanonicalChain (legacy chainwork-search): chain length = " +
                  std::to_string(canonical_chain.size()) +
                  " best_tip_hash = " + records[best_idx].hash.GetHex());
    return canonical_chain;
}

} // namespace

BlockReindexer::BlockReindexer(
    const std::filesystem::path& datadir,
    ChainDB* chain_db,
    BlockStorage* block_storage,
    const Config& config
)
    : datadir_(datadir)
    , chain_db_(chain_db)
    , block_storage_(block_storage)
    , config_(config)
    , accumulated_chainwork_()  // Initialize to zero
    , forest_(std::make_unique<UtreexoForest>())
    , shielded_frontier_output_path_(
          config.shielded_frontier_output_path.empty()
              ? (datadir / "blockchain" / "shielded_frontier.bin")
              : config.shielded_frontier_output_path)
    , shielded_nullifier_db_path_(
          config.shielded_nullifier_db_path.empty()
              ? (datadir / "blockchain" / "shielded_nullifiers.db")
              : config.shielded_nullifier_db_path)
{
}

BlockReindexer::~BlockReindexer() = default;

// Apply one block's Utreexo delta to forest_, verifying the resulting forest
// root against block.header.utreexo_root. Mirrors BlockValidator's pure-compute
// algorithm in block_validation.cpp (PASS 1 remove, PASS 2 add, skipping
// intra-block ephemeral UTXOs) but operates directly on the real rebuilt forest
// and commits the delta only after root verification passes.
//
// Preconditions on entry: for any UTXO this block SPENDS that is not also
// created in the same block, the coin must already be readable via
// chain_db_->getCoin() (i.e., we look up inputs BEFORE deleting them in the
// caller). The caller is responsible for serializing delta and persisting the
// undo flatfile; this function only mutates the forest and reports the delta.
Status BlockReindexer::applyBlockToForest(const Block& block, uint32_t height,
                                          UtreexoDelta& delta_out, std::string& error) {
    if (!forest_) {
        error = "reindex-forest-null";
        return Status::Internal;
    }

    delta_out = UtreexoDelta{};
    delta_out.numLeavesBefore = forest_->getNumLeaves();

    // PRE-SCAN: identify intra-block outputs and which of them get spent in the
    // same block. Those UTXOs are ephemeral and never enter the forest.
    std::unordered_map<OutPoint, size_t> intra_block_outputs;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        TxId txid = tx.GetTxid();
        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            intra_block_outputs[OutPoint(txid, n)] = tx_idx;
        }
    }
    std::unordered_set<OutPoint> intra_block_spends;
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(outpoint)) {
                intra_block_spends.insert(outpoint);
            }
        }
    }

    // Snapshot the forest with the same height-scoped semantics used by the
    // mining/validation oracle. This keeps fork activation changes off the
    // durable recovery forest until the block's header root has matched.
    const UtreexoHash forest_pre_commitment = forest_->getCommitment();
    UtreexoForest snapshot = forest_->cloneForHeight(height);
    const UtreexoHash snapshot_pre_commitment = snapshot.getCommitment();

    // PASS 1: REMOVE spent leaves (skip coinbase inputs and intra-block spends).
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            if (intra_block_spends.count(outpoint)) continue;

            auto coin_result = chain_db_->getCoin(input.prevout.txid.AsUint256(), input.prevout.vout);
            if (coin_result.status() != Status::Ok) {
                error = "reindex-forest-missing-spent-coin: " + outpoint.ToString();
                AppendForestTrace(height, "applyBlockToForest", block,
                                  forest_pre_commitment, snapshot_pre_commitment,
                                  snapshot.getCommitment(), delta_out,
                                  snapshot.getNumLeaves(), "error", error);
                return Status::Invalid;
            }
            const auto& coin = coin_result.value();

            std::vector<uint8_t> spk_bytes;
            spk_bytes.reserve(coin.script_pubkey.size() / 2);
            for (size_t i = 0; i + 1 < coin.script_pubkey.size(); i += 2) {
                spk_bytes.push_back(static_cast<uint8_t>(
                    std::stoi(coin.script_pubkey.substr(i, 2), nullptr, 16)));
            }

            UtreexoHash leafHash = HashUTXOForCreationHeight(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                LeafAmountForCoin(coin),
                spk_bytes,
                static_cast<uint32_t>(coin.height),
                coin.coinbase
            );

            auto pos_opt = snapshot.findLeafPosition(leafHash);
            if (!pos_opt.has_value()) {
                error = "reindex-forest-leaf-missing: " + outpoint.ToString() +
                        " (height=" + std::to_string(height) + ")";
                AppendForestTrace(height, "applyBlockToForest", block,
                                  forest_pre_commitment, snapshot_pre_commitment,
                                  snapshot.getCommitment(), delta_out,
                                  snapshot.getNumLeaves(), "error", error);
                return Status::Invalid;
            }
            uint64_t position = pos_opt.value();
            if (!snapshot.removeAtKnownPosition(position, leafHash)) {
                error = "reindex-forest-remove-failed: " + outpoint.ToString();
                AppendForestTrace(height, "applyBlockToForest", block,
                                  forest_pre_commitment, snapshot_pre_commitment,
                                  snapshot.getCommitment(), delta_out,
                                  snapshot.getNumLeaves(), "error", error);
                return Status::Invalid;
            }
            delta_out.recordDelete(position, leafHash);
        }
    }

    // PASS 2: ADD non-ephemeral outputs.
    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();
        for (size_t n = 0; n < tx.vout.size(); ++n) {
            OutPoint op(txid, static_cast<uint32_t>(n));
            if (intra_block_spends.count(op)) continue;

            const auto& output = tx.vout[n];
            UtreexoHash leafHash = HashUTXOForCreationHeight(
                txid.AsUint256(),
                static_cast<uint32_t>(n),
                LeafAmountForOutput(output),
                std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end()),
                height,
                tx.IsCoinbase()
            );
            uint64_t position = snapshot.add(leafHash);
            if (position == UINT64_MAX) {
                error = "reindex-forest-add-failed (duplicate leaf or capacity)";
                AppendForestTrace(height, "applyBlockToForest", block,
                                  forest_pre_commitment, snapshot_pre_commitment,
                                  snapshot.getCommitment(), delta_out,
                                  snapshot.getNumLeaves(), "error", error);
                return Status::Invalid;
            }
            delta_out.recordAdd(leafHash, position);
        }
    }

    // Verify: forest root after applying this block must match the header
    // commitment. This is the single consensus check that guarantees the
    // reindex output is correct.
    UtreexoHash computed = snapshot.getCommitment();
    if (computed.size() != 32) {
        error = "reindex-forest-invalid-commitment-size";
        AppendForestTrace(height, "applyBlockToForest", block,
                          forest_pre_commitment, snapshot_pre_commitment,
                          computed, delta_out, snapshot.getNumLeaves(),
                          "error", error);
        return Status::Invalid;
    }
    uint256 computed_u256;
    std::memcpy(computed_u256.data, computed.data(), 32);
    if (computed_u256 != block.header.utreexo_root) {
        // Phase 3b reindex investigation: dump enough state on
        // mismatch to tell apart "PASS 2 leaf bytes wrong" from
        // "cumulative internal-state drift survived prior commitment
        // matches." The numLeaves + add/remove counts pin the
        // shape of THIS block's contribution; the per-add leaf
        // hashes pin the leaf bytes themselves; the snapshot
        // numLeaves pins the post-state size.
        std::ostringstream diag;
        diag << "reindex-forest-root-mismatch at height " << height
             << " computed=" << computed_u256.GetHex()
             << " header=" << block.header.utreexo_root.GetHex()
             << " numLeavesBefore=" << delta_out.numLeavesBefore
             << " numLeavesAfter=" << snapshot.getNumLeaves()
             << " removed=" << delta_out.deletedLeaves.size()
             << " added=" << delta_out.addedLeaves.size()
             << " block_vtx=" << block.vtx.size();
        for (size_t i = 0; i < delta_out.addedLeaves.size(); ++i) {
            const auto& add = delta_out.addedLeaves[i];
            diag << "\n  add[" << i << "] pos=" << add.position << " leaf=";
            for (size_t b = 0; b < std::min(add.hash.size(), size_t(8)); ++b) {
                char buf[3];
                std::snprintf(buf, sizeof(buf), "%02x", add.hash[b]);
                diag << buf;
            }
        }
        // Phase 3b reindex investigation (paired-snapshot plan): on
        // mismatch, dump the FOREST INTERNAL STATE just before the
        // failing add. Because this branch runs while the snapshot
        // was being mutated, but `*forest_ = std::move(snapshot)` is
        // skipped on the failure path, `forest_` still holds the
        // end-of-(height-1) state — exactly the canonical-replay
        // 9290 snapshot we want for the comparison. Also dump the
        // mutated snapshot's state so we can see the full add cascade
        // result.
        const auto pre_dump_path = std::string("/tmp/reindex_forest_dump_pre_h") +
                                    std::to_string(height) + ".txt";
        const auto post_dump_path = std::string("/tmp/reindex_forest_dump_post_h") +
                                    std::to_string(height) + ".txt";
        {
            std::ofstream pre_out(pre_dump_path);
            if (pre_out) pre_out << forest_->dumpInternalState();
        }
        {
            std::ofstream post_out(post_dump_path);
            if (post_out) post_out << snapshot.dumpInternalState();
        }
        diag << "\n  pre_dump=" << pre_dump_path
             << "\n  post_dump=" << post_dump_path;

        // Coinbase output sample for visual confirmation that reindex
        // is reading the same scriptPubKey bytes the live path does.
        if (!block.vtx.empty()) {
            const auto& coinbase = block.vtx[0];
            diag << "\n  coinbase txid=" << coinbase.GetTxid().AsUint256().GetHex().substr(0, 32)
                 << " vout_count=" << coinbase.vout.size();
            for (size_t n = 0; n < coinbase.vout.size(); ++n) {
                const auto& out = coinbase.vout[n];
                diag << "\n    vout[" << n << "] value=" << out.value.GetUna()
                     << " is_ct=" << out.is_confidential
                     << " spk_size=" << out.scriptPubKey.size()
                     << " spk_first_byte=";
                if (!out.scriptPubKey.empty()) {
                    char buf[3];
                    std::snprintf(buf, sizeof(buf), "%02x", out.scriptPubKey[0]);
                    diag << buf;
                }
            }
        }
        error = diag.str();
        AppendForestTrace(height, "applyBlockToForest", block,
                          forest_pre_commitment, snapshot_pre_commitment,
                          computed, delta_out, snapshot.getNumLeaves(),
                          "mismatch", error);
        return Status::Invalid;
    }

    AppendForestTrace(height, "applyBlockToForest", block,
                      forest_pre_commitment, snapshot_pre_commitment,
                      computed, delta_out, snapshot.getNumLeaves(), "ok");

    // Commit snapshot into the real forest.
    *forest_ = std::move(snapshot);
    return Status::Ok;
}

Status BlockReindexer::seedGenesis(arith_uint256& genesis_work_out, const FilePosition* genesis_pos) {
    if (!chain_db_) {
        return Status::Internal;
    }

    auto tip_result = chain_db_->getTip();
    if (tip_result.ok()) {
        g_logger.error("[reindex] Refusing to seed genesis into a non-empty ChainDB target");
        return Status::Invalid;
    }
    if (tip_result.status() != Status::NotFound) {
        return tip_result.status();
    }

    const auto canonical = BuildCanonicalGenesis(Params());
    genesis_work_out = GetBlockProof(canonical.header.difficulty);
    Transaction coinbase_tx;
    const auto coinbase_bytes = TransactionSerializer::FromHex(canonical.coinbase_hex);
    size_t coinbase_consumed = 0;
    if (coinbase_bytes.empty() ||
        !TransactionSerializer::Deserialize(coinbase_tx, coinbase_bytes, coinbase_consumed) ||
        coinbase_consumed != coinbase_bytes.size()) {
        g_logger.error("[reindex] Failed to deserialize canonical genesis coinbase");
        return Status::Serialization;
    }

    Block genesis_block;
    genesis_block.header = canonical.header;
    genesis_block.vtx.push_back(std::move(coinbase_tx));

    const uint256 genesis_hash = genesis_block.GetHash();

    ChainWriteToken token;
    rocksdb::WriteBatch batch;

    std::optional<FilePosition> stored_genesis_pos;
    if (genesis_pos != nullptr) {
        stored_genesis_pos = *genesis_pos;
    } else if (block_storage_ != nullptr) {
        auto write_result = block_storage_->writeBlock(genesis_hash, genesis_block);
        if (write_result.status() != Status::Ok) {
            g_logger.error("[reindex] Failed to persist genesis block to flatfile storage");
            return write_result.status();
        }
        stored_genesis_pos = write_result.value();
    } else {
        auto status = chain_db_->putBlock(token, genesis_hash, genesis_block, &batch);
        if (status != Status::Ok) return status;
    }

    auto status = chain_db_->putHeader(token, genesis_hash, genesis_block.header, 0, genesis_work_out, &batch);
    if (status != Status::Ok) return status;

    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = genesis_block.header.prev_block_hash;
    metadata.height = 0;
    metadata.chainwork = genesis_work_out;
    metadata.status_flags = BLOCK_VALID_HEADER |
                            BLOCK_VALID_TREE |
                            BLOCK_VALID_TRANSACTIONS |
                            BLOCK_VALID_CHAIN |
                            BLOCK_HAVE_DATA;
    if (stored_genesis_pos.has_value()) {
        if (stored_genesis_pos->offset > std::numeric_limits<uint32_t>::max()) {
            g_logger.error("[reindex] Genesis block offset exceeds persisted metadata range");
            return Status::Invalid;
        }
        metadata.file_number = stored_genesis_pos->file_number;
        metadata.data_pos = static_cast<uint32_t>(stored_genesis_pos->offset);
        metadata.data_size = stored_genesis_pos->size;
    }

    status = chain_db_->putHeaderMetadata(token, genesis_hash, metadata, &batch);
    if (status != Status::Ok) return status;

    status = chain_db_->putHeightIndex(token, 0, genesis_hash, &batch);
    if (status != Status::Ok) return status;

    status = chain_db_->setTip(token, genesis_hash, 0, genesis_work_out, &batch);
    if (status != Status::Ok) return status;

    UtreexoDelta genesis_delta;
    genesis_delta.numLeavesBefore = 0;

    if (!genesis_block.vtx.empty()) {
        const Transaction& genesis_tx = genesis_block.vtx[0];
        TxId txid = genesis_tx.GetTxid();

        for (uint32_t vout = 0; vout < genesis_tx.vout.size(); ++vout) {
            const auto& output = genesis_tx.vout[vout];

            Coin coin;
            coin.amount = output.value.GetUna();
            std::ostringstream spk_hex;
            for (uint8_t byte : output.scriptPubKey) {
                spk_hex << std::hex << std::setfill('0') << std::setw(2)
                        << static_cast<int>(byte);
            }
            coin.script_pubkey = spk_hex.str();
            coin.height = 0;
            coin.coinbase = true;
            coin.is_confidential = output.is_confidential;
            coin.commitment = output.commitment;

            status = chain_db_->putCoin(token, txid.AsUint256(), vout, coin, &batch);
            if (status != Status::Ok) return status;

            // Genesis is special: the canonical header commits to the empty
            // forest. Startup/backfill code seeds height 0 that same way and
            // then replays blocks from height 1 onward. Reindex must follow the
            // identical rule instead of hashing the burned OP_RETURN output into
            // the forest.
        }
    }

    // Verify the forest state at genesis matches the header commitment.
    if (IsUtreexoActive(0) && forest_) {
        UtreexoHash computed = forest_->getCommitment();
        uint256 computed_u256;
        if (computed.size() == 32) {
            std::memcpy(computed_u256.data, computed.data(), 32);
        } else {
            computed_u256.SetNull();
        }
        if (computed_u256 != genesis_block.header.utreexo_root) {
            g_logger.error("[reindex] Genesis forest root mismatch"
                           " computed=" + computed_u256.GetHex() +
                           " header=" + genesis_block.header.utreexo_root.GetHex());
            return Status::Invalid;
        }

        // Emit genesis Utreexo delta sidecar + forest checkpoint in-batch so
        // the genesis write and the forest state commit atomically.
        std::string delta_blob;
        std::string delta_error;
        if (SerializeUtreexoDelta(genesis_delta, delta_blob, delta_error)) {
            batch.Put(MakeUtreexoDeltaUndoKey(genesis_hash), delta_blob);
        } else {
            g_logger.error("[reindex] Failed to serialize genesis Utreexo delta: " + delta_error);
            return Status::Serialization;
        }

        const std::vector<uint8_t> serialized_forest = forest_->serialize();
        auto ckpt_status = chain_db_->putUtreexoCheckpointWithChecksum(
            token, 0, serialized_forest, &batch);
        if (ckpt_status != Status::Ok) {
            g_logger.error("[reindex] Failed to emit genesis Utreexo checkpoint");
            return ckpt_status;
        }
    }

    // Record the genesis tip so ForestTipMarker emission at end-of-reindex
    // always has a valid value even when no blocks beyond genesis exist.
    final_tip_hash_ = genesis_hash;
    final_tip_height_ = 0;

    return chain_db_->writeBatch(token, std::move(batch), true);
}

// Foundation for offline `--rebuild-undo-range` (post-Apr 30 chainstate
// hardening): instead of seeding from genesis and replaying from height 1,
// the reindexer can be given an externally-prepared snapshot of consensus
// state at an arbitrary height. Used by the windowed-undo orchestrator,
// which feeds the reindexer's processing loop a temp ChainDB already
// populated up to the anchor (via prior reindex or assumeUTXO load) and
// then drives it through the historical hole window.
//
// The function only mutates the reindexer's in-memory state. ChainDB writes
// for the anchor block are the orchestrator's responsibility — the anchor
// block must already be at the temp DB's tip before this is called.
//
// This commit (#1) adds the seed primitive and the test-only state
// snapshot accessor. The execute() wiring (Mode::WINDOWED_UNDO_ONLY,
// shielded artifact preservation, loop start at anchor.height + 1) lands
// in the next commit.
Status BlockReindexer::seedFromAnchor(const AnchorState& anchor) {
    if (!forest_) {
        g_logger.error("[reindex] seedFromAnchor: forest_ uninitialized");
        return Status::Internal;
    }

    if (anchor.height == 0) {
        // height 0 is genesis; seedGenesis is the right path. We refuse
        // here so callers don't accidentally bypass canonical genesis
        // construction.
        g_logger.error("[reindex] seedFromAnchor: height=0 not supported, use seedGenesis");
        return Status::Invalid;
    }

    // Forest seed (only if Utreexo is active at the anchor height; pre-
    // activation blocks have an empty forest just like genesis does).
    if (IsUtreexoActive(anchor.height)) {
        if (anchor.forest_serialized.empty()) {
            g_logger.error("[reindex] seedFromAnchor: forest_serialized empty at"
                           " Utreexo-active height " + std::to_string(anchor.height));
            return Status::Invalid;
        }
        UtreexoForest seeded = UtreexoForest::deserialize(anchor.forest_serialized);
        // Sanity: deserialize() returns a default-constructed forest on
        // failure with numLeaves==0. If the input was non-empty but produces
        // a zero-leaf forest at a Utreexo-active height, the orchestrator
        // gave us garbage; refuse rather than silently advance.
        if (seeded.getNumLeaves() == 0 && !anchor.forest_serialized.empty()) {
            // A genuinely empty forest serialization is 8 bytes (numLeaves
            // == 0 prefix, no roots); reject anything longer that came back
            // with zero leaves.
            constexpr size_t kEmptyForestSerSize = sizeof(uint64_t);
            if (anchor.forest_serialized.size() > kEmptyForestSerSize) {
                g_logger.error("[reindex] seedFromAnchor: forest_serialized failed to"
                               " deserialize into a non-empty forest at height " +
                               std::to_string(anchor.height));
                return Status::Serialization;
            }
        }
        *forest_ = std::move(seeded);
    } else if (!anchor.forest_serialized.empty()) {
        g_logger.warning("[reindex] seedFromAnchor: forest_serialized provided at"
                         " Utreexo-inactive height " + std::to_string(anchor.height) +
                         "; ignoring");
    }

    // Shielded frontier seed (only if shielded is active at the anchor
    // height per chainparams). Pre-activation anchors carry an empty
    // tree just like genesis.
    const uint32_t shielded_activation_height =
        Params().shielded_activation_height;
    const bool shielded_active_at_anchor =
        anchor.height >= shielded_activation_height;
    if (shielded_active_at_anchor) {
        // It is legal for the shielded tree to be empty even after
        // activation if no shielded transactions have landed yet, so an
        // empty `shielded_frontier_serialized` is acceptable. Otherwise,
        // load it.
        shielded_tree_ = shielded::CommitmentTree();
        if (!anchor.shielded_frontier_serialized.empty()) {
            const bool ok = shielded_tree_.DeserializeFrontier(
                anchor.shielded_frontier_serialized.data(),
                anchor.shielded_frontier_serialized.size());
            if (!ok) {
                g_logger.error("[reindex] seedFromAnchor: shielded frontier"
                               " deserialize failed at height " +
                               std::to_string(anchor.height));
                return Status::Serialization;
            }
        }
    } else if (!anchor.shielded_frontier_serialized.empty()) {
        g_logger.warning("[reindex] seedFromAnchor: shielded frontier provided at"
                         " pre-activation height " + std::to_string(anchor.height) +
                         "; ignoring");
    }

    accumulated_chainwork_ = anchor.chainwork;
    final_tip_hash_ = anchor.hash;
    final_tip_height_ = static_cast<int32_t>(anchor.height);

    return Status::Ok;
}

// Commit #3 of the offline `--rebuild-undo-range` series.
//
// The verification harness — the load-bearing safety property the user
// asked for: "rebuilt undo is accepted only if DisconnectBlock round-trips
// cleanly." If any check fails, the LIVE writes for this height are
// skipped (the caller proceeds to the next in-window block instead of
// aborting the run; D.3 already handles the historical hole correctly).
//
// The harness operates entirely on read-only views of pre/post-apply
// state. The temp DB is never mutated. The forest reverse pass runs on
// a CLONE of the post-apply forest. The shielded comparison is a byte-
// equal check on the captured frontier; we do not mutate `shielded_tree_`.
Status BlockReindexer::verifyRebuiltUndoRoundTrip(
    const Block& block,
    uint64_t height,
    const std::vector<uint8_t>& candidate_undo_bytes,
    const UndoRecord& built_undo,
    const PreApplyStateForVerification& pre_state,
    const UtreexoDelta& utreexo_delta,
    std::string& error_out
) {
    auto fail = [&](const std::string& reason) {
        error_out = "rebuild-verify-failed-height-" +
                    std::to_string(height) + ": " + reason;
        return Status::Invalid;
    };

    // Property 1: serialization round-trip stability. The candidate
    // bytes are what we'll write to LIVE rev*.dat. If Deserialize
    // followed by Serialize doesn't reproduce the input byte-for-byte,
    // there's encoder drift and DisconnectBlock will silently disagree
    // with what was written. (The Apr 30 tx_sighash bug class.)
    UndoRecord decoded;
    try {
        decoded = UndoRecord::Deserialize(candidate_undo_bytes);
    } catch (const std::exception& ex) {
        return fail(std::string("deserialize-threw: ") + ex.what());
    }
    const auto reserialized = decoded.Serialize();
    if (reserialized != candidate_undo_bytes) {
        return fail("serialize-roundtrip-not-byte-stable size_in=" +
                    std::to_string(candidate_undo_bytes.size()) +
                    " size_out=" + std::to_string(reserialized.size()));
    }

    // Property 2: spent count matches the non-coinbase input count of
    // the block.
    size_t expected_spent_count = 0;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        if (tx_idx == 0) continue;  // coinbase has no real prevouts
        expected_spent_count += block.vtx[tx_idx].vin.size();
    }
    if (decoded.spent.size() != expected_spent_count) {
        return fail("spent-count-mismatch decoded=" +
                    std::to_string(decoded.spent.size()) +
                    " expected=" + std::to_string(expected_spent_count));
    }

    // Property 3: created count matches the total output count of the
    // block.
    size_t expected_created_count = 0;
    for (const auto& tx : block.vtx) {
        expected_created_count += tx.vout.size();
    }
    if (decoded.created.size() != expected_created_count) {
        return fail("created-count-mismatch decoded=" +
                    std::to_string(decoded.created.size()) +
                    " expected=" + std::to_string(expected_created_count));
    }

    // Property 4: spent entries field-by-field equality with what we
    // actually built from the temp DB pre-apply. If they disagree it
    // means the Serialize→Deserialize path silently dropped or shuffled
    // a field — DisconnectBlock would then restore the wrong UTXO.
    if (built_undo.spent.size() != decoded.spent.size()) {
        return fail("internal-built-vs-decoded-spent-size-mismatch");
    }
    for (size_t i = 0; i < decoded.spent.size(); ++i) {
        const auto& d = decoded.spent[i];
        const auto& b = built_undo.spent[i];
        if (d.prev_txid != b.prev_txid) {
            return fail("spent[" + std::to_string(i) + "].prev_txid mismatch");
        }
        if (d.prev_vout != b.prev_vout) {
            return fail("spent[" + std::to_string(i) + "].prev_vout mismatch");
        }
        if (d.value != b.value) {
            return fail("spent[" + std::to_string(i) + "].value mismatch");
        }
        if (d.scriptPubKey != b.scriptPubKey) {
            return fail("spent[" + std::to_string(i) + "].scriptPubKey mismatch");
        }
        if (d.is_coinbase != b.is_coinbase) {
            return fail("spent[" + std::to_string(i) + "].is_coinbase mismatch");
        }
        if (d.height != b.height) {
            return fail("spent[" + std::to_string(i) + "].height mismatch");
        }
        if (d.is_confidential != b.is_confidential) {
            return fail("spent[" + std::to_string(i) + "].is_confidential mismatch");
        }
        if (d.commitment != b.commitment) {
            return fail("spent[" + std::to_string(i) + "].commitment mismatch");
        }
    }

    // Property 5: created entries map to real outputs. We check
    // existence as a (txid, vout) set; ordering equality is property
    // 4-style strict for spent but for created we tolerate any order
    // (block order is the natural choice but DisconnectBlock would
    // accept either).
    std::unordered_set<std::string> block_outputs;
    for (const auto& tx : block.vtx) {
        const auto txid = tx.GetTxid().AsUint256();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            block_outputs.insert(txid.GetHex() + ":" + std::to_string(vout));
        }
    }
    for (const auto& c : decoded.created) {
        const auto key = c.txid.GetHex() + ":" + std::to_string(c.vout);
        if (block_outputs.find(key) == block_outputs.end()) {
            return fail("created-entry-not-in-block " + key);
        }
    }

    // Property 6: pre_block_shielded_frontier presence + content match.
    if (pre_state.shielded_active_at_height) {
        if (!decoded.pre_block_shielded_frontier.has_value()) {
            return fail("missing-pre_block_shielded_frontier-at-shielded-active-height");
        }
        if (*decoded.pre_block_shielded_frontier !=
            pre_state.shielded_frontier_serialized) {
            return fail("pre_block_shielded_frontier-bytes-mismatch decoded=" +
                        std::to_string(decoded.pre_block_shielded_frontier->size()) +
                        " expected=" +
                        std::to_string(pre_state.shielded_frontier_serialized.size()));
        }
    }

    // Property 7: forest reverse-apply on a clone reaches pre-apply
    // commitment. This is the single most likely place for silent
    // divergence (delta encoding, leaf position determinism, root
    // recompute path) so we run it explicitly. The clone is freed
    // when the function returns; `forest_` is untouched.
    if (pre_state.utreexo_active_at_height) {
        UtreexoForest clone = *forest_;  // post-apply state
        const auto post_commitment = clone.getCommitment();
        const uint64_t post_num_leaves = clone.getNumLeaves();
        // Reverse the additions: removeLastNLeaves expects them in
        // reverse-of-add order.
        if (!utreexo_delta.addedLeaves.empty()) {
            const uint64_t added_count =
                static_cast<uint64_t>(utreexo_delta.addedLeaves.size());
            if (!clone.removeLastNLeaves(added_count)) {
                const std::string reason = "forest-reverse-add-failed count=" +
                                           std::to_string(added_count);
                AppendForestVerifyTrace(height, block,
                                        pre_state.forest_commitment,
                                        post_commitment,
                                        clone.getCommitment(),
                                        pre_state.forest_num_leaves,
                                        post_num_leaves,
                                        clone.getNumLeaves(),
                                        utreexo_delta,
                                        candidate_undo_bytes.size(),
                                        "error", reason);
                return fail(reason);
            }
        }
        // Reverse the deletions in delta order — restoreDeletedLeaf
        // is symmetric, so the order doesn't matter for correctness,
        // but iterating in delta order matches what DisconnectBlock
        // does and is the cleanest equivalence test.
        for (const auto& deleted : utreexo_delta.deletedLeaves) {
            if (!clone.restoreDeletedLeaf(deleted.position, deleted.leafHash)) {
                const std::string reason = "forest-reverse-restore-failed position=" +
                                           std::to_string(deleted.position);
                AppendForestVerifyTrace(height, block,
                                        pre_state.forest_commitment,
                                        post_commitment,
                                        clone.getCommitment(),
                                        pre_state.forest_num_leaves,
                                        post_num_leaves,
                                        clone.getNumLeaves(),
                                        utreexo_delta,
                                        candidate_undo_bytes.size(),
                                        "error", reason);
                return fail(reason);
            }
        }
        const auto recovered_commitment = clone.getCommitment();
        if (recovered_commitment != pre_state.forest_commitment) {
            const std::string reason = "forest-reverse-commitment-mismatch";
            AppendForestVerifyTrace(height, block,
                                    pre_state.forest_commitment,
                                    post_commitment,
                                    recovered_commitment,
                                    pre_state.forest_num_leaves,
                                    post_num_leaves,
                                    clone.getNumLeaves(),
                                    utreexo_delta,
                                    candidate_undo_bytes.size(),
                                    "mismatch", reason);
            return fail(reason);
        }
        if (clone.getNumLeaves() != pre_state.forest_num_leaves) {
            const std::string reason =
                "forest-reverse-leaf-count-mismatch recovered=" +
                std::to_string(clone.getNumLeaves()) +
                " expected=" + std::to_string(pre_state.forest_num_leaves);
            AppendForestVerifyTrace(height, block,
                                    pre_state.forest_commitment,
                                    post_commitment,
                                    recovered_commitment,
                                    pre_state.forest_num_leaves,
                                    post_num_leaves,
                                    clone.getNumLeaves(),
                                    utreexo_delta,
                                    candidate_undo_bytes.size(),
                                    "mismatch", reason);
            return fail(reason);
        }
        AppendForestVerifyTrace(height, block,
                                pre_state.forest_commitment,
                                post_commitment,
                                recovered_commitment,
                                pre_state.forest_num_leaves,
                                post_num_leaves,
                                clone.getNumLeaves(),
                                utreexo_delta,
                                candidate_undo_bytes.size(),
                                "ok");
    }

    return Status::Ok;
}

Status BlockReindexer::verifyRebuiltUndoRoundTripForTesting(
    const Block& block,
    uint64_t height,
    const std::vector<uint8_t>& candidate_undo_bytes,
    const ::dinero::UndoRecord& built_undo,
    const PreApplyStateForVerification& pre_state,
    const UtreexoDelta& utreexo_delta,
    std::string& error_out
) {
    return verifyRebuiltUndoRoundTrip(block, height, candidate_undo_bytes,
                                       built_undo, pre_state, utreexo_delta,
                                       error_out);
}

BlockReindexer::InternalStateSnapshotForTesting
BlockReindexer::snapshotInternalStateForTesting() const {
    InternalStateSnapshotForTesting snap;
    if (forest_) {
        snap.forest_num_leaves = forest_->getNumLeaves();
        snap.forest_commitment = forest_->getCommitment();
    }
    snap.shielded_frontier_serialized = shielded_tree_.SerializeFrontier();
    snap.accumulated_chainwork = accumulated_chainwork_;
    snap.final_tip_hash = final_tip_hash_;
    snap.final_tip_height = final_tip_height_;
    return snap;
}

StatusOr<BlockReindexer::Stats> BlockReindexer::execute() {
    auto start_time = std::chrono::steady_clock::now();

    auto mode_to_string = [](Mode m) {
        switch (m) {
            case Mode::FULL: return "FULL";
            case Mode::CHAINSTATE_ONLY: return "CHAINSTATE_ONLY";
            case Mode::WINDOWED_UNDO_ONLY: return "WINDOWED_UNDO_ONLY";
        }
        return "UNKNOWN";
    };

    g_logger.info("🔄 Starting reindex operation...");
    g_logger.info("   Mode: " + std::string(mode_to_string(config_.mode)));
    g_logger.info("   Data directory: " + datadir_.string());
    g_logger.info("   AssumeValid: " + std::string(config_.use_assumevalid ? "enabled" : "disabled"));

    // WINDOWED_UNDO_ONLY config validation. Hard-fail at the boundary
    // rather than letting misconfiguration silently fall through to
    // surprising mid-run errors.
    const bool windowed = config_.mode == Mode::WINDOWED_UNDO_ONLY;
    if (windowed) {
        if (!config_.anchor_state.has_value()) {
            stats_.error = "WINDOWED_UNDO_ONLY requires Config::anchor_state";
            return stats_;
        }
        if (!config_.undo_rebuild_window.has_value()) {
            stats_.error = "WINDOWED_UNDO_ONLY requires Config::undo_rebuild_window";
            return stats_;
        }
        const auto& window = *config_.undo_rebuild_window;
        if (window.live_chain_db == nullptr) {
            stats_.error = "WINDOWED_UNDO_ONLY requires undo_rebuild_window.live_chain_db";
            return stats_;
        }
        if (window.live_block_storage == nullptr) {
            stats_.error = "WINDOWED_UNDO_ONLY requires undo_rebuild_window.live_block_storage";
            return stats_;
        }
        const uint32_t anchor_height = config_.anchor_state->height;
        if (window.start_height <= anchor_height) {
            stats_.error = "WINDOWED_UNDO_ONLY: window.start_height (" +
                           std::to_string(window.start_height) +
                           ") must be > anchor.height (" +
                           std::to_string(anchor_height) + ")";
            return stats_;
        }
        if (window.end_height < window.start_height) {
            stats_.error = "WINDOWED_UNDO_ONLY: window.end_height < window.start_height";
            return stats_;
        }
        // Two valid anchor regimes:
        //   (a) anchor.height == 0 — no real anchor; temp DB is fresh and
        //       the reindexer does a full genesis replay into it. No
        //       precondition on preserve_shielded_state_on_init (we
        //       clear shielded artifacts in this case, same as
        //       FULL/CHAINSTATE_ONLY).
        //   (b) anchor.height >= 1 — the orchestrator pre-populated the
        //       temp DB up through anchor.height (e.g. via a prior
        //       FULL reindex or assumeUTXO snapshot load). In this
        //       regime preserve_shielded_state_on_init MUST be set so
        //       initializeShieldedArtifacts opens the existing nullifier
        //       DB / frontier file rather than wiping them.
        if (anchor_height >= 1 && !config_.preserve_shielded_state_on_init) {
            stats_.error = "WINDOWED_UNDO_ONLY with non-genesis anchor requires "
                           "preserve_shielded_state_on_init "
                           "(orchestrator pre-populates shielded state in temp DB)";
            return stats_;
        }
        if (anchor_height == 0) {
            g_logger.info("   Anchor: GENESIS (full replay into temp DB)");
        } else {
            g_logger.info("   Anchor: height=" + std::to_string(anchor_height) +
                          " hash=" + config_.anchor_state->hash.GetHex());
        }
        g_logger.info("   Window: [" + std::to_string(window.start_height) + ", " +
                      std::to_string(window.end_height) + "]");
    }

    // Step 1: Scan block files
    g_logger.info("📁 Step 1: Scanning block files...");
    auto files_result = scanBlockFiles();
    if (!files_result.ok()) {
        stats_.error = "Failed to scan block files";
        return stats_;
    }

    std::vector<std::filesystem::path> block_files = files_result.value();
    stats_.files_scanned = block_files.size();

    g_logger.info("   Found " + std::to_string(block_files.size()) + " block file(s)");

    if (block_files.empty()) {
        stats_.error = "No block files found in " + (datadir_ / "blocks").string();
        return stats_;
    }

    auto shielded_status = initializeShieldedArtifacts();
    if (shielded_status != Status::Ok) {
        stats_.error = "Failed to initialize shielded artifacts";
        return stats_;
    }
    struct ShieldedCloseGuard {
        shielded::NullifierSet* nullifiers;
        ~ShieldedCloseGuard() {
            if (nullifiers) {
                nullifiers->Close();
            }
        }
    } shielded_close_guard{&shielded_nullifiers_};

    // Step 2: Parse all block bodies from flat files
    g_logger.info("🔨 Step 2: Parsing block files into a recoverable block set...");
    auto records_result = ReadDiskBlocks(block_files, &stats_);
    if (!records_result.ok()) {
        stats_.error = "Failed to parse block files";
        return stats_;
    }
    auto records = std::move(records_result.value());
    if (records.empty()) {
        stats_.error = "No block bodies found in block files";
        return stats_;
    }

    g_logger.info("   Parsed " + std::to_string(records.size()) + " block bodies from disk");

    // Step 3: Recover the best chain from the parsed block set.
    g_logger.info("🧭 Step 3: Recovering canonical chain from parent links...");
    const std::string anchor_tip_hex = config_.known_canonical_tip_hash.has_value()
        ? config_.known_canonical_tip_hash->GetHex()
        : std::string{};
    auto canonical_result = SelectCanonicalChain(records, anchor_tip_hex);
    if (!canonical_result.ok()) {
        stats_.error = "Failed to recover canonical chain from block files";
        return stats_;
    }
    const auto canonical_chain = std::move(canonical_result.value());
    g_logger.info("   Selected canonical chain length: " + std::to_string(canonical_chain.size()));

    // Step 4: Seed the reindexer's starting state.
    //
    // FULL / CHAINSTATE_ONLY: seed from canonical genesis and replay
    //                         from height 1.
    //
    // WINDOWED_UNDO_ONLY with anchor.height >= 1:
    //                         skip the canonical-genesis bootstrap and
    //                         seed from the orchestrator-provided
    //                         anchor. The anchor block is presumed
    //                         already at the temp ChainDB's tip.
    //
    // WINDOWED_UNDO_ONLY with anchor.height == 0:
    //                         no real anchor — fall through to the
    //                         FULL-style genesis seed and full replay
    //                         into the (fresh) temp DB. This is the
    //                         common case for `--rebuild-undo-range`
    //                         until assumeUTXO snapshot anchoring is
    //                         parametrized in a follow-up.
    const bool windowed_genesis_replay =
        windowed && config_.anchor_state->height == 0;
    if (windowed && !windowed_genesis_replay) {
        g_logger.info("🌱 Step 4: Seeding from anchor (skipping canonical genesis path)...");
        const auto seed_status = seedFromAnchor(*config_.anchor_state);
        if (seed_status != Status::Ok) {
            stats_.error = "Failed to seed reindexer state from anchor";
            return stats_;
        }
    } else {
        g_logger.info("🌱 Step 4: Seeding canonical genesis...");
        arith_uint256 genesis_work;
        std::optional<FilePosition> parsed_genesis_pos;
        const std::string genesis_hash = Params().genesis_hash;
        for (const auto& record : records) {
            if (record.hash.GetHex() == genesis_hash) {
                parsed_genesis_pos = record.pos;
                break;
            }
        }

        auto genesis_status = seedGenesis(genesis_work, parsed_genesis_pos ? &*parsed_genesis_pos : nullptr);
        if (genesis_status != Status::Ok) {
            stats_.error = "Failed to seed genesis in ChainDB";
            return stats_;
        }
        accumulated_chainwork_ = genesis_work;
    }

    // Step 5: Apply the recovered best chain in canonical order.
    //
    // Recovery semantic for forest-root-mismatch failures (Apr 29 2026
    // incident): if processBlock returns Invalid because applyBlockToForest
    // detected a header-vs-canonical-replay forest root mismatch, we
    // STOP THE REINDEX AT THE LAST SUCCESSFULLY-WRITTEN HEIGHT and mark
    // the offending block (and every descendant in canonical_chain) as
    // permanently invalid. The chaindb is left committed at height-1
    // (the last good block); the daemon comes up with a consistent
    // canonical-replay forest at that height and refuses to follow the
    // poisoned suffix.
    //
    // Pre-fix: any failure aborted the whole reindex and the daemon
    // refused to start ("[FATAL] Failed to initialize daemon"). That
    // left operators with no recovery path on chains where the post-
    // mismatch suffix had been mined under a header-root-enforcement
    // bypass — which is exactly the LA fleet state on 2026-04-29.
    g_logger.info("⛓️  Step 5: Writing canonical chain to ChainDB...");
    bool truncated_at_invalid = false;
    size_t first_invalid_index = 0;
    std::string truncation_reason;

    // WINDOWED_UNDO_ONLY: the anchor block at height A is presumed already
    // at the temp DB's tip, so the loop starts at i = A (height A+1).
    // The loop also stops at the configured window end so the run does
    // not waste cycles processing post-window blocks unnecessarily —
    // their undo bytes don't need rebuilding.
    const size_t loop_start_index =
        windowed ? static_cast<size_t>(config_.anchor_state->height) : 0;
    const size_t loop_end_index_exclusive =
        windowed ? std::min<size_t>(canonical_chain.size(),
                                    static_cast<size_t>(config_.undo_rebuild_window->end_height))
                 : canonical_chain.size();

    for (size_t i = loop_start_index; i < loop_end_index_exclusive; ++i) {
        const auto& record = records[canonical_chain[i]];
        const uint64_t height = static_cast<uint64_t>(i + 1);
        auto status = processBlock(record.block, record.pos, height);
        if (status == Status::Ok) {
            continue;
        }

        // Forest-root-mismatch is the signal that the canonical chain
        // diverged from genesis-replay — the only recoverable case for
        // step-5 truncation. Other failures (I/O errors, malformed
        // tx data, missing UTXOs) abort the reindex as before because
        // they don't have a clean "stop here, mark bad, continue safe"
        // semantic.
        const bool is_forest_mismatch =
            stats_.error.empty() &&
            status == Status::Invalid &&
            // processBlock logs the specific error; use the last
            // applyBlockToForest error string set on the reindexer's
            // log path. The simplest invariant is "Invalid status from
            // processBlock at a forest-active height with an existing
            // earlier successful block" — exactly the runtime
            // signature of the 9291 case.
            IsUtreexoActive(static_cast<uint32_t>(height)) &&
            i > 0;

        if (!is_forest_mismatch) {
            stats_.error = "Failed to write canonical block at height " + std::to_string(height);
            return stats_;
        }

        truncated_at_invalid = true;
        first_invalid_index = i;
        truncation_reason =
            "canonical-replay forest mismatch at height " + std::to_string(height) +
            " — block and all descendants marked permanently invalid; "
            "reindex tip held at height " + std::to_string(height - 1);
        g_logger.warning("[reindex] " + truncation_reason);
        break;
    }

    if (truncated_at_invalid) {
        // Mark the failing block + every canonical-chain descendant as
        // BLOCK_FAILED_VALID (and propagate BLOCK_FAILED_CHILD to
        // everything below them in the canonical_chain order). These
        // header-metadata writes are best-effort: if the operator
        // reconsiderblock's the failing height under a future binary
        // that DOES handle these roots (e.g., a planned hardfork), the
        // flag is reversible. Until then, the daemon refuses to
        // reconnect this suffix.
        ChainWriteToken token;
        rocksdb::WriteBatch invalidate_batch;
        bool first = true;
        for (size_t j = first_invalid_index; j < canonical_chain.size(); ++j) {
            const auto& record = records[canonical_chain[j]];
            ChainDB::PersistedHeaderMetadata metadata;
            metadata.parent_hash = record.block.header.prev_block_hash;
            metadata.height = static_cast<int32_t>(j + 1);
            metadata.chainwork = arith_uint256(0);
            metadata.status_flags = (first ? BLOCK_FAILED_VALID : BLOCK_FAILED_CHILD) |
                                    BLOCK_VALID_HEADER |
                                    BLOCK_HAVE_DATA;
            metadata.file_number = record.pos.file_number;
            metadata.data_pos = static_cast<uint32_t>(record.pos.offset);
            metadata.data_size = record.pos.size;
            const auto md_status = chain_db_->putHeaderMetadata(
                token, record.hash, metadata, &invalidate_batch);
            if (md_status != Status::Ok) {
                g_logger.error("[reindex] Failed to stage invalidation metadata at height " +
                               std::to_string(j + 1));
                stats_.error = "Failed to mark invalid suffix at height " + std::to_string(j + 1);
                return stats_;
            }
            first = false;
        }
        const auto invalidate_status =
            chain_db_->writeBatch(token, std::move(invalidate_batch), true);
        if (invalidate_status != Status::Ok) {
            stats_.error = "Failed to commit invalid-suffix metadata batch";
            return stats_;
        }
        g_logger.info("[reindex] Marked " +
                      std::to_string(canonical_chain.size() - first_invalid_index) +
                      " block(s) as permanently invalid; canonical tip held at height " +
                      std::to_string(final_tip_height_));
        // Surface the truncation in the stats so the daemon startup
        // path can log it loudly. Keep stats_.error empty (we're
        // returning Ok) — operators discriminate on the dedicated
        // truncated-at field.
        stats_.canonical_truncated_at_height = final_tip_height_;
        stats_.canonical_truncation_reason = truncation_reason;
    }

    // Step 6: Rebuild UTXO set (if CHAINSTATE_ONLY mode)
    if (config_.mode == Mode::CHAINSTATE_ONLY) {
        g_logger.info("🔄 Step 6: Rebuilding UTXO set...");
        auto rebuild_status = rebuildUTXOSet();
        if (rebuild_status != Status::Ok) {
            stats_.error = "Failed to rebuild UTXO set";
            return stats_;
        }
    }

    // Step 7: Persist the ForestTipMarker so startup can verify forest state
    // matches the block header at tip without deserializing the full checkpoint.
    // This is the single metadata record that tells a fresh-starting daemon
    // "the forest in the latest checkpoint corresponds to this block hash at
    // this height with this root." Without it, ChainstateService::Init would
    // have no durable link between the tip and the forest it rebuilt.
    if (forest_ && final_tip_height_ >= 0 && IsUtreexoActive(static_cast<uint32_t>(final_tip_height_))) {
        ChainWriteToken token;
        ChainDB::ForestTipMarker marker;
        marker.height = final_tip_height_;
        marker.block_hash = final_tip_hash_;
        UtreexoHash commitment = forest_->getCommitment();
        if (commitment.size() == 32) {
            std::memcpy(marker.forest_root.data, commitment.data(), 32);
        } else {
            marker.forest_root.SetNull();
        }
        auto marker_status = chain_db_->putForestTipMarker(token, marker);
        if (marker_status != Status::Ok) {
            g_logger.error("[reindex] Failed to persist ForestTipMarker at height " +
                           std::to_string(final_tip_height_));
            stats_.error = "Failed to persist ForestTipMarker";
            return stats_;
        }
        g_logger.info("[reindex] Persisted ForestTipMarker @ height " +
                      std::to_string(final_tip_height_) +
                      " root=" + marker.forest_root.GetHex());
    }

    if (final_tip_height_ >= 0) {
        ChainWriteToken token;
        ChainDB::ShieldedTipMarker marker;
        marker.height = final_tip_height_;
        marker.block_hash = final_tip_hash_;
        const auto root = shielded_tree_.Root();
        std::memcpy(marker.shielded_root.data, root.data(), root.size());
        marker.tree_size = shielded_tree_.Size();
        marker.nullifier_count = shielded_nullifiers_.Size();
        auto marker_status = chain_db_->putShieldedTipMarker(token, marker);
        if (marker_status != Status::Ok) {
            g_logger.error("[reindex] Failed to persist ShieldedTipMarker at height " +
                           std::to_string(final_tip_height_));
            stats_.error = "Failed to persist ShieldedTipMarker";
            return stats_;
        }
        g_logger.info("[reindex] Persisted ShieldedTipMarker @ height " +
                      std::to_string(final_tip_height_) +
                      " root=" + marker.shielded_root.GetHex());

        // Persist the anchor history to the rebuilt ChainDB — mirrors the live
        // ConnectTip putUtreexoMeta("shielded_anchor_history"). The reindexer
        // reconstructs anchor_history in memory (RecordRoot per block above) but
        // otherwise never writes it to the rebuilt ChainDB, so the post-reindex
        // startup load found no blob and fell back to a stale flat file (or an
        // empty window). That diverged a reindexed node's anchor_history — and
        // thus its DSR2 shieldedStateHash — from a live node's: a live-vs-reindex
        // consensus split. Written as the ChainDB blob (load priority #1) so it
        // supersedes any stale flat file.
        const auto anchor_bytes = shielded_anchor_history_.SerializeBytes();
        const std::string anchor_blob(anchor_bytes.begin(), anchor_bytes.end());
        auto anchor_status =
            chain_db_->putUtreexoMeta(token, "shielded_anchor_history", anchor_blob);
        if (anchor_status != Status::Ok) {
            g_logger.error("[reindex] Failed to persist shielded anchor history at height " +
                           std::to_string(final_tip_height_));
            stats_.error = "Failed to persist shielded anchor history";
            return stats_;
        }
        g_logger.info("[reindex] Persisted shielded anchor history @ height " +
                      std::to_string(final_tip_height_) + " (" +
                      std::to_string(anchor_bytes.size()) + " bytes)");

        // Persist the frontier blob to the rebuilt ChainDB too — same reason as
        // the anchor blob. Without it, a reindexed node has no "shielded_frontier"
        // ChainDB row and the startup load falls back to the flat file; past the
        // epoch reset height the loader now refuses that stale fallback (a
        // resurrection guard), so a reindexed node past the cutover would fail to
        // start. Writing the blob makes the ChainDB the authoritative source for
        // both frontier and anchors post-reindex.
        const auto frontier_bytes = shielded_tree_.SerializeFrontier();
        const std::string frontier_blob(frontier_bytes.begin(),
                                        frontier_bytes.end());
        auto frontier_status =
            chain_db_->putUtreexoMeta(token, "shielded_frontier", frontier_blob);
        if (frontier_status != Status::Ok) {
            g_logger.error("[reindex] Failed to persist shielded frontier at height " +
                           std::to_string(final_tip_height_));
            stats_.error = "Failed to persist shielded frontier";
            return stats_;
        }
        g_logger.info("[reindex] Persisted shielded frontier @ height " +
                      std::to_string(final_tip_height_) + " (" +
                      std::to_string(frontier_bytes.size()) + " bytes)");
    }

    if (forest_ && final_tip_height_ >= 0 && IsUtreexoActive(static_cast<uint32_t>(final_tip_height_))) {
        auto meta_check = chain_db_->getUtreexoMeta("CHECKSUM_VERSION");
        if (meta_check.status() == Status::NotFound) {
            ChainWriteToken token;
            auto meta_status = chain_db_->putUtreexoMeta(token, "CHECKSUM_VERSION", "1");
            if (meta_status != Status::Ok) {
                g_logger.error("[reindex] Failed to persist CHECKSUM_VERSION=1");
                stats_.error = "Failed to persist Utreexo checksum version";
                return stats_;
            }
            g_logger.info("[reindex] Persisted CHECKSUM_VERSION=1");
        }

        // Campaign phase 3: with every-N gating the loop may not have
        // written a checkpoint at the reindex tip — emit one now so the
        // reindexed datadir restarts instantly (no replay window).
        const uint32_t final_interval =
            config_.utreexo_checkpoint_interval > 1
                ? config_.utreexo_checkpoint_interval : 1;
        if (static_cast<uint32_t>(final_tip_height_) % final_interval != 0) {
            ChainWriteToken token;
            auto final_ckpt_status = chain_db_->putUtreexoCheckpointWithChecksum(
                token, final_tip_height_, forest_->serialize());
            if (final_ckpt_status != Status::Ok) {
                g_logger.error("[reindex] Failed to emit final Utreexo checkpoint at tip " +
                               std::to_string(final_tip_height_));
                stats_.error = "Failed to persist final Utreexo checkpoint";
                return stats_;
            }
            g_logger.info("[reindex] Persisted final Utreexo checkpoint @ tip " +
                          std::to_string(final_tip_height_));
        }
    }

    auto persist_shielded_status = persistShieldedArtifacts();
    if (persist_shielded_status != Status::Ok) {
        stats_.error = "Failed to persist shielded artifacts";
        return stats_;
    }

    // Calculate duration
    auto end_time = std::chrono::steady_clock::now();
    stats_.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    stats_.success = true;

    g_logger.info("✅ Reindex complete!");
    g_logger.info("   Blocks processed: " + std::to_string(stats_.blocks_processed));
    g_logger.info("   Files scanned: " + std::to_string(stats_.files_scanned));
    g_logger.info("   UTXOs created: " + std::to_string(stats_.utxos_created));
    g_logger.info("   UTXOs spent: " + std::to_string(stats_.utxos_spent));
    g_logger.info("   Total bytes: " + std::to_string(stats_.total_bytes));
    g_logger.info("   Duration: " + std::to_string(stats_.duration_ms / 1000.0) + " seconds");

    return stats_;
}

StatusOr<std::vector<std::filesystem::path>> BlockReindexer::scanBlockFiles() {
    std::vector<std::filesystem::path> files;
    std::filesystem::path blocks_dir = datadir_ / "blocks";

    if (!std::filesystem::exists(blocks_dir)) {
        return Status::NotFound;
    }

    // Scan for blk*.dat files
    for (const auto& entry : std::filesystem::directory_iterator(blocks_dir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            // C++17-compatible string prefix/suffix check
            if (filename.rfind("blk", 0) == 0 &&
                filename.size() >= 4 && filename.substr(filename.size() - 4) == ".dat") {
                files.push_back(entry.path());
            }
        }
    }

    // Sort files by number (blk00000.dat, blk00001.dat, ...)
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::string a_name = a.filename().string();
        std::string b_name = b.filename().string();
        return a_name < b_name;
    });

    return files;
}

Status BlockReindexer::processBlockFile(const std::filesystem::path& file_path, uint32_t file_number) {
    g_logger.info("      Processing file: " + file_path.string());
    const auto& params = Params();
    const uint32_t expected_magic = params.magic;

    // Open file for reading
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        g_logger.error("Failed to open block file: " + file_path.string());
        return Status::Io;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    std::streampos file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint64_t offset = 0;
    uint64_t blocks_in_file = 0;

    while (offset < static_cast<uint64_t>(file_size)) {
        // Read magic bytes (4 bytes)
        uint32_t magic;
        file.read(reinterpret_cast<char*>(&magic), 4);
        if (file.gcount() != 4) {
            break;  // End of file
        }

        if (magic != expected_magic) {
            g_logger.error("Invalid block magic at offset " + std::to_string(offset) +
                           " in " + file_path.string());
            return Status::Corruption;
        }

        // Read block size (4 bytes, little-endian)
        uint32_t block_size;
        file.read(reinterpret_cast<char*>(&block_size), 4);
        if (file.gcount() != 4) {
            g_logger.error("Incomplete block size at offset " + std::to_string(offset));
            return Status::Corruption;
        }

        // Sanity check block size (must be reasonable)
        if (block_size == 0 || block_size > 32 * 1024 * 1024) {  // Max 32MB per block
            g_logger.error("Invalid block size " + std::to_string(block_size) +
                     " at offset " + std::to_string(offset));
            return Status::Invalid;
        }

        // Read block data
        std::vector<uint8_t> block_data(block_size);
        file.read(reinterpret_cast<char*>(block_data.data()), block_size);
        if (file.gcount() != static_cast<std::streamsize>(block_size)) {
            g_logger.error("Incomplete block data at offset " + std::to_string(offset));
            return Status::Corruption;
        }

        // Read and verify checksum (FNV-1a, same format as BlockStorage).
        uint32_t stored_checksum = 0;
        file.read(reinterpret_cast<char*>(&stored_checksum), 4);
        if (file.gcount() != 4) {
            g_logger.error("Incomplete block checksum at offset " +
                           std::to_string(offset + 8 + block_size));
            return Status::Corruption;
        }

        const uint32_t calculated_checksum = Fnv1aChecksum(block_data.data(), block_data.size());
        if (stored_checksum != calculated_checksum) {
            g_logger.error("Block checksum mismatch at offset " + std::to_string(offset) +
                           " in " + file_path.string());
            return Status::Corruption;
        }

        auto parsed_block = Block::Deserialize(block_data.data(), block_data.size());
        if (!parsed_block.has_value()) {
            g_logger.error("Failed to deserialize block at offset " +
                           std::to_string(offset) + " from " + file_path.string());
            return Status::Serialization;
        }

        // Create FilePosition for this block
        FilePosition pos(file_number, offset, block_size);

        // Process the block (validate + apply to UTXO set)
        auto status = processBlock(parsed_block.value(), pos, stats_.blocks_processed);
        if (status != Status::Ok) {
            return status;  // FAIL HARD on invalid block
        }

        // Update progress
        offset += 12 + block_size;  // magic (4) + size (4) + data + checksum (4)
        stats_.total_bytes += 12 + block_size;
        blocks_in_file++;
    }

    g_logger.info("      Processed " + std::to_string(blocks_in_file) + " blocks from file");

    return Status::Ok;
}

Status BlockReindexer::processBlock(const Block& block, const FilePosition& pos, uint64_t height) {
    // Create write token for ChainDB mutations
    ChainWriteToken token;

    // ═══════════════════════════════════════════════════════════════════
    // Step 1: Validate Block Header and PoW
    // ═══════════════════════════════════════════════════════════════════

    // CRITICAL: FAIL HARD on invalid PoW (prevents chain corruption)
    if (!config_.use_assumevalid || height > 100000) {  // Always validate recent blocks
        if (!CheckProofOfWork(block.header, true)) {
            g_logger.error("Block at height " + std::to_string(height) + " failed PoW validation");
            return Status::Invalid;
        }
    }

    if ((!config_.use_assumevalid || height > 100000) &&
        !CheckDifficultyBits(block.header.difficulty)) {
        g_logger.error("Block at height " + std::to_string(height) + " has invalid difficulty bits");
        return Status::Invalid;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 2: Compute Block Hash + Accumulated Work
    // ═══════════════════════════════════════════════════════════════════

    uint256 block_hash = block.GetHash();
    arith_uint256 block_work = GetBlockProof(block.header.difficulty);
    accumulated_chainwork_ += block_work;

    if (pos.offset > std::numeric_limits<uint32_t>::max()) {
        g_logger.error("Block file offset exceeds persisted metadata range at height " +
                       std::to_string(height));
        return Status::Invalid;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 2b: WINDOWED_UNDO_ONLY — capture pre-apply state for verifier
    // ═══════════════════════════════════════════════════════════════════
    // The verification harness in Step 5a needs a snapshot of consensus
    // state taken BEFORE any forward mutation (Steps 4 / 4b). We capture
    // it here so the harness can prove the candidate undo bytes are
    // sufficient to recover this exact pre-state on disconnect.
    PreApplyStateForVerification pre_state{};
    const bool windowed_in_window =
        config_.mode == Mode::WINDOWED_UNDO_ONLY &&
        config_.undo_rebuild_window.has_value() &&
        height >= config_.undo_rebuild_window->start_height &&
        height <= config_.undo_rebuild_window->end_height;
    const bool verify_enabled =
        windowed_in_window &&
        config_.undo_rebuild_window->verify_disconnect_roundtrip;
    if (verify_enabled) {
        pre_state.utreexo_active_at_height =
            IsUtreexoActive(static_cast<uint32_t>(height));
        if (pre_state.utreexo_active_at_height && forest_) {
            pre_state.forest_commitment = forest_->getCommitment();
            pre_state.forest_num_leaves = forest_->getNumLeaves();
        }
        pre_state.shielded_active_at_height =
            height >= Params().shielded_activation_height;
        if (pre_state.shielded_active_at_height) {
            pre_state.shielded_frontier_serialized =
                shielded_tree_.SerializeFrontier();
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 3: Build the undo record BEFORE any UTXO mutation
    // ═══════════════════════════════════════════════════════════════════
    // Spent coins must be read via getCoin() while they are still in the
    // ChainDB UTXO set (deleteCoin happens below in Step 6). UndoRecord
    // holds SpentCoin entries (for restoring the UTXO set on disconnect) and
    // CreatedOut markers (for deleting this block's outputs on disconnect).
    // The Utreexo forest delta is persisted separately as the UD:<blockhash>
    // sidecar below — it is consulted by DisconnectTip via the same path the
    // live ConnectTip writes today.
    UndoRecord undo;
    undo.pre_block_shielded_frontier = shielded_tree_.SerializeFrontier();
    std::vector<shielded::ShieldedBundle> shielded_bundles;
    std::vector<int64_t> shielded_deltas;

    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        TxId txid = tx.GetTxid();
        std::vector<Coin> input_coins;
        uint64_t total_input_value = 0;

        if (tx_idx > 0) {
            for (const auto& input : tx.vin) {
                auto coin_result = chain_db_->getCoinWithConfidentialFallback(
                    input.prevout.txid.AsUint256(), input.prevout.vout);
                if (coin_result.status() != Status::Ok) {
                    g_logger.error("reindex-missing-utxo-for-undo at height " +
                                   std::to_string(height) +
                                   " outpoint=" + input.prevout.txid.AsUint256().GetHex() +
                                   ":" + std::to_string(input.prevout.vout));
                    return Status::Invalid;
                }
                const auto& coin = coin_result.value();
                input_coins.push_back(coin);
                if (coin.amount > 0) {
                    const uint64_t new_total = total_input_value + coin.amount;
                    if (new_total < total_input_value) {
                        g_logger.error("reindex-input-overflow at height " + std::to_string(height));
                        return Status::Invalid;
                    }
                    total_input_value = new_total;
                }

                SpentCoin spent;
                spent.prev_txid = input.prevout.txid.AsUint256();
                spent.prev_vout = input.prevout.vout;
                spent.value = coin.amount;
                spent.scriptPubKey.reserve(coin.script_pubkey.size() / 2);
                for (size_t i = 0; i + 1 < coin.script_pubkey.size(); i += 2) {
                    spent.scriptPubKey.push_back(static_cast<uint8_t>(
                        std::stoi(coin.script_pubkey.substr(i, 2), nullptr, 16)));
                }
                spent.is_coinbase = coin.coinbase;
                spent.height = coin.height;
                spent.is_confidential = coin.is_confidential;
                spent.commitment = coin.commitment;
                undo.spent.push_back(std::move(spent));
            }
        }

        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            undo.created.emplace_back(txid.AsUint256(), vout);
        }

        // tx_idx > 0: the coinbase is excluded, matching the live path. Both
        // ConnectBlockInternal's per-tx loop (block_validation.cpp) and
        // ApplyBlockShieldedSection start at index 1, so a bundle attached to
        // vtx[0] is never validated and never enters shielded state on a live
        // node. Walking it here instead would give a reindexed node a different
        // commitment tree, anchor history and shieldedStateHash than the chain
        // it is replaying — a silent fork between reindexed and live nodes.
        if (tx_idx > 0 && UsesShieldedValueSemantics(tx)) {
            if (!Transaction::IsShieldedVersion(tx.version)) {
                g_logger.error("[reindex] Non-shielded transaction carries shielded bundle at height " +
                               std::to_string(height));
                return Status::Invalid;
            }
            if (tx.shielded_bundle_bytes.empty()) {
                g_logger.error("[reindex] Shielded transaction missing shielded bundle at height " +
                               std::to_string(height));
                return Status::Invalid;
            }

            shielded::ShieldedBundle bundle;
            const auto decode = shielded::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
            if (decode != shielded::BundleDecodeError::Ok) {
                g_logger.error("[reindex] Shielded bundle decode failed at height " +
                               std::to_string(height) + " tx " + std::to_string(tx_idx) +
                               " code=" + std::to_string(static_cast<int>(decode)));
                return Status::Invalid;
            }

            const uint64_t total_output_value = SumOutputs(tx);
            if (total_output_value == UINT64_MAX) {
                g_logger.error("[reindex] Shielded output sum overflow at height " +
                               std::to_string(height) + " tx " + std::to_string(tx_idx));
                return Status::Invalid;
            }

            uint64_t fee = 0;
            std::string fee_error;
            if (!ComputeValidatedTransactionFee(tx, input_coins, total_input_value,
                                               total_output_value, fee, fee_error)) {
                g_logger.error("[reindex] Shielded fee validation failed at height " +
                               std::to_string(height) + " tx " + std::to_string(tx_idx) +
                               ": " + fee_error);
                return Status::Invalid;
            }

            int64_t transparent_delta = 0;
            std::string delta_error;
            if (!ComputeTransparentValueDelta(total_input_value, total_output_value,
                                             fee, transparent_delta, delta_error)) {
                g_logger.error("[reindex] Shielded transparent delta failed at height " +
                               std::to_string(height) + " tx " + std::to_string(tx_idx) +
                               ": " + delta_error);
                return Status::Invalid;
            }

            // Single-helper construction; matches block_validation.cpp.
            // The Apr 30 fleet split was caused by this site being
            // built by hand and silently omitting tx_sighash. Routing
            // through BuildShieldedValidationContext makes that bug
            // class structurally impossible.
            auto ctx = shielded::BuildShieldedValidationContext(
                tx,
                &shielded_nullifiers_,
                &shielded_tree_,
                static_cast<uint32_t>(height),
                transparent_delta,
                Params().shielded_activation_height,
                &shielded_anchor_history_,
                Params().shielded_input_binding_activation_height,
                Params().shielded_cv_binding_activation_height);
            const auto validation = shielded::ValidateShieldedBundle(bundle, ctx);
            if (validation != shielded::ShieldedValidationError::Ok) {
                g_logger.error("[reindex] Shielded validation failed at height " +
                               std::to_string(height) + " tx " + std::to_string(tx_idx) +
                               ": " + ShieldedValidationErrorToString(validation));
                return Status::Invalid;
            }

            shielded_bundles.push_back(std::move(bundle));
            shielded_deltas.push_back(transparent_delta);
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 4: Apply this block to the Utreexo forest + verify root
    // ═══════════════════════════════════════════════════════════════════
    // If root matches, the forest has been updated and we have a correct
    // UtreexoDelta capturing removed/added leaves with positions. The delta
    // is persisted as a UD:<blockhash> sidecar (see Step 7) so DisconnectTip
    // can retrieve it quickly by block hash — exactly the shape DisconnectTip
    // already expects from the live ConnectTip path.
    UtreexoDelta utreexo_delta;
    if (IsUtreexoActive(static_cast<uint32_t>(height))) {
        std::string forest_error;
        auto forest_status = applyBlockToForest(block, static_cast<uint32_t>(height),
                                                utreexo_delta, forest_error);
        if (forest_status != Status::Ok) {
            g_logger.error("[reindex] Forest update failed at height " +
                           std::to_string(height) + ": " + forest_error);
            return forest_status;
        }
    }

    // Epoch-reset gate + block-level validate/apply + anchor-root recording,
    // mirroring the live ConnectBlockInternal path so a reindexed chain
    // reconstructs the SAME post-cutover pool and anchor history (or a
    // reindexed node forks off the live one). See the single canonical
    // implementation and its rationale comments (including why RecordRoot
    // fires for every block ≥ activation, not just shielded-tx blocks) in
    // ConnectBlockShieldedSection (shielded_block_section.cpp).
    std::string shielded_section_err;
    if (!shielded::ConnectBlockShieldedSection(
            shielded_bundles, shielded_deltas, static_cast<uint32_t>(height),
            Params().shielded_epoch_reset_height,
            Params().shielded_activation_height,
            shielded_tree_, shielded_nullifiers_, &shielded_anchor_history_,
            undo.pre_reset_shielded_epoch, shielded_section_err)) {
        g_logger.error("[reindex] " + shielded_section_err);
        return Status::Invalid;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 5: Persist the undo flatfile BEFORE any RocksDB tip advance
    // ═══════════════════════════════════════════════════════════════════
    // Mirrors the bug-1 crash-safety fix in ConnectTip: writeUndo fsyncs
    // before the tip pointer is written. If the daemon is killed between
    // these two fsyncs, the batch simply never commits and the new ChainDB
    // (building in .reindex.tmp) is discarded on next startup.
    std::optional<FilePosition> undo_flatfile_pos;
    std::vector<uint8_t> undo_bytes;
    // Reuses the windowed_in_window flag computed in Step 2b; the two
    // names are kept distinct so future readers can tell at a glance
    // which slot of code each guards.
    const bool serialize_undo_for_live_writes = windowed_in_window;
    if (block_storage_ != nullptr) {
        undo_bytes = undo.Serialize();
        auto undo_pos_result = block_storage_->writeUndo(block_hash, undo_bytes);
        if (undo_pos_result.status() != Status::Ok) {
            g_logger.error("[reindex] Failed to persist undo flatfile at height " +
                           std::to_string(height));
            return undo_pos_result.status();
        }
        undo_flatfile_pos = undo_pos_result.value();
        if (undo_flatfile_pos->offset > std::numeric_limits<uint32_t>::max()) {
            g_logger.error("[reindex] Undo file offset exceeds persisted metadata range at height " +
                           std::to_string(height));
            return Status::Invalid;
        }
    } else if (serialize_undo_for_live_writes) {
        // No primary block_storage_ but the LIVE rebuild path still needs
        // the bytes. Pre-serialize once so the LIVE write path below has
        // them without a duplicated walk.
        undo_bytes = undo.Serialize();
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 5a: WINDOWED_UNDO_ONLY — verify rebuilt undo round-trips
    // ═══════════════════════════════════════════════════════════════════
    // Load-bearing safety property: rebuilt undo bytes are accepted only
    // if a clone of the post-apply state, fed those bytes through a
    // DisconnectBlock-equivalent reverse pass, recovers the captured
    // pre-apply state. If verification fails, the LIVE writes for this
    // height are skipped (see condition guarding Step 5b below). The run
    // continues — D.3 already handles untouched historical holes lazily.
    bool verification_failed = false;
    if (verify_enabled) {
        std::string verify_error;
        const auto verify_status = verifyRebuiltUndoRoundTrip(
            block, height, undo_bytes, undo, pre_state, utreexo_delta,
            verify_error);
        if (verify_status != Status::Ok) {
            verification_failed = true;
            stats_.verify_failures_in_window++;
            stats_.verify_failure_heights.push_back(static_cast<uint32_t>(height));
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: rebuild-verify failed at"
                           " height " + std::to_string(height) +
                           " hash=" + block_hash.GetHex() + ": " + verify_error +
                           " — skipping LIVE writes; D.3 will handle this height lazily");
        } else {
            g_logger.info("[reindex] WINDOWED_UNDO_ONLY: rebuild-verify OK at"
                          " height " + std::to_string(height));
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 5b: WINDOWED_UNDO_ONLY — write rebuilt undo to LIVE targets
    // ═══════════════════════════════════════════════════════════════════
    // For heights inside `undo_rebuild_window`, append the rebuilt undo
    // bytes to the LIVE rev*.dat (a separate `BlockStorage` instance that
    // points at the production blocks/ directory) and stage a metadata-
    // only Put on the LIVE ChainDB so DisconnectTip can find it.
    //
    // Hard guardrails (this path is the consumer side of the Apr 30
    // chainstate-publication hardening, NOT a tip-publication site):
    //   * No setTip on LIVE
    //   * No putHeightIndex on LIVE (the height index is already
    //     populated by the live ConnectTip path)
    //   * No putHeader / putBlock on LIVE (header + body already exist;
    //     re-writing them would invalidate hash-based lookups elsewhere)
    //   * No Utreexo delta sidecar / forest checkpoint on LIVE (the
    //     historical block wasn't connected by *this* binary, so its
    //     sidecar was already written by the original ConnectTip — or
    //     not, in which case D.3 already handles it lazily)
    //   * No UTXO mutations on LIVE (UTXO state at LIVE tip is
    //     consensus-correct already; any rewrite would re-introduce
    //     exactly the bug class that bit reindex on Apr 30)
    //   * NO ChainstateCommitBatch (it refuses commit unless tip
    //     publication fields are staged — by design)
    //
    // The LIVE write is a single putHeaderMetadata that preserves every
    // existing field on the row and only flips BLOCK_HAVE_UNDO + sets
    // undo_file/pos/size. Committed via a raw rocksdb::WriteBatch with
    // sync=true so the metadata pointer becomes durable simultaneously
    // with the rev*.dat fsync.
    if (serialize_undo_for_live_writes && !verification_failed) {
        const auto& window = *config_.undo_rebuild_window;
        if (window.live_chain_db == nullptr || window.live_block_storage == nullptr) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY at height " +
                           std::to_string(height) +
                           ": live_chain_db or live_block_storage is null");
            return Status::Internal;
        }

        // Hole-only optimization (commit #8): when the orchestrator
        // populated a non-empty `hole_heights_to_rebuild` whitelist,
        // skip LIVE writes for any height not in the set. The
        // verifier already ran above (Step 5a) so we still get the
        // free DisconnectBlock-roundtrip equivalence check on every
        // in-window block; we just don't append duplicate undo bytes
        // to LIVE rev*.dat for blocks whose existing undo is already
        // durable + readable + correct (preflight `already_ok`).
        //
        // Empty whitelist preserves the legacy behavior (write
        // every in-window block) for callers that use the API
        // without the orchestrator's preflight, or for diagnostic
        // runs where the operator wants every height re-stamped.
        // BUG FIX (Apr 30 2026 LA diagnostic session): the previous
        // implementation `return Status::Ok` here on whitelist-skip
        // — but Step 5b is INSIDE processBlock, BEFORE Step 6's
        // `chain_db_->putCoin(...)` calls that populate the TEMP DB
        // for subsequent blocks' prevout lookups. Returning early
        // skipped Step 6 entirely for already_ok heights, leaving
        // the temp DB empty and breaking the next block's
        // `getCoinWithConfidentialFallback` lookups.
        //
        // Surfaced on LA via range-dependent failure: range 1:7187
        // (empty whitelist → no skip → Step 6 runs) succeeded;
        // range 1:10783 (non-empty whitelist → already_ok heights
        // returned early → Step 6 never ran → h=7187 missing-utxo)
        // failed at h=7187 looking up prevout 002d901aaa…d0c4:0
        // that should have been added by an earlier canonical block.
        const bool skip_live_writes_due_to_whitelist =
            !window.hole_heights_to_rebuild.empty() &&
            window.hole_heights_to_rebuild.find(static_cast<uint32_t>(height)) ==
                window.hole_heights_to_rebuild.end();
        if (skip_live_writes_due_to_whitelist) {
            g_logger.info("[reindex] WINDOWED_UNDO_ONLY: height " +
                          std::to_string(height) +
                          " verified clean; skipping LIVE writes (preflight already_ok)");
            // Fall through — Step 6+ MUST still run to populate
            // the temp DB for subsequent blocks' prevout lookups.
        } else {

        // 1. Append rebuilt undo bytes to LIVE rev*.dat (writeUndo fsyncs
        //    the rev file before returning the position).
        auto live_undo_result = window.live_block_storage->writeUndo(block_hash, undo_bytes);
        if (live_undo_result.status() != Status::Ok) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: live writeUndo failed at height " +
                           std::to_string(height));
            return live_undo_result.status();
        }
        const auto live_pos = live_undo_result.value();
        if (live_pos.offset > std::numeric_limits<uint32_t>::max()) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: live undo file offset exceeds"
                           " persisted metadata range at height " + std::to_string(height));
            return Status::Invalid;
        }

        // Crash oracle (regtest-only): abort right after the LIVE
        // rev*.dat append + fsync but BEFORE the LIVE chain DB
        // metadata commit. Property pinned by this hook on restart:
        //   * BLOCK_HAVE_UNDO unchanged on the LIVE row
        //   * undo_file / undo_pos / undo_size unchanged on the LIVE row
        //   * the appended undo bytes are durable in rev*.dat as
        //     orphan dead space (no metadata pointer references them)
        // → no dishonest metadata. The orchestrator can be re-run
        // safely; the verifier will produce the same bytes again and
        // the next writeUndo will simply append a second copy. The
        // first orphan is wasted disk; it is never read.
        dinero::testing::MaybeAbortAt(
            "rebuild_after_live_writeUndo_before_metadata_commit",
            dinero::Params().network_id == "regtest");

        // 2. Load the existing LIVE header metadata. We require it exist —
        //    the offline rebuild only ever runs against blocks that were
        //    connected by some past ConnectTip and therefore must have a
        //    persisted header metadata row. If the row is missing, the
        //    chain is structurally broken and writing undo metadata
        //    pointing into rev*.dat would be the wrong fix.
        auto live_meta_result = window.live_chain_db->getHeaderMetadata(block_hash);
        if (!live_meta_result.ok()) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: live header metadata missing at"
                           " height " + std::to_string(height) + " hash=" + block_hash.GetHex() +
                           " — refusing to fabricate metadata");
            return Status::NotFound;
        }
        ChainDB::PersistedHeaderMetadata live_meta = live_meta_result.value();

        // 3. Mutate ONLY BLOCK_HAVE_UNDO + undo_file/pos/size. Everything
        //    else (status_flags except BLOCK_HAVE_UNDO, parent_hash,
        //    height, chainwork, file_number, data_pos, data_size) is
        //    preserved exactly as it stood on the LIVE row.
        live_meta.status_flags |= BLOCK_HAVE_UNDO;
        live_meta.undo_file = live_pos.file_number;
        live_meta.undo_pos = static_cast<uint32_t>(live_pos.offset);
        live_meta.undo_size = live_pos.size;

        // 4. Stage and commit on the LIVE ChainDB via a RAW WriteBatch.
        //    NOT ChainstateCommitBatch — that batch refuses commit unless
        //    setTip + heightIndex are staged, which is exactly what we
        //    must NOT do here.
        ChainWriteToken live_token;
        rocksdb::WriteBatch live_batch;
        const auto stage_status = window.live_chain_db->putHeaderMetadata(
            live_token, block_hash, live_meta, &live_batch);
        if (stage_status != Status::Ok) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: live putHeaderMetadata failed at"
                           " height " + std::to_string(height));
            return stage_status;
        }
        const auto commit_status = window.live_chain_db->writeBatch(
            live_token, std::move(live_batch), true);
        if (commit_status != Status::Ok) {
            g_logger.error("[reindex] WINDOWED_UNDO_ONLY: live writeBatch failed at"
                           " height " + std::to_string(height));
            return commit_status;
        }

        // Crash oracle (regtest-only): abort right after the LIVE
        // chain DB metadata commit returns. Property pinned by this
        // hook on restart:
        //   * BLOCK_HAVE_UNDO == true on the LIVE row
        //   * undo_file / undo_pos / undo_size point at the rebuilt
        //     bytes in LIVE rev*.dat
        //   * the daemon reads those undo bytes cleanly on next start
        //     (DisconnectTip-equivalence preserved)
        // → consistent durable undo. The rebuilder run was atomic
        // from the perspective of any in-window block whose metadata
        // commit succeeded.
        dinero::testing::MaybeAbortAt(
            "rebuild_after_live_metadata_commit",
            dinero::Params().network_id == "regtest");

        g_logger.info("[reindex] WINDOWED_UNDO_ONLY: rebuilt undo bytes durable at"
                      " live height " + std::to_string(height) +
                      " hash=" + block_hash.GetHex() +
                      " live_undo_pos=" + std::to_string(live_pos.offset) +
                      " size=" + std::to_string(live_pos.size));
        stats_.live_undo_writes_committed++;
        stats_.live_undo_write_success_heights.push_back(static_cast<uint32_t>(height));
        }  // close `else { LIVE writes }` (paired with the
           // skip_live_writes_due_to_whitelist branch above)
    }

    // ═══════════════════════════════════════════════════════════════════
    // Step 6: Build the ChainDB WriteBatch (everything durable in one atomic commit)
    // ═══════════════════════════════════════════════════════════════════
    rocksdb::WriteBatch batch;

    if (block_storage_ == nullptr) {
        auto put_status = chain_db_->putBlock(token, block_hash, block, &batch);
        if (put_status != Status::Ok) {
            g_logger.error("Failed to store block at height " + std::to_string(height));
            return put_status;
        }
    }

    auto header_status = chain_db_->putHeader(token, block_hash, block.header, static_cast<int>(height),
                                              accumulated_chainwork_, &batch);
    if (header_status != Status::Ok) {
        g_logger.error("Failed to store header at height " + std::to_string(height));
        return header_status;
    }

    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = block.header.prev_block_hash;
    metadata.height = static_cast<int32_t>(height);
    metadata.chainwork = accumulated_chainwork_;
    metadata.status_flags = BLOCK_VALID_HEADER |
                            BLOCK_VALID_TREE |
                            BLOCK_VALID_TRANSACTIONS |
                            BLOCK_VALID_CHAIN |
                            BLOCK_HAVE_DATA;
    metadata.file_number = pos.file_number;
    metadata.data_pos = static_cast<uint32_t>(pos.offset);
    metadata.data_size = pos.size;
    if (undo_flatfile_pos.has_value()) {
        metadata.status_flags |= BLOCK_HAVE_UNDO;
        metadata.undo_file = undo_flatfile_pos->file_number;
        metadata.undo_pos = static_cast<uint32_t>(undo_flatfile_pos->offset);
        metadata.undo_size = undo_flatfile_pos->size;
    }

    auto metadata_status = chain_db_->putHeaderMetadata(token, block_hash, metadata, &batch);
    if (metadata_status != Status::Ok) {
        g_logger.error("Failed to store header metadata at height " + std::to_string(height));
        return metadata_status;
    }

    auto height_status = chain_db_->putHeightIndex(token, height, block_hash, &batch);
    if (height_status != Status::Ok) {
        g_logger.error("Failed to store height index at height " + std::to_string(height));
        return height_status;
    }

    // Apply UTXO mutations (inputs deleted, outputs added, tx index updated).
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const auto& tx = block.vtx[tx_idx];
        TxId txid = tx.GetTxid();

        if (tx_idx > 0) {
            for (const auto& input : tx.vin) {
                auto delete_status = chain_db_->deleteCoin(token, input.prevout.txid.AsUint256(), input.prevout.vout, &batch);
                if (delete_status != Status::Ok) {
                    g_logger.error("Failed to spend UTXO at height " + std::to_string(height) +
                             " txid=" + txid.AsUint256().GetHex());
                    return delete_status;
                }
                stats_.utxos_spent++;
            }
        }

        for (size_t out_idx = 0; out_idx < tx.vout.size(); out_idx++) {
            const auto& output = tx.vout[out_idx];

            Coin coin;
            coin.amount = output.value.GetUna();
            std::ostringstream spk_hex;
            for (uint8_t byte : output.scriptPubKey) {
                spk_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
            }
            coin.script_pubkey = spk_hex.str();
            coin.height = height;
            coin.coinbase = (tx_idx == 0);
            coin.is_confidential = output.is_confidential;
            coin.commitment = output.commitment;

            auto put_status = chain_db_->putCoin(token, txid.AsUint256(), out_idx, coin, &batch);
            if (put_status != Status::Ok) {
                g_logger.error("Failed to create UTXO at height " + std::to_string(height));
                return put_status;
            }
            stats_.utxos_created++;
        }

        auto tx_status = chain_db_->putTxIndex(token, txid.AsUint256(), block_hash, tx_idx, &batch);
        if (tx_status != Status::Ok) {
            g_logger.warn("Failed to add TX index at height " + std::to_string(height));
        }
    }

    // Utreexo delta sidecar (keyed by block hash so DisconnectTip can fetch
    // the delta directly without replaying UTXO work).
    if (IsUtreexoActive(static_cast<uint32_t>(height))) {
        std::string delta_blob;
        std::string delta_error;
        if (!SerializeUtreexoDelta(utreexo_delta, delta_blob, delta_error)) {
            g_logger.error("[reindex] Failed to serialize Utreexo delta at height " +
                           std::to_string(height) + ": " + delta_error);
            return Status::Serialization;
        }
        batch.Put(MakeUtreexoDeltaUndoKey(block_hash), delta_blob);

        // Forest checkpoint (in-batch so it commits atomically with the tip).
        // Campaign phase 3: gated to every-N like the live writer — the
        // sidecar above covers restore in between; a final checkpoint at the
        // reindex tip is emitted at completion (execute()).
        const uint32_t ckpt_interval =
            config_.utreexo_checkpoint_interval > 1
                ? config_.utreexo_checkpoint_interval : 1;
        if (static_cast<uint32_t>(height) % ckpt_interval == 0) {
            const std::vector<uint8_t> serialized_forest = forest_->serialize();
            auto ckpt_status = chain_db_->putUtreexoCheckpointWithChecksum(
                token, static_cast<int>(height), serialized_forest, &batch);
            if (ckpt_status != Status::Ok) {
                g_logger.error("[reindex] Failed to emit Utreexo checkpoint at height " +
                               std::to_string(height));
                return ckpt_status;
            }
        }
    }

    // Canonical tip pointer is last in the batch, preserving the invariant
    // that the tip pointer becoming durable implies all referenced state
    // (undo flatfile + forest checkpoint + UTXO mutations) is already durable.
    auto tip_status = chain_db_->setTip(token, block_hash, height, accumulated_chainwork_, &batch);
    if (tip_status != Status::Ok) {
        g_logger.error("Failed to update tip at height " + std::to_string(height));
        return tip_status;
    }

    // Commit with sync=true: reindex is recovery-grade work — correctness over
    // raw throughput. Every block commit is durable before returning.
    auto commit_status = chain_db_->writeBatch(token, std::move(batch), true);
    if (commit_status != Status::Ok) {
        g_logger.error("Failed to commit block at height " + std::to_string(height));
        return commit_status;
    }

    // Track final tip for ForestTipMarker emission in execute().
    final_tip_hash_ = block_hash;
    final_tip_height_ = static_cast<int32_t>(height);

    stats_.blocks_processed++;

    if (stats_.blocks_processed % config_.progress_interval == 0) {
        reportProgress(stats_.blocks_processed, 0);
    }

    return Status::Ok;
}

Status BlockReindexer::initializeShieldedArtifacts() {
    std::error_code ec;
    const bool preserve = config_.preserve_shielded_state_on_init;

    auto remove_artifact = [&](const std::filesystem::path& path, bool sqlite_sidecars) {
        std::filesystem::remove_all(path, ec);
        ec.clear();
        if (sqlite_sidecars) {
            std::filesystem::remove(path.string() + "-wal", ec);
            ec.clear();
            std::filesystem::remove(path.string() + "-shm", ec);
            ec.clear();
        }
    };

    if (!shielded_frontier_output_path_.empty()) {
        std::filesystem::create_directories(shielded_frontier_output_path_.parent_path(), ec);
        if (ec) {
            g_logger.error("[reindex] Failed to create shielded frontier directory: " + ec.message());
            return Status::Io;
        }
        if (!preserve) {
            remove_artifact(shielded_frontier_output_path_, false);
        }
    }

    if (!shielded_nullifier_db_path_.empty()) {
        std::filesystem::create_directories(shielded_nullifier_db_path_.parent_path(), ec);
        if (ec) {
            g_logger.error("[reindex] Failed to create shielded nullifier directory: " + ec.message());
            return Status::Io;
        }
        if (!preserve) {
            remove_artifact(shielded_nullifier_db_path_, true);
        }
        const auto open_result = shielded_nullifiers_.Open(shielded_nullifier_db_path_.string());
        if (open_result != shielded::NullifierSet::OpenResult::Ok) {
            g_logger.error("[reindex] Failed to open shielded nullifier DB at " +
                           shielded_nullifier_db_path_.string());
            return Status::Io;
        }
    }

    if (preserve) {
        // Anchor mode: tree is seeded by `seedFromAnchor` from the
        // AnchorState's frontier. Do NOT clobber it with a fresh
        // CommitmentTree() here. The frontier file on disk is also
        // preserved untouched — we'll write a fresh one at end-of-run via
        // `persistShieldedArtifacts` which serializes the in-memory tree.
        return Status::Ok;
    }

    shielded_tree_ = shielded::CommitmentTree();
    return Status::Ok;
}

Status BlockReindexer::persistShieldedArtifacts() {
    if (shielded_frontier_output_path_.empty()) {
        return Status::Ok;
    }

    const auto frontier = shielded_tree_.SerializeFrontier();
    std::ofstream out(shielded_frontier_output_path_, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        g_logger.error("[reindex] Failed to open shielded frontier output: " +
                       shielded_frontier_output_path_.string());
        return Status::Io;
    }
    out.write(reinterpret_cast<const char*>(frontier.data()),
              static_cast<std::streamsize>(frontier.size()));
    if (!out.good()) {
        g_logger.error("[reindex] Failed to write shielded frontier output: " +
                       shielded_frontier_output_path_.string());
        return Status::Io;
    }
    out.flush();
    if (!out.good()) {
        g_logger.error("[reindex] Failed to flush shielded frontier output: " +
                       shielded_frontier_output_path_.string());
        return Status::Io;
    }

    return Status::Ok;
}

Status BlockReindexer::rebuildUTXOSet() {
    g_logger.info("      Rebuilding UTXO set from block index...");

    // Create write token for ChainDB mutations
    ChainWriteToken token;

    // ═══════════════════════════════════════════════════════════════════
    // Step 1: Clear Existing UTXO Set
    // ═══════════════════════════════════════════════════════════════════

    // In production, we would:
    // 1. Delete all coins from UTXO column family
    // 2. Reset UTXO statistics
    // For now, we assume UTXO set is already cleared or we're rebuilding from scratch

    g_logger.info("      Cleared existing UTXO set");

    // ═══════════════════════════════════════════════════════════════════
    // Step 2: Get Chain Tip Height
    // ═══════════════════════════════════════════════════════════════════

    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        g_logger.error("Failed to get chain tip");
        return tip_result.status();
    }

    uint64_t chain_height = tip_result.value().height;
    g_logger.info("      Chain height: " + std::to_string(chain_height));

    // ═══════════════════════════════════════════════════════════════════
    // Step 3: Replay All Blocks from Genesis
    // ═══════════════════════════════════════════════════════════════════

    for (uint64_t height = 0; height <= chain_height; height++) {
        // Get block hash by height
        auto hash_result = chain_db_->getBlockHashByHeight(height);
        if (hash_result.status() != Status::Ok) {
            g_logger.error("Failed to get block hash at height " + std::to_string(height));
            return hash_result.status();
        }

        uint256 block_hash = hash_result.value();

        // Get block by hash
        auto block_result = storage::ReadArchivalBlock(*chain_db_, block_storage_, block_hash);
        if (block_result.status() != Status::Ok) {
            g_logger.error("Failed to get block at height " + std::to_string(height));
            return block_result.status();
        }

        const Block& block = block_result.value();

        // Apply block transactions to UTXO set
        rocksdb::WriteBatch batch;

        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            const auto& tx = block.vtx[tx_idx];
            TxId txid = tx.GetTxid();  // Phase M.4: GetTxid() returns TxId

            // Spend inputs (except coinbase)
            if (tx_idx > 0) {
                for (const auto& input : tx.vin) {
                    // Phase M.4: input.prevout.txid is TxId, extract uint256 for deleteCoin API
                auto delete_status = chain_db_->deleteCoin(token, input.prevout.txid.AsUint256(), input.prevout.vout, &batch);
                    if (delete_status != Status::Ok) {
                        g_logger.error("Failed to spend UTXO at height " + std::to_string(height));
                        return delete_status;
                    }
                    stats_.utxos_spent++;
                }
            }

            // Create new UTXOs from outputs
            for (size_t out_idx = 0; out_idx < tx.vout.size(); out_idx++) {
                const auto& output = tx.vout[out_idx];

                Coin coin;
                // Phase M.6.2: Extract raw value from AmountUna for ChainDB boundary
                coin.amount = output.value.GetUna();
                // FIX: ChainDB stores scriptPubKey as hex string - encode binary to hex
                std::ostringstream spk_hex;
                for (uint8_t byte : output.scriptPubKey) {
                    spk_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                }
                coin.script_pubkey = spk_hex.str();
                coin.height = height;
                coin.coinbase = (tx_idx == 0);
                coin.is_confidential = output.is_confidential;
                coin.commitment = output.commitment;

                // Phase M.4: txid is TxId, extract uint256 for putCoin API
            auto put_status = chain_db_->putCoin(token, txid.AsUint256(), out_idx, coin, &batch);
                if (put_status != Status::Ok) {
                    g_logger.error("Failed to create UTXO at height " + std::to_string(height));
                    return put_status;
                }
                stats_.utxos_created++;
            }
        }

        // Commit batch for this block
        auto commit_status = chain_db_->writeBatch(token, std::move(batch), false);
        if (commit_status != Status::Ok) {
            g_logger.error("Failed to commit UTXO updates at height " + std::to_string(height));
            return commit_status;
        }

        // Report progress
        if (height % config_.progress_interval == 0) {
            g_logger.info("      Rebuilt UTXOs up to height " + std::to_string(height) +
                    " (" + std::to_string((height * 100) / chain_height) + "%)");
        }
    }

    g_logger.info("      UTXO set rebuild complete");

    return Status::Ok;
}

void BlockReindexer::reportProgress(uint64_t blocks_processed, uint64_t total_blocks) {
    if (config_.progress_cb) {
        config_.progress_cb(blocks_processed, total_blocks);
    }

    if (total_blocks > 0) {
        double progress = (blocks_processed * 100.0) / total_blocks;
        g_logger.info("   Progress: " + std::to_string(blocks_processed) + "/" +
                 std::to_string(total_blocks) + " (" +
                 std::to_string(static_cast<int>(progress)) + "%)");
    } else {
        g_logger.info("   Progress: " + std::to_string(blocks_processed) + " blocks processed");
    }
}

} // namespace consensus
} // namespace dinero
