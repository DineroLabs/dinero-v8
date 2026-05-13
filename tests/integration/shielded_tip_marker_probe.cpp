#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "crypto/sha256.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/transaction.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <json/json.h>
#include <sstream>
#include <sqlite3.h>

namespace {

using dinero::ChainDB;
using dinero::Status;
using dinero::StatusOr;
using dinero::Transaction;
using dinero::consensus::shielded::CommitmentTree;
using dinero::consensus::shielded::NullifierSet;
using dinero::uint256;

struct ShieldedStateSnapshot {
    uint256 root;
    uint64_t tree_size{0};
    uint64_t nullifier_count{0};
};

void PrintUsage() {
    std::cerr << "Usage: shielded_tip_marker_probe --datadir <path>"
                 " [--verify-tip-height <height>]"
                 " [--rewind-active-height <height>]\n";
}

bool ReadUint32Arg(int argc, char** argv, int* index, uint32_t* out) {
    if (!index || !out || *index + 1 >= argc) {
        return false;
    }
    try {
        *out = static_cast<uint32_t>(std::stoul(argv[++(*index)]));
        return true;
    } catch (...) {
        return false;
    }
}

bool LoadShieldedState(const std::filesystem::path& frontier_path,
                       const std::filesystem::path& nullifier_db_path,
                       CommitmentTree* tree,
                       NullifierSet* nullifiers) {
    if (!tree || !nullifiers) {
        return false;
    }

    const auto open_result = nullifiers->Open(nullifier_db_path.string());
    if (open_result != NullifierSet::OpenResult::Ok) {
        return false;
    }

    if (!std::filesystem::exists(frontier_path)) {
        return true;
    }

    std::ifstream in(frontier_path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::vector<uint8_t> frontier((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    if (frontier.empty()) {
        return true;
    }
    return tree->DeserializeFrontier(frontier.data(), frontier.size());
}

bool PersistShieldedState(const std::filesystem::path& frontier_path,
                          const CommitmentTree& tree) {
    const auto frontier = tree.SerializeFrontier();
    std::ofstream out(frontier_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(frontier.data()),
              static_cast<std::streamsize>(frontier.size()));
    return out.good();
}

ShieldedStateSnapshot CurrentShieldedStateSnapshot(const CommitmentTree& tree,
                                                   const NullifierSet& nullifiers) {
    ShieldedStateSnapshot snapshot;
    snapshot.tree_size = tree.Size();
    snapshot.nullifier_count = nullifiers.Size();

    const auto root = tree.Root();
    static_assert(sizeof(snapshot.root.data) == root.size(), "uint256/shielded root size mismatch");
    std::memcpy(snapshot.root.data, root.data(), root.size());
    return snapshot;
}

std::string Sha256Hex(const std::vector<uint8_t>& bytes) {
    uint8_t hash[32];
    dinero::crypto::CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::string HashFrontierFile(const std::filesystem::path& frontier_path) {
    if (!std::filesystem::exists(frontier_path)) {
        return Sha256Hex({});
    }

    std::ifstream in(frontier_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return Sha256Hex(bytes);
}

std::string HashCanonicalNullifierDump(const std::filesystem::path& nullifier_db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(nullifier_db_path.string().c_str(), &db) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return {};
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT hex(nullifier), block_height FROM nullifiers ORDER BY nullifier ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }

    std::vector<uint8_t> canonical_rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* nf = sqlite3_column_text(stmt, 0);
        const auto* nf_str = nf ? reinterpret_cast<const char*>(nf) : "";
        const int height = sqlite3_column_int(stmt, 1);
        const std::string row =
            std::string(nf_str) + ":" + std::to_string(height) + "\n";
        canonical_rows.insert(canonical_rows.end(), row.begin(), row.end());
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Sha256Hex(canonical_rows);
}

bool PersistShieldedTipMarker(ChainDB& chain_db,
                              const CommitmentTree& tree,
                              const NullifierSet& nullifiers,
                              const uint256& tip_hash,
                              uint32_t tip_height) {
    const auto snapshot = CurrentShieldedStateSnapshot(tree, nullifiers);
    dinero::ChainWriteToken token = dinero::ChainWriteToken::CreateForTesting();
    ChainDB::ShieldedTipMarker marker;
    marker.height = static_cast<int32_t>(tip_height);
    marker.block_hash = tip_hash;
    marker.shielded_root = snapshot.root;
    marker.tree_size = snapshot.tree_size;
    marker.nullifier_count = snapshot.nullifier_count;
    return chain_db.putShieldedTipMarker(token, marker) == Status::Ok;
}

bool RangeHasShieldedActivity(ChainDB& chain_db,
                              const dinero::BlockStorage& block_storage,
                              uint32_t start_height,
                              uint32_t end_height) {
    if (start_height > end_height) {
        return false;
    }

    for (uint32_t height = start_height; height <= end_height; ++height) {
        auto hash_result = chain_db.getBlockHashByHeight(static_cast<int>(height));
        if (hash_result.status() != Status::Ok) {
            return true;
        }

        auto block_result = dinero::storage::ReadArchivalBlock(
            chain_db, &block_storage, hash_result.value(),
            dinero::storage::ArchivalReadMode::RequireFlatfiles);
        if (block_result.status() != Status::Ok) {
            return true;
        }

        for (const auto& tx : block_result.value().vtx) {
            if (tx.IsShielded()) {
                return true;
            }
        }
    }

    return false;
}

bool VerifyOrBootstrapShieldedTipMarker(ChainDB& chain_db,
                                        const dinero::BlockStorage& block_storage,
                                        const CommitmentTree& tree,
                                        const NullifierSet& nullifiers,
                                        const uint256& tip_hash,
                                        uint32_t tip_height) {
    const auto snapshot = CurrentShieldedStateSnapshot(tree, nullifiers);
    const auto marker_result = chain_db.getShieldedTipMarker();
    if (marker_result.status() == Status::NotFound) {
        const bool safe_bootstrap =
            tip_height == 0 || !RangeHasShieldedActivity(chain_db, block_storage, 1, tip_height);
        if (!safe_bootstrap) {
            return false;
        }
        return PersistShieldedTipMarker(chain_db, tree, nullifiers, tip_hash, tip_height);
    }
    if (marker_result.status() != Status::Ok) {
        return false;
    }

    const auto& marker = marker_result.value();
    const bool state_matches =
        marker.shielded_root == snapshot.root &&
        marker.tree_size == snapshot.tree_size &&
        marker.nullifier_count == snapshot.nullifier_count;
    if (!state_matches) {
        return false;
    }

    const bool tip_matches =
        marker.block_hash == tip_hash &&
        marker.height == static_cast<int32_t>(tip_height);
    if (tip_matches) {
        return true;
    }

    const uint32_t marker_height = marker.height < 0 ? 0u : static_cast<uint32_t>(marker.height);
    const uint32_t range_start = std::min(marker_height, tip_height) + 1;
    const uint32_t range_end = std::max(marker_height, tip_height);
    if (range_start <= range_end &&
        RangeHasShieldedActivity(chain_db, block_storage, range_start, range_end)) {
        return false;
    }

    return PersistShieldedTipMarker(chain_db, tree, nullifiers, tip_hash, tip_height);
}

StatusOr<dinero::UndoRecord> ReadStoredUndo(ChainDB& chain_db,
                                            const dinero::BlockStorage& block_storage,
                                            const uint256& hash) {
    return dinero::storage::ReadArchivalUndo(
        chain_db, &block_storage, hash, dinero::storage::ArchivalReadMode::RequireFlatfiles);
}

bool RewindShieldedStateToHeight(ChainDB& chain_db,
                                 const dinero::BlockStorage& block_storage,
                                 const std::filesystem::path& frontier_path,
                                 CommitmentTree* tree,
                                 NullifierSet* nullifiers,
                                 const uint256& active_tip_hash,
                                 uint32_t active_height,
                                 uint32_t stored_tip_height) {
    if (!tree || !nullifiers) {
        return false;
    }
    if (active_height >= stored_tip_height) {
        return true;
    }

    std::vector<uint8_t> frontier_at_active_tip;
    if (active_height > 0 || stored_tip_height > 0) {
        const auto next_hash_result =
            chain_db.getBlockHashByHeight(static_cast<int>(active_height + 1));
        if (next_hash_result.status() != Status::Ok) {
            return false;
        }

        const auto undo_result = ReadStoredUndo(chain_db, block_storage, next_hash_result.value());
        if (undo_result.status() != Status::Ok ||
            !undo_result.value().pre_block_shielded_frontier.has_value()) {
            return false;
        }
        frontier_at_active_tip = *undo_result.value().pre_block_shielded_frontier;
    }

    *tree = CommitmentTree();
    if (!frontier_at_active_tip.empty() &&
        !tree->DeserializeFrontier(frontier_at_active_tip.data(),
                                   frontier_at_active_tip.size())) {
        return false;
    }

    nullifiers->RollbackAbove(active_height);
    if (!PersistShieldedState(frontier_path, *tree)) {
        return false;
    }
    return PersistShieldedTipMarker(chain_db, *tree, *nullifiers, active_tip_hash, active_height);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path datadir;
    bool have_verify_height = false;
    uint32_t verify_height = 0;
    bool have_rewind_height = false;
    uint32_t rewind_height = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--datadir") {
            if (i + 1 >= argc) {
                PrintUsage();
                return 2;
            }
            datadir = argv[++i];
        } else if (arg == "--verify-tip-height") {
            if (!ReadUint32Arg(argc, argv, &i, &verify_height)) {
                PrintUsage();
                return 2;
            }
            have_verify_height = true;
        } else if (arg == "--rewind-active-height") {
            if (!ReadUint32Arg(argc, argv, &i, &rewind_height)) {
                PrintUsage();
                return 2;
            }
            have_rewind_height = true;
        } else {
            PrintUsage();
            return 2;
        }
    }

    if (datadir.empty()) {
        PrintUsage();
        return 2;
    }

    ChainDB chain_db;
    const auto chain_db_status = chain_db.init(datadir / "blockchain" / "chaindb");
    if (chain_db_status != Status::Ok) {
        std::cerr << "Failed to open ChainDB: " << static_cast<int>(chain_db_status) << "\n";
        return 1;
    }

    dinero::BlockStorage block_storage;
    const auto block_storage_status = block_storage.init(datadir);
    if (block_storage_status != Status::Ok) {
        std::cerr << "Failed to open BlockStorage: " << static_cast<int>(block_storage_status) << "\n";
        return 1;
    }

    const auto frontier_path = datadir / "blockchain" / "shielded_frontier.bin";
    const auto nullifier_db_path = datadir / "blockchain" / "shielded_nullifiers.db";
    CommitmentTree tree;
    NullifierSet nullifiers;
    if (!LoadShieldedState(frontier_path, nullifier_db_path, &tree, &nullifiers)) {
        std::cerr << "Failed to load shielded state\n";
        return 1;
    }

    if (have_verify_height) {
        const auto hash_result = chain_db.getBlockHashByHeight(static_cast<int>(verify_height));
        if (hash_result.status() != Status::Ok) {
            std::cerr << "Failed to load verify-tip block hash at height " << verify_height << "\n";
            return 1;
        }
        if (!VerifyOrBootstrapShieldedTipMarker(
                chain_db, block_storage, tree, nullifiers, hash_result.value(), verify_height)) {
            std::cerr << "VerifyOrBootstrapShieldedTipMarker failed at height " << verify_height << "\n";
            return 1;
        }
    }

    if (have_rewind_height) {
        const auto hash_result = chain_db.getBlockHashByHeight(static_cast<int>(rewind_height));
        if (hash_result.status() != Status::Ok) {
            std::cerr << "Failed to load rewind block hash at height " << rewind_height << "\n";
            return 1;
        }

        const auto stored_tip = chain_db.getTip();
        if (stored_tip.status() != Status::Ok) {
            std::cerr << "Failed to load ChainDB tip for rewind\n";
            return 1;
        }

        if (!RewindShieldedStateToHeight(chain_db,
                                         block_storage,
                                         frontier_path,
                                         &tree,
                                         &nullifiers,
                                         hash_result.value(),
                                         rewind_height,
                                         static_cast<uint32_t>(stored_tip.value().height))) {
            std::cerr << "RewindShieldedStateToHeight failed\n";
            return 1;
        }
    }

    const auto marker_result = chain_db.getShieldedTipMarker();
    if (marker_result.status() != Status::Ok) {
        std::cerr << "Failed to read ShieldedTipMarker after operation\n";
        return 1;
    }

    const auto tip_result = chain_db.getTip();
    if (tip_result.status() != Status::Ok) {
        std::cerr << "Failed to read ChainDB tip after operation\n";
        return 1;
    }

    const auto snapshot = CurrentShieldedStateSnapshot(tree, nullifiers);
    Json::Value root(Json::objectValue);
    root["chaindb_tip_height"] = tip_result.value().height;
    root["chaindb_tip_hash"] = tip_result.value().hash.GetHex();
    root["current_root"] = snapshot.root.GetHex();
    root["current_tree_size"] = Json::UInt64(snapshot.tree_size);
    root["current_nullifier_count"] = Json::UInt64(snapshot.nullifier_count);
    root["current_frontier_sha256"] = HashFrontierFile(frontier_path);
    root["current_nullifier_dump_sha256"] = HashCanonicalNullifierDump(nullifier_db_path);
    root["marker_height"] = marker_result.value().height;
    root["marker_hash"] = marker_result.value().block_hash.GetHex();
    root["marker_root"] = marker_result.value().shielded_root.GetHex();
    root["marker_tree_size"] = Json::UInt64(marker_result.value().tree_size);
    root["marker_nullifier_count"] = Json::UInt64(marker_result.value().nullifier_count);

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::cout << Json::writeString(builder, root) << std::endl;
    return 0;
}
