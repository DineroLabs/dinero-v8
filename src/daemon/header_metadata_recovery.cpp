#include "daemon/header_metadata_recovery.h"

#include "common/serialization.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "consensus/chainwork.h"
#include "primitives/block.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <rocksdb/write_batch.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dinero::daemon {
namespace {

struct ScannedBlock {
    FilePosition pos;
    uint256 hash;
    Block block;
};

struct FlatfileIndex {
    std::unordered_map<std::string, ScannedBlock> by_hash;
    std::unordered_map<std::string, std::vector<std::string>> children_by_parent;
};

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

uint32_t Fnv1a(const std::string& data) {
    uint32_t hash = 0x811c9dc5;
    for (char byte : data) {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 0x01000193;
    }
    return hash;
}

std::filesystem::path BlockFilePath(const std::filesystem::path& datadir, uint32_t file_number) {
    std::ostringstream oss;
    oss << "blk" << std::setfill('0') << std::setw(5) << file_number << ".dat";
    return datadir / "blocks" / oss.str();
}

Status ScanFlatfiles(
    const std::filesystem::path& datadir,
    uint32_t max_file_number,
    FlatfileIndex& index,
    std::string& error
) {
    const uint32_t expected_magic = Params().magic;

    for (uint32_t file_number = 0; file_number <= max_file_number; ++file_number) {
        const auto path = BlockFilePath(datadir, file_number);
        if (!std::filesystem::exists(path)) {
            continue;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            error = "failed to open " + path.string();
            return Status::Io;
        }

        uint64_t offset = 0;
        while (true) {
            uint32_t magic = 0;
            uint32_t size = 0;

            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            if (in.eof()) {
                break;
            }
            if (!in.good()) {
                error = "failed to read magic from " + path.string();
                return Status::Io;
            }

            in.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!in.good()) {
                error = "failed to read block size from " + path.string();
                return Status::Io;
            }

            if (magic != expected_magic) {
                error = "unexpected block magic in " + path.string() +
                        " offset=" + std::to_string(offset);
                return Status::Corruption;
            }
            if (size == 0 || size > 128U * 1024U * 1024U) {
                error = "invalid block size in " + path.string() +
                        " offset=" + std::to_string(offset) +
                        " size=" + std::to_string(size);
                return Status::Corruption;
            }

            std::string bytes(size, '\0');
            in.read(bytes.data(), size);
            if (!in.good()) {
                error = "failed to read block bytes from " + path.string();
                return Status::Io;
            }

            uint32_t stored_checksum = 0;
            in.read(reinterpret_cast<char*>(&stored_checksum), sizeof(stored_checksum));
            if (!in.good()) {
                error = "failed to read block checksum from " + path.string();
                return Status::Io;
            }
            if (stored_checksum != Fnv1a(bytes)) {
                error = "block checksum mismatch in " + path.string() +
                        " offset=" + std::to_string(offset);
                return Status::Corruption;
            }

            auto parsed = Block::Deserialize(
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            if (!parsed.has_value()) {
                error = "block deserialization failed in " + path.string() +
                        " offset=" + std::to_string(offset);
                return Status::Serialization;
            }

            const auto hash = parsed->GetHash();
            const auto hash_hex = hash.GetHex();
            if (index.by_hash.find(hash_hex) == index.by_hash.end()) {
                ScannedBlock scanned;
                scanned.pos = FilePosition(file_number, offset, size);
                scanned.hash = hash;
                scanned.block = std::move(*parsed);
                index.by_hash.emplace(hash_hex, std::move(scanned));
            }

            auto& children = index.children_by_parent[parsed->header.prev_block_hash.GetHex()];
            if (std::find(children.begin(), children.end(), hash_hex) == children.end()) {
                children.push_back(hash_hex);
            }

            offset += 12ULL + static_cast<uint64_t>(size);
        }
    }

    return Status::Ok;
}

std::optional<arith_uint256> ExistingWork(ChainDB& db, const uint256& hash) {
    auto work = db.getBlockWork(hash);
    if (work.status() == Status::Ok) {
        return work.value();
    }
    auto meta = db.getHeaderMetadata(hash);
    if (meta.status() == Status::Ok) {
        return meta.value().chainwork;
    }
    return std::nullopt;
}

Status WriteManifestIfRequested(const std::filesystem::path& path,
                                const HeaderMetadataRecoveryManifest& manifest) {
    if (path.empty()) {
        return Status::Ok;
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Status::Io;
    }
    out << manifest.ToJson();
    if (!out.good()) {
        return Status::Io;
    }
    return Status::Ok;
}

}  // namespace

const char* HeaderMetadataRecoveryStatusName(HeaderMetadataRecoveryStatus status) {
    switch (status) {
        case HeaderMetadataRecoveryStatus::AlreadyOk: return "already_ok";
        case HeaderMetadataRecoveryStatus::Recoverable: return "recoverable";
        case HeaderMetadataRecoveryStatus::Recovered: return "recovered";
        case HeaderMetadataRecoveryStatus::MissingHeightIndex: return "missing_height_index";
        case HeaderMetadataRecoveryStatus::MissingFlatfileBlock: return "missing_flatfile_block";
        case HeaderMetadataRecoveryStatus::InvalidFlatfileBlock: return "invalid_flatfile_block";
        case HeaderMetadataRecoveryStatus::MissingChainwork: return "missing_chainwork";
        case HeaderMetadataRecoveryStatus::WriteFailed: return "write_failed";
    }
    return "unknown";
}

std::string HeaderMetadataRecoveryManifest::ToJson() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"window_start\": " << window_start << ",\n";
    ss << "  \"window_end\": " << window_end << ",\n";
    ss << "  \"write\": " << (write ? "true" : "false") << ",\n";
    ss << "  \"scanned\": " << scanned << ",\n";
    ss << "  \"already_ok\": " << already_ok << ",\n";
    ss << "  \"recoverable\": " << recoverable << ",\n";
    ss << "  \"recovered\": " << recovered << ",\n";
    ss << "  \"failed\": " << failed << ",\n";
    ss << "  \"final_status\": \"" << JsonEscape(final_status) << "\",\n";
    ss << "  \"entries\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ss << "    {\"height\": " << e.height
           << ", \"status\": \"" << HeaderMetadataRecoveryStatusName(e.status) << "\""
           << ", \"block_hash\": \"" << e.block_hash.GetHex() << "\""
           << ", \"reason\": \"" << JsonEscape(e.reason) << "\"}";
        if (i + 1 != entries.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

StatusOr<HeaderMetadataRecoveryManifest>
RecoverMissingHeaderMetadataRange(const HeaderMetadataRecoveryOptions& opts) {
    if (!opts.live_chain_db || !opts.live_block_storage) {
        return Status::Invalid;
    }
    if (opts.window_start > opts.window_end) {
        return Status::Invalid;
    }
    if (opts.write && !opts.write_token) {
        return Status::Invalid;
    }

    HeaderMetadataRecoveryManifest manifest;
    manifest.window_start = opts.window_start;
    manifest.window_end = opts.window_end;
    manifest.write = opts.write;

    std::unordered_set<std::string> missing_hashes;
    std::unordered_map<uint32_t, uint256> known_hash_by_height;
    std::unordered_map<uint32_t, uint256> resolved_hash_by_height;
    bool has_missing_height_index = false;
    manifest.entries.reserve(opts.window_end - opts.window_start + 1);

    for (uint32_t height = opts.window_start; height <= opts.window_end; ++height) {
        HeaderMetadataRecoveryEntry entry;
        entry.height = height;
        manifest.scanned++;

        auto hash_result = opts.live_chain_db->getBlockHashByHeight(static_cast<int>(height));
        if (hash_result.status() != Status::Ok) {
            entry.status = HeaderMetadataRecoveryStatus::MissingHeightIndex;
            entry.reason = "no canonical height-index entry";
            has_missing_height_index = true;
            manifest.entries.push_back(std::move(entry));
            continue;
        }
        entry.block_hash = hash_result.value();
        known_hash_by_height[height] = entry.block_hash;

        auto metadata_result = opts.live_chain_db->getHeaderMetadata(entry.block_hash);
        if (metadata_result.status() == Status::Ok) {
            entry.status = HeaderMetadataRecoveryStatus::AlreadyOk;
            manifest.already_ok++;
            manifest.entries.push_back(std::move(entry));
            continue;
        }

        entry.status = HeaderMetadataRecoveryStatus::Recoverable;
        entry.reason = "header metadata missing; awaiting flatfile scan";
        missing_hashes.insert(entry.block_hash.GetHex());
        manifest.entries.push_back(std::move(entry));
    }

    std::unordered_map<std::string, ScannedBlock> flatfile_blocks;
    if (!missing_hashes.empty() || has_missing_height_index) {
        auto stats = opts.live_block_storage->getStats();
        if (stats.status() != Status::Ok) {
            return stats.status();
        }
        std::string scan_error;
        FlatfileIndex flatfile_index;
        const auto scan_status = ScanFlatfiles(
            opts.datadir,
            stats.value().current_file_number,
            flatfile_index,
            scan_error);
        if (scan_status != Status::Ok) {
            for (auto& entry : manifest.entries) {
                if (entry.status == HeaderMetadataRecoveryStatus::Recoverable ||
                    entry.status == HeaderMetadataRecoveryStatus::MissingHeightIndex) {
                    entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                    entry.reason = scan_error;
                    manifest.failed++;
                }
            }
            manifest.recoverable = 0;
            manifest.final_status = "scan_failed";
            const auto manifest_status =
                WriteManifestIfRequested(opts.manifest_path_override, manifest);
            if (manifest_status != Status::Ok) {
                return manifest_status;
            }
            return manifest;
        }
        flatfile_blocks = flatfile_index.by_hash;

        auto resolve_hash_at_height = [&](uint32_t height) -> std::optional<uint256> {
            auto resolved = resolved_hash_by_height.find(height);
            if (resolved != resolved_hash_by_height.end()) {
                return resolved->second;
            }
            auto known = known_hash_by_height.find(height);
            if (known != known_hash_by_height.end()) {
                return known->second;
            }
            auto db_hash = opts.live_chain_db->getBlockHashByHeight(static_cast<int>(height));
            if (db_hash.status() == Status::Ok) {
                return db_hash.value();
            }
            return std::nullopt;
        };

        for (auto& entry : manifest.entries) {
            if (entry.status != HeaderMetadataRecoveryStatus::MissingHeightIndex) {
                continue;
            }
            if (entry.height == 0) {
                entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                entry.reason = "cannot recover genesis from missing height index";
                manifest.failed++;
                continue;
            }

            auto parent_hash = resolve_hash_at_height(entry.height - 1);
            if (!parent_hash.has_value()) {
                entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                entry.reason = "cannot identify parent hash for missing height-index row";
                manifest.failed++;
                continue;
            }

            auto children_it = flatfile_index.children_by_parent.find(parent_hash->GetHex());
            if (children_it == flatfile_index.children_by_parent.end() ||
                children_it->second.empty()) {
                entry.status = HeaderMetadataRecoveryStatus::MissingFlatfileBlock;
                entry.reason = "no flatfile child found for missing height-index parent";
                manifest.failed++;
                continue;
            }
            if (children_it->second.size() != 1) {
                entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                entry.reason = "ambiguous flatfile children for missing height-index parent";
                manifest.failed++;
                continue;
            }

            auto child_it = flatfile_blocks.find(children_it->second.front());
            if (child_it == flatfile_blocks.end()) {
                entry.status = HeaderMetadataRecoveryStatus::MissingFlatfileBlock;
                entry.reason = "flatfile child index missing block body";
                manifest.failed++;
                continue;
            }

            entry.block_hash = child_it->second.hash;
            entry.status = HeaderMetadataRecoveryStatus::Recoverable;
            entry.reason = "height index missing; canonical child recovered from flatfile parent chain";
            missing_hashes.insert(entry.block_hash.GetHex());
            resolved_hash_by_height[entry.height] = entry.block_hash;

            auto next_known = resolve_hash_at_height(entry.height + 1);
            if (next_known.has_value()) {
                auto next_header = opts.live_chain_db->getHeader(*next_known);
                if (next_header.status() == Status::Ok &&
                    next_header.value().prev_block_hash != entry.block_hash) {
                    entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                    entry.reason = "recovered child does not anchor to next known header";
                    manifest.failed++;
                    resolved_hash_by_height.erase(entry.height);
                }
            }
        }
    }

    std::unordered_map<uint32_t, arith_uint256> recovered_work_by_height;
    rocksdb::WriteBatch batch;

    auto resolve_hash_at_height = [&](uint32_t height) -> std::optional<uint256> {
        auto resolved = resolved_hash_by_height.find(height);
        if (resolved != resolved_hash_by_height.end()) {
            return resolved->second;
        }
        auto known = known_hash_by_height.find(height);
        if (known != known_hash_by_height.end()) {
            return known->second;
        }
        auto db_hash = opts.live_chain_db->getBlockHashByHeight(static_cast<int>(height));
        if (db_hash.status() == Status::Ok) {
            return db_hash.value();
        }
        return std::nullopt;
    };

    uint64_t write_failed = 0;
    for (auto& entry : manifest.entries) {
        if (entry.status != HeaderMetadataRecoveryStatus::Recoverable) {
            continue;
        }

        const auto hash_hex = entry.block_hash.GetHex();
        auto found_it = flatfile_blocks.find(hash_hex);
        if (found_it == flatfile_blocks.end()) {
            entry.status = HeaderMetadataRecoveryStatus::MissingFlatfileBlock;
            entry.reason = "canonical block body not found in blk*.dat";
            manifest.failed++;
            continue;
        }

        const ScannedBlock& scanned = found_it->second;
        if (scanned.block.GetHash() != entry.block_hash) {
            entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
            entry.reason = "flatfile block hash mismatch";
            manifest.failed++;
            continue;
        }

        if (entry.height > 0) {
            auto parent_hash = resolve_hash_at_height(entry.height - 1);
            if (!parent_hash.has_value() ||
                scanned.block.header.prev_block_hash != *parent_hash) {
                entry.status = HeaderMetadataRecoveryStatus::InvalidFlatfileBlock;
                entry.reason = "block prev hash does not match recovered canonical parent";
                manifest.failed++;
                continue;
            }
        }

        std::optional<arith_uint256> chainwork = ExistingWork(*opts.live_chain_db, entry.block_hash);
        if (!chainwork.has_value()) {
            if (entry.height == 0) {
                chainwork = GetBlockProof(scanned.block.header.difficulty);
            } else {
                auto parent_hash = resolve_hash_at_height(entry.height - 1);
                if (parent_hash.has_value()) {
                    chainwork = ExistingWork(*opts.live_chain_db, *parent_hash);
                    auto recovered_parent = recovered_work_by_height.find(entry.height - 1);
                    if (!chainwork.has_value() &&
                        recovered_parent != recovered_work_by_height.end()) {
                        chainwork = recovered_parent->second;
                    }
                }
                if (chainwork.has_value()) {
                    *chainwork += GetBlockProof(scanned.block.header.difficulty);
                }
            }
        }

        if (!chainwork.has_value()) {
            entry.status = HeaderMetadataRecoveryStatus::MissingChainwork;
            entry.reason = "cannot recover cumulative chainwork from header or parent";
            manifest.failed++;
            continue;
        }

        ChainDB::PersistedHeaderMetadata metadata;
        metadata.parent_hash = scanned.block.header.prev_block_hash;
        metadata.height = static_cast<int32_t>(entry.height);
        metadata.chainwork = *chainwork;
        metadata.status_flags = BLOCK_VALID_HEADER |
                                BLOCK_VALID_TREE |
                                BLOCK_VALID_TRANSACTIONS |
                                BLOCK_VALID_CHAIN |
                                BLOCK_HAVE_DATA;
        metadata.file_number = scanned.pos.file_number;
        metadata.data_pos = static_cast<uint32_t>(scanned.pos.offset);
        metadata.data_size = scanned.pos.size;

        manifest.recoverable++;
        entry.reason = "canonical block body found; metadata can be reconstructed";
        recovered_work_by_height[entry.height] = *chainwork;

        if (!opts.write) {
            continue;
        }

        auto header_status = opts.live_chain_db->putHeader(
            *opts.write_token, entry.block_hash, scanned.block.header,
            static_cast<int>(entry.height), *chainwork, &batch);
        if (header_status != Status::Ok) {
            entry.status = HeaderMetadataRecoveryStatus::WriteFailed;
            entry.reason = "failed to stage header: " + std::string(StatusToString(header_status));
            write_failed++;
            continue;
        }

        auto height_status = opts.live_chain_db->putHeightIndex(
            *opts.write_token, static_cast<int>(entry.height), entry.block_hash, &batch);
        if (height_status != Status::Ok) {
            entry.status = HeaderMetadataRecoveryStatus::WriteFailed;
            entry.reason = "failed to stage height index: " + std::string(StatusToString(height_status));
            write_failed++;
            continue;
        }

        auto meta_status = opts.live_chain_db->putHeaderMetadata(
            *opts.write_token, entry.block_hash, metadata, &batch);
        if (meta_status != Status::Ok) {
            entry.status = HeaderMetadataRecoveryStatus::WriteFailed;
            entry.reason = "failed to stage header metadata: " + std::string(StatusToString(meta_status));
            write_failed++;
            continue;
        }

        entry.status = HeaderMetadataRecoveryStatus::Recovered;
        entry.reason = "header metadata reconstructed from canonical flatfile chain + blk*.dat";
    }

    if (opts.write && write_failed == 0 && manifest.recoverable > 0) {
        auto commit_status = opts.live_chain_db->writeBatch(*opts.write_token, std::move(batch), true);
        if (commit_status != Status::Ok) {
            for (auto& entry : manifest.entries) {
                if (entry.status == HeaderMetadataRecoveryStatus::Recovered) {
                    entry.status = HeaderMetadataRecoveryStatus::WriteFailed;
                    entry.reason = "writeBatch failed: " + std::string(StatusToString(commit_status));
                }
            }
            manifest.failed += manifest.recoverable;
            manifest.recovered = 0;
            manifest.final_status = "write_failed";
            const auto manifest_status =
                WriteManifestIfRequested(opts.manifest_path_override, manifest);
            if (manifest_status != Status::Ok) {
                return manifest_status;
            }
            return manifest;
        }
    }

    if (opts.write) {
        for (const auto& entry : manifest.entries) {
            if (entry.status == HeaderMetadataRecoveryStatus::Recovered) {
                manifest.recovered++;
            }
        }
    }
    if (write_failed > 0) {
        manifest.failed += write_failed;
        for (auto& entry : manifest.entries) {
            if (entry.status == HeaderMetadataRecoveryStatus::Recovered) {
                entry.status = HeaderMetadataRecoveryStatus::WriteFailed;
                entry.reason = "not committed because another recovery row failed to stage";
            }
        }
        manifest.recovered = 0;
    }

    if (manifest.failed > 0) {
        manifest.final_status = "failed";
    } else if (opts.write) {
        manifest.final_status = "ok";
    } else {
        manifest.final_status = "dry_run_complete";
    }

    const auto manifest_status =
        WriteManifestIfRequested(opts.manifest_path_override, manifest);
    if (manifest_status != Status::Ok) {
        return manifest_status;
    }

    return manifest;
}

}  // namespace dinero::daemon
