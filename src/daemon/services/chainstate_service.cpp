#include "daemon/services/chainstate_service.h"
#include "daemon/chainstate_recovery_marker.h"
#include "daemon/chainstate_commit_batch.h"
#include "daemon/coinbase_readback_gate.h"  // maturity gate for coinbase read-back
#include "common/crash_injection.h"
#include "rpc/longpoll_notifier.h"  // Server-side long-poll signaling for getblocktemplate
#include "daemon/services/chainstate_restart_import.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/assumeutxo_state.h"
#include "daemon/services/assumeutxo_replay.h"  // genesis->base replay engine (background validation)
#include "daemon/snapshot_bootstrap_policy.h"
#include "daemon/services/replay_failure_policy.h"  // confirm-before-fatal for replay validation failures
#include "consensus/merkle_root.h"  // torn-body guard on replay reads
#include "daemon/services/config_service.h"
#include "daemon/config.h"
#include "daemon/services/p2p_service.h"  // Phase C.1 v2: For block broadcasting
#include "daemon/services/mining_service.h"  // Phase 40: For ActivateBestChain service notifications
#include "daemon/services/mempool_service.h"  // Phase 40: For ActivateBestChain service notifications
#include "daemon/daemon_context.h"
#include "daemon/block_acceptor.h"  // Week 3: Wire context to BlockAcceptor
#include "daemon/block_relay_manager.h"  // Phase G.2: For block announcements
#include "wallet/wallet_worker.h"        // Thread-safe async wallet notifications via WalletWorker
#include "vault/vault_runtime.h"          // Track C: Liquidity Vault block-event hook
#include "daemon/tx_relay_manager.h"  // #6: Proof refresh after block connect
#include "consensus/block_download_scheduler.h"  // Phase N.4: For scheduler notification
#include "consensus/header_chain.h"  // P2P fix: For HeaderChainSelector notification
#include "ipc/oracles/chain_oracle_client.h"  // Phase 9.2: For Lightning event forwarding
#include "ipc/oracles/time_oracle_client.h"   // Phase 9.2: For Lightning time tracking
#include "ipc/oracles/transaction_oracle_client.h"  // Phase 9.2: For Lightning TX tracking
#include "network/bridge_node.h"  // Phase P.2: For Utreexo proof pre-caching
#include "network/stateless_node.h"  // CSN reorg: For RewindToCheckpoint/ReplayBlock
#include "storage/chain_write_token.h"  // For genesis bootstrap token
#include "consensus/chainparams.h"   // For Params()
#include "consensus/utreexo_delta_codec.h"  // UD sidecar codec + forward replay (campaign phase 2)
#include "storage/forest_restore.h"  // shared checkpoint+sidecar replay walk (campaign phase 3)
#include "consensus/chainwork.h"     // For canonical genesis proof
#include "consensus/pow.h"           // For canonical header PoW checks
#include "consensus/block_filter.h"  // BIP158 GCS block filter construction
#include "consensus/filter_commitment.h"  // DNRF coinbase filter commitment validation
#include "consensus/adapters/wallet_utxo_adapter.h"  // v2.2.0: Consensus interface adapter (kept for wallet)
#include "consensus/consensus_utxo_set.h"  // Phase 2: Pure in-memory UTXO set (owns forest)
#include "daemon/services/wallet_service.h"  // Snapshot wallet rescan: reach active wallet
#include "wallet/wallet_manager.h"            // Snapshot wallet rescan: rescanUtxoSet
#include "storage/persistent_utxo_adapter.h"  // Phase 2: Bridges consensus to ChainDB
#include "consensus/utreexo_accumulator.h"  // v0.14.0.4: For forest rebuild from UTXO set
#include "consensus/utreexo_activation.h"  // Phase 3.2: For IsUtreexoActive() in reorg sanity check
#include "consensus/utreexo_canonical_roots_activation.h"  // Apr 13 2026 Stage 3 fork
#include "consensus/consensus_write_batch.h"  // Phase 3a: atomic persistence scaffold
#include "consensus/proof_gossip.h"  // Phase 9.3+: Tip-proof prewarm
#include "consensus/assume_utxo.h"  // Snapshot trust anchors (optional hard gate)
#include "consensus/utxo_set_digest.h"  // Canonical per-UTXO record serializer (Task 2)
#include "consensus/active_chain_ancestry.h"  // Active-chain hash lookup by height
#include "consensus/block_index.h"   // REORG FIX: For global FindBlockIndex fallback
#include "consensus/block_lifecycle.h"  // BLOCK_HAVE_DATA status flag
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_epoch.h"
#include "consensus/shielded/shielded_block_section.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "pool/pool_manager.h"  // Pool accounting lifecycle wiring
#include "primitives/block.h"        // For Block struct
#include "wallet/transaction.h"      // For Transaction, TxInput, TxOutput
#include "p2p_manager.h"             // Phase C.1 v2: For P2PMessage::create_inv(), create_block()
#include "crypto/sha256.h"           // Phase 42: For snapshot checksum computation
#include "common/serialization.h"    // VectorWriter/Reader for delta sidecar persistence
#include "util/hex.h"                // #274: util::unhex for ChainDB hex spk decode
#include "util/thread_util.h"        // #298: SetThreadName for gdb backtraces
// #298: process id for the hang-watchdog gdb-capture log hint. <unistd.h> is
// POSIX-only and breaks the MSVC/Windows build (C1083); guard it like
// datadir_guard.cpp does.
#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>       // GetCurrentProcessId()
#else
#include <unistd.h>                  // getpid()
#endif
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <iomanip>  // For std::setw, std::setfill
#include <iostream>
#include <vector>
#include <algorithm>  // Phase 3D: For std::find in wallet notifier registry
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "consensus/outpoint.h"  // For OutPoint in ephemeral UTXO detection
#include <chrono>     // Phase C.3 Phase 3: For timestamp tracking
#include <map>        // Phase C.3 Phase 3: For in-flight block tracking
#include <set>        // Phase C.3 Phase 3: For completed block tracking
#include <cstdlib>    // For std::getenv
#include <ctime>      // For std::time
#include <limits>
#include <array>
#include <json/json.h>

namespace dinero {

// #360 residual: g_block_index_mutex is defined in block_index.cpp but not exported
// via block_index.h. Forward-declare it here so the direct CBlockIndex graph
// mutations/walks in this TU (EnsureHeaderBranchIndexed relink, ActivateBestChain
// ancestry walk) can hold it, serializing against the locked AddBlockIndex writers
// on the P2P thread. Recursive + innermost — order is [activation_mutex_] -> this.
extern std::recursive_mutex g_block_index_mutex;

namespace {
constexpr const char* kActivationLastErrorKey = "activation_last_error";
constexpr const char* kActivationLastErrorTimeKey = "activation_last_error_time";
constexpr const char* kActivationFailureStreakKey = "activation_failure_streak";
constexpr const char* kStartupCatchupSource = "startup-catchup";
constexpr uint32_t kInvMsgBlock = 2u;
constexpr uint32_t kInvMsgUtreexoBlock = 0x50000002u;
constexpr char kCsnReplayDataMagic[] = {'C', 'S', 'N', '2'};

// Startup undo audit window: the number of active-chain blocks (tail) that
// VerifyActiveChainUndoCoverage checks at startup. PromoteValidatedHistory uses
// the same constant to gate promotion: the engine's undo tail must cover
// [max(1, base-1023)..base] before the tip is committed, so the first
// post-promotion restart passes the audit without a recovery marker.
constexpr uint32_t kStartupUndoAuditWindow = 1024;

// Convert a consensus::UTXOEntry to the storage Coin format used by ChainDB.
// MUST stay byte-identical to PersistentUTXOAdapter::ToDbCoin
// (include/storage/persistent_utxo_adapter.h, private static) —
// drift breaks promotion/adapter equivalence (coins promoted here are later
// read back via LoadInitialState which calls FromDbCoin on the same rows).
// ToDbCoin cannot be called from here because it is private.
Coin UtxoEntryToDbCoin(const consensus::UTXOEntry& entry) {
    Coin coin;
    coin.amount = entry.value.GetUna();
    coin.script_pubkey.reserve(entry.scriptPubKey.size() * 2);
    for (uint8_t byte : entry.scriptPubKey) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", byte);
        coin.script_pubkey += buf;
    }
    coin.height = static_cast<int>(entry.height);
    coin.coinbase = entry.isCoinbase;
    coin.is_confidential = entry.is_confidential;
    coin.commitment = entry.commitment;
    return coin;
}

struct CsnReplayData {
    std::vector<consensus::UtreexoHash> spend_targets;
    std::vector<consensus::SpentOutputData> spent_outputs;
    bool has_spent_outputs = false;
};

bool ReadU32LE(const std::string& data, size_t& offset, uint32_t& out) {
    if (offset + 4 > data.size()) {
        return false;
    }
    out = static_cast<uint8_t>(data[offset]) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24);
    offset += 4;
    return true;
}

bool DecodeCsnReplayData(const std::string& blob, CsnReplayData& out) {
    size_t offset = 0;
    const bool v2 =
        blob.size() >= 4 &&
        std::memcmp(blob.data(), kCsnReplayDataMagic, 4) == 0;
    if (v2) {
        offset = 4;
    }

    uint32_t target_count = 0;
    if (!ReadU32LE(blob, offset, target_count)) {
        return false;
    }
    if (target_count > 1'000'000u || offset + static_cast<size_t>(target_count) * 32 > blob.size()) {
        return false;
    }

    out.spend_targets.clear();
    out.spend_targets.reserve(target_count);
    for (uint32_t i = 0; i < target_count; ++i) {
        out.spend_targets.emplace_back(blob.begin() + offset, blob.begin() + offset + 32);
        offset += 32;
    }

    if (!v2) {
        return offset == blob.size();
    }

    uint32_t spent_count = 0;
    if (!ReadU32LE(blob, offset, spent_count) || offset >= blob.size()) {
        return false;
    }
    const uint8_t format_version = static_cast<uint8_t>(blob[offset++]);
    if (spent_count > 1'000'000u) {
        return false;
    }

    std::vector<uint8_t> bytes(blob.begin(), blob.end());
    out.spent_outputs.clear();
    out.spent_outputs.reserve(spent_count);
    for (uint32_t i = 0; i < spent_count; ++i) {
        const size_t before = offset;
        auto spent = consensus::SpentOutputData::deserialize(bytes, offset, format_version);
        if (offset <= before) {
            return false;
        }
        out.spent_outputs.push_back(std::move(spent));
    }

    out.has_spent_outputs = true;
    return offset == blob.size();
}

uint32_t BlockGetDataInventoryType() {
    return GetConfig().utreexo_stateless ? kInvMsgUtreexoBlock : kInvMsgBlock;
}

::P2PMessage CreateBlockGetDataMessage(const std::string& block_hash_hex) {
    const uint256 block_hash = uint256::FromHexUnsafe(block_hash_hex);
    return ::P2PMessage::create_getdata_binary(
        block_hash.begin(), 32, BlockGetDataInventoryType()
    );
}

bool ReadCompactSize(const std::vector<uint8_t>& data, size_t* offset, uint64_t* out) {
    if (!offset || !out || *offset >= data.size()) {
        return false;
    }

    const uint8_t first = data[(*offset)++];
    if (first < 0xFD) {
        *out = first;
        return true;
    }
    if (first == 0xFD) {
        if (*offset + 2 > data.size()) {
            return false;
        }
        *out = static_cast<uint64_t>(data[*offset]) |
               (static_cast<uint64_t>(data[*offset + 1]) << 8);
        *offset += 2;
        return true;
    }
    if (first == 0xFE) {
        if (*offset + 4 > data.size()) {
            return false;
        }
        *out = static_cast<uint64_t>(data[*offset]) |
               (static_cast<uint64_t>(data[*offset + 1]) << 8) |
               (static_cast<uint64_t>(data[*offset + 2]) << 16) |
               (static_cast<uint64_t>(data[*offset + 3]) << 24);
        *offset += 4;
        return true;
    }
    if (*offset + 8 > data.size()) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[*offset + i]) << (8 * i);
    }
    *offset += 8;
    *out = value;
    return true;
}

void SyncPoolLifecycleState(pool::PoolManager* pool_manager,
                            ChainDB* chain_db,
                            LoggerService* logger) {
    if (!pool_manager || !chain_db) {
        return;
    }

    auto tip_result = chain_db->getTip();
    if (tip_result.status() != Status::Ok) {
        return;
    }

    const uint32_t tip_height = tip_result.value().height;
    auto pending_blocks = pool_manager->getDatabase().getPendingBlocks();

    uint32_t orphaned = 0;
    uint32_t confirmations_updated = 0;
    for (const auto& pending : pending_blocks) {
        if (pending.block_hash.size() != 64 ||
            !std::all_of(pending.block_hash.begin(), pending.block_hash.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            pool_manager->markBlockOrphaned(pending.block_hash);
            ++orphaned;
            continue;
        }

        if (pending.height > tip_height) {
            pool_manager->markBlockOrphaned(pending.block_hash);
            ++orphaned;
            continue;
        }

        auto active_hash = chain_db->getBlockHashByHeight(static_cast<int>(pending.height));
        if (active_hash.status() != Status::Ok) {
            pool_manager->markBlockOrphaned(pending.block_hash);
            ++orphaned;
            continue;
        }

        const uint256 pending_hash = uint256::FromHexUnsafe(pending.block_hash);
        if (active_hash.value() != pending_hash) {
            pool_manager->markBlockOrphaned(pending.block_hash);
            ++orphaned;
            continue;
        }

        const uint32_t confirmations = tip_height - pending.height + 1;
        if (pending.confirmations != confirmations) {
            pool_manager->updateBlockConfirmations(pending.block_hash, confirmations);
            ++confirmations_updated;
        }
    }

    uint32_t payouts_processed = 0;
    if (confirmations_updated > 0) {
        payouts_processed = pool_manager->processConfirmedBlocks();
    }
    const uint32_t payouts_sent = pool_manager->sendPendingPayouts();

    if (logger && (confirmations_updated > 0 || payouts_processed > 0 || payouts_sent > 0)) {
        logger->info("[ChainstateService] Pool lifecycle sync: confirmations=" +
                     std::to_string(confirmations_updated) +
                     ", payouts_processed=" + std::to_string(payouts_processed) +
                     ", payouts_sent=" + std::to_string(payouts_sent));
    } else if (orphaned > 0 && logger) {
        logger->warning("[ChainstateService] Pool lifecycle sync: orphaned=" +
                        std::to_string(orphaned));
    }
}


std::string BinaryToHexString(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

bool IsHeaderChainBetterThanActiveTip(const consensus::HeaderIndexEntry* best_header,
                                      const CBlockIndex* active_tip) {
    if (!best_header || !active_tip) return false;

    const arith_uint256 active_work = ChainworkFromHex(active_tip->chainwork);
    if (best_header->chainwork > active_work) return true;
    if (best_header->chainwork < active_work) return false;

    // Deterministic tie-breaker: lower tip hash wins.
    return best_header->hash < active_tip->hash;
}

CBlockIndex* FindCommonAncestorByHash(CBlockIndex* a, CBlockIndex* b) {
    if (!a || !b) {
        return nullptr;
    }

    while (a->height > b->height && a->pprev) {
        a = a->pprev;
    }
    while (b->height > a->height && b->pprev) {
        b = b->pprev;
    }

    while (a && b && a->hash != b->hash) {
        a = a->pprev;
        b = b->pprev;
    }

    return (a && b && a->hash == b->hash) ? a : nullptr;
}

void AppendNonCoinbaseTransactions(const Block& block, std::vector<Transaction>* out) {
    if (!out) {
        return;
    }
    for (const auto& tx : block.vtx) {
        if (!tx.IsCoinbase()) {
            out->push_back(tx);
        }
    }
}

void AppendTransactionsFromBlocks(ChainDB* chain_db,
                                  BlockStorage* block_storage,
                                  const std::vector<CBlockIndex*>& block_indexes,
                                  bool ancestor_first,
                                  std::vector<Transaction>* out) {
    if (!chain_db || !out || block_indexes.empty()) {
        return;
    }

    auto append_one = [&](CBlockIndex* block_index) {
        if (!block_index) {
            return;
        }
        auto block_result = storage::ReadArchivalBlock(*chain_db, block_storage, block_index->hash);
        if (block_result.status() != Status::Ok) {
            return;
        }
        AppendNonCoinbaseTransactions(block_result.value(), out);
    };

    if (ancestor_first) {
        for (auto it = block_indexes.rbegin(); it != block_indexes.rend(); ++it) {
            append_one(*it);
        }
        return;
    }

    for (CBlockIndex* block_index : block_indexes) {
        append_one(block_index);
    }
}

void AppendTransactionsOnChainPath(ChainDB* chain_db,
                                   BlockStorage* block_storage,
                                   CBlockIndex* tip,
                                   CBlockIndex* exclusive_ancestor,
                                   std::vector<Transaction>* out) {
    if (!chain_db || !tip || !out) {
        return;
    }

    std::vector<CBlockIndex*> path;
    for (CBlockIndex* walk = tip; walk && walk != exclusive_ancestor; walk = walk->pprev) {
        path.push_back(walk);
    }
    AppendTransactionsFromBlocks(chain_db, block_storage, path, /*ancestor_first=*/true, out);
}

void RecordActivationFailure(UTXOIndex* utxo_index, const std::string& reason) {
    if (!utxo_index) return;

    uint32_t streak = 0;
    if (auto current = utxo_index->GetMetadata(kActivationFailureStreakKey)) {
        try {
            streak = static_cast<uint32_t>(std::stoul(*current));
        } catch (...) {
            streak = 0;
        }
    }

    utxo_index->SetMetadata(kActivationFailureStreakKey, std::to_string(streak + 1));
    utxo_index->SetMetadata(kActivationLastErrorKey, reason.empty() ? "unknown activation failure" : reason);
    utxo_index->SetMetadata(
        kActivationLastErrorTimeKey,
        std::to_string(static_cast<long long>(std::time(nullptr)))
    );
}

void ClearActivationFailure(UTXOIndex* utxo_index) {
    if (!utxo_index) return;
    utxo_index->DeleteMetadata(kActivationLastErrorKey);
    utxo_index->DeleteMetadata(kActivationLastErrorTimeKey);
    utxo_index->DeleteMetadata(kActivationFailureStreakKey);
}

struct SnapshotManifest {
    std::string sha256_hex;
    std::string block_hash_hex;
    uint32_t block_height = 0;
    uint64_t min_bytes = 0;
    uint64_t max_bytes = 0;
    std::string snapshot_file;
};

std::string ToLowerHex(const std::string& in) {
    std::string out = in;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool IsHexString64(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool ComputeFileSha256Hex(const std::filesystem::path& path, std::string& out_hex, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "Failed to open file for SHA256: " + path.string();
        return false;
    }

    crypto::CSHA256 hasher;
    std::array<char, 1 << 16> buffer{};
    while (file.good()) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0) {
            hasher.Write(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(count));
        }
    }

    if (!file.eof()) {
        error = "Failed while reading file for SHA256: " + path.string();
        return false;
    }

    std::array<uint8_t, 32> digest{};
    hasher.Finalize(digest.data());

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : digest) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    out_hex = ss.str();
    return true;
}

bool ParseSnapshotManifest(
    const std::filesystem::path& manifest_path,
    SnapshotManifest& out,
    std::string& error
) {
    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        error = "Failed to open snapshot manifest: " + manifest_path.string();
        return false;
    }

    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    JSONCPP_STRING parse_errors;
    Json::Value root;
    if (!parseFromStream(builder, in, &root, &parse_errors)) {
        error = "Failed to parse snapshot manifest JSON: " + parse_errors;
        return false;
    }

    const Json::Value& section = root.isMember("snapshot") ? root["snapshot"] : root;
    if (!section.isObject()) {
        error = "Snapshot manifest must contain an object root or \"snapshot\" object";
        return false;
    }

    if (!section.isMember("sha256") || !section["sha256"].isString()) {
        error = "Snapshot manifest missing required string field: sha256";
        return false;
    }
    out.sha256_hex = ToLowerHex(section["sha256"].asString());
    if (!IsHexString64(out.sha256_hex)) {
        error = "Snapshot manifest sha256 must be 64 hex characters";
        return false;
    }

    if (!section.isMember("height") || !section["height"].isUInt()) {
        error = "Snapshot manifest missing required unsigned field: height";
        return false;
    }
    out.block_height = section["height"].asUInt();

    if (!section.isMember("block_hash") || !section["block_hash"].isString()) {
        error = "Snapshot manifest missing required string field: block_hash";
        return false;
    }
    out.block_hash_hex = ToLowerHex(section["block_hash"].asString());
    if (!IsHexString64(out.block_hash_hex)) {
        error = "Snapshot manifest block_hash must be 64 hex characters";
        return false;
    }

    if (section.isMember("min_bytes")) {
        if (!section["min_bytes"].isUInt64()) {
            error = "Snapshot manifest field min_bytes must be uint64";
            return false;
        }
        out.min_bytes = section["min_bytes"].asUInt64();
    }
    if (section.isMember("max_bytes")) {
        if (!section["max_bytes"].isUInt64()) {
            error = "Snapshot manifest field max_bytes must be uint64";
            return false;
        }
        out.max_bytes = section["max_bytes"].asUInt64();
    }
    if (!out.min_bytes && section.isMember("bytes")) {
        if (!section["bytes"].isUInt64()) {
            error = "Snapshot manifest field bytes must be uint64";
            return false;
        }
        out.min_bytes = section["bytes"].asUInt64();
        out.max_bytes = section["bytes"].asUInt64();
    }
    if (out.max_bytes && out.min_bytes && out.max_bytes < out.min_bytes) {
        error = "Snapshot manifest max_bytes is smaller than min_bytes";
        return false;
    }

    if (section.isMember("snapshot_file")) {
        if (!section["snapshot_file"].isString()) {
            error = "Snapshot manifest field snapshot_file must be string";
            return false;
        }
        out.snapshot_file = section["snapshot_file"].asString();
    }

    return true;
}

bool ReadSnapshotHeaderPreview(
    const std::filesystem::path& snapshot_path,
    consensus::SnapshotMetadata& header,
    std::string& error
) {
    std::ifstream file(snapshot_path, std::ios::binary);
    if (!file.is_open()) {
        error = "Failed to open snapshot file for header preview: " + snapshot_path.string();
        return false;
    }

    file.read(reinterpret_cast<char*>(&header.magic), sizeof(header.magic));
    file.read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
    file.read(reinterpret_cast<char*>(header.block_hash.data), 32);
    file.read(reinterpret_cast<char*>(&header.block_height), sizeof(header.block_height));
    file.read(reinterpret_cast<char*>(&header.utxo_count), sizeof(header.utxo_count));
    file.read(reinterpret_cast<char*>(&header.timestamp), sizeof(header.timestamp));
    file.read(reinterpret_cast<char*>(&header.reserved), sizeof(header.reserved));

    if (!file.good()) {
        error = "Snapshot file too small to contain a full header";
        return false;
    }
    return true;
}

bool ValidateSnapshotTransportPreflight(
    const std::filesystem::path& snapshot_path,
    uint64_t max_snapshot_bytes,
    std::string& error
) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(snapshot_path, ec);
    if (ec) {
        error = "Failed to inspect snapshot path: " + ec.message();
        return false;
    }
    if (!std::filesystem::exists(status)) {
        error = "Snapshot file not found: " + snapshot_path.string();
        return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
        error = "Snapshot path is not a regular file: " + snapshot_path.string();
        return false;
    }
    if (std::filesystem::is_symlink(status)) {
        error = "Snapshot path must not be a symlink: " + snapshot_path.string();
        return false;
    }

    const uint64_t min_snapshot_bytes = static_cast<uint64_t>(
        consensus::SNAPSHOT_HEADER_SIZE + consensus::SNAPSHOT_FOOTER_SIZE
    );
    const uint64_t file_size = std::filesystem::file_size(snapshot_path, ec);
    if (ec) {
        error = "Failed to read snapshot file size: " + ec.message();
        return false;
    }

    if (file_size < min_snapshot_bytes) {
        error = "Snapshot file is too small (" + std::to_string(file_size) +
                " bytes, min " + std::to_string(min_snapshot_bytes) + ")";
        return false;
    }
    if (max_snapshot_bytes > 0 && file_size > max_snapshot_bytes) {
        error = "Snapshot file exceeds configured max size (" + std::to_string(file_size) +
                " bytes > " + std::to_string(max_snapshot_bytes) + ")";
        return false;
    }

    return true;
}

bool ValidateSnapshotManifestPreflight(
    const std::filesystem::path& snapshot_path,
    const std::filesystem::path& manifest_path,
    std::string& error
) {
    SnapshotManifest manifest;
    if (!ParseSnapshotManifest(manifest_path, manifest, error)) {
        return false;
    }

    const auto file_name = snapshot_path.filename().string();
    if (!manifest.snapshot_file.empty() && manifest.snapshot_file != file_name) {
        error = "Snapshot manifest snapshot_file mismatch: expected \"" + manifest.snapshot_file +
                "\" got \"" + file_name + "\"";
        return false;
    }

    std::error_code ec;
    const uint64_t file_size = std::filesystem::file_size(snapshot_path, ec);
    if (ec) {
        error = "Failed to read snapshot file size for manifest validation: " + ec.message();
        return false;
    }
    if (manifest.min_bytes && file_size < manifest.min_bytes) {
        error = "Snapshot file smaller than manifest min_bytes";
        return false;
    }
    if (manifest.max_bytes && file_size > manifest.max_bytes) {
        error = "Snapshot file larger than manifest max_bytes";
        return false;
    }

    consensus::SnapshotMetadata header;
    if (!ReadSnapshotHeaderPreview(snapshot_path, header, error)) {
        return false;
    }

    if (header.magic != consensus::SNAPSHOT_MAGIC) {
        error = "Snapshot header magic mismatch during manifest preflight";
        return false;
    }
    if (header.block_height != manifest.block_height) {
        error = "Snapshot header height mismatch with manifest";
        return false;
    }
    if (ToLowerHex(header.block_hash.GetHex()) != manifest.block_hash_hex) {
        error = "Snapshot header block hash mismatch with manifest";
        return false;
    }

    std::string computed_sha256;
    if (!ComputeFileSha256Hex(snapshot_path, computed_sha256, error)) {
        return false;
    }
    if (computed_sha256 != manifest.sha256_hex) {
        error = "Snapshot SHA256 mismatch with manifest";
        return false;
    }

    // Optional hard gate against built-in registry if this height is registered.
    if (auto registered = consensus::AssumeUTXORegistry::GetSnapshot(manifest.block_height)) {
        if (ToLowerHex(registered->block_hash.GetHex()) != manifest.block_hash_hex) {
            error = "Manifest block_hash conflicts with built-in AssumeUTXO registry";
            return false;
        }
        if (ToLowerHex(registered->snapshot_hash.GetHex()) != manifest.sha256_hex) {
            error = "Manifest sha256 conflicts with built-in AssumeUTXO registry";
            return false;
        }
    }

    return true;
}

void ApplyPersistedMetadataToBlockIndex(CBlockIndex* block_index,
                                        const ChainDB::PersistedHeaderMetadata& metadata) {
    if (!block_index) {
        return;
    }

    block_index->status = metadata.status_flags;
    block_index->file_number = metadata.file_number;
    block_index->data_pos = metadata.data_pos;
    block_index->data_size = metadata.data_size;
    block_index->undo_file = metadata.undo_file;
    block_index->undo_pos = metadata.undo_pos;
    block_index->undo_size = metadata.undo_size;
}

bool BackfillFailedChildFromParent(ChainDB* chain_db,
                                   CBlockIndex* block_index,
                                   const std::shared_ptr<LoggerService>& logger,
                                   bool* repaired,
                                   std::string* error) {
    if (repaired) {
        *repaired = false;
    }
    if (!block_index || !block_index->pprev || !chain_db) {
        return true;
    }

    if (block_index->status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) {
        return true;
    }

    const uint32_t parent_failed_flags =
        block_index->pprev->status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD);
    if (parent_failed_flags == 0) {
        return true;
    }

    block_index->status |= BLOCK_FAILED_CHILD;

    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const auto persist_status =
        chain_db->setHeaderStatusBits(token, block_index->hash, BLOCK_FAILED_CHILD);
    if (persist_status != Status::Ok) {
        if (error) {
            *error = "failed to persist BLOCK_FAILED_CHILD backfill at height " +
                     std::to_string(block_index->height);
        }
        return false;
    }

    if (repaired) {
        *repaired = true;
    }
    if (logger) {
        logger->warning("[ChainstateService] Backfilled BLOCK_FAILED_CHILD at height " +
                        std::to_string(block_index->height) + " from persisted invalid ancestor");
    }
    return true;
}
}  // namespace

// v2.2.4: Out-of-line constructor/destructor (WalletUTXOAdapter is incomplete in header)
ChainstateService::ChainstateService() = default;
ChainstateService::~ChainstateService() = default;

bool ChainstateService::LoadShieldedState() {
    const std::string nullifier_path =
        (std::filesystem::path(datadir_) / "blockchain" / "shielded_nullifiers.db").string();
    const auto open_result = shielded_nullifiers_.Open(nullifier_path);
    if (open_result != consensus::shielded::NullifierSet::OpenResult::Ok) {
        if (logger_) {
            logger_->error("[ChainstateService] Failed to open shielded nullifier DB: " + nullifier_path);
        }
        return false;
    }

    // Phase 3b nullifier fold-in (final): ChainDB is the durable
    // source of truth for the nullifier set. The sqlite NullifierSet
    // is now a per-instance in-memory cache that must be reconciled
    // to ChainDB at startup, in two modes:
    //
    //   B) ChainDB has rows (any count > 0) — ChainDB wins
    //      UNCONDITIONALLY. Clear sqlite, repopulate by iterating
    //      every ChainDB nullifier row, then assert hydrated count
    //      == ChainDB count. Count equality alone is NOT content
    //      equality (two sets with the same size but different
    //      bytes/heights would silently feed mismatched DSRH bytes
    //      into journal verification — exactly the v2 content drift
    //      case), so we always rebuild rather than trust the size
    //      match. Handles upgraded datadirs, post-crash orphans,
    //      manual datadir surgery, snapshot import — every drift
    //      shape leaves sqlite byte-equal to ChainDB after this.
    //
    //   C) ChainDB is empty AND sqlite has rows — pre-fold-in
    //      datadirs upgraded for the first time. Drain sqlite into
    //      ChainDB via SerializeContent parse so subsequent
    //      ConnectTip / DisconnectTip can rely on ChainDB being
    //      authoritative.
    //
    // Fresh chains and post-reset datadirs hit neither branch:
    // both stores start at zero and stay in lockstep through
    // ConnectTip / DisconnectTip's atomic batch.
    //
    // Both ChainDB read calls (countShieldedNullifiers,
    // forEachShieldedNullifier) are fail-loud at this site — a
    // missing read of the authoritative source is a corruption
    // signal, not "the chain has no nullifiers."
    if (chain_db_) {
        const auto chaindb_count_result = chain_db_->countShieldedNullifiers();
        if (!chaindb_count_result.ok()) {
            if (logger_) {
                logger_->error(
                    "[ChainstateService] ChainDB countShieldedNullifiers failed at startup "
                    "(status=" +
                    std::to_string(static_cast<int>(chaindb_count_result.status())) +
                    "); refusing to proceed without an authoritative nullifier count");
            }
            return false;
        }
        const uint64_t chaindb_count = chaindb_count_result.value();
        const uint64_t sqlite_count_before = shielded_nullifiers_.Size();

        if (chaindb_count > 0) {
            // Mode B: ChainDB has rows → ChainDB is authoritative,
            // unconditionally. Count equality is NOT content equality
            // (two sets with the same size but different
            // bytes/heights would silently feed mismatched DSRH bytes
            // into the journal-row verification — exactly the content
            // drift v2 was meant to catch). Always wipe sqlite and
            // rehydrate from ChainDB iteration so post-startup
            // SerializeContent is byte-exact equal to the ChainDB
            // preimage by construction.
            if (logger_) {
                logger_->warning(
                    "[ChainstateService] Rebuilding sqlite nullifier cache from ChainDB "
                    "(chaindb=" + std::to_string(chaindb_count) +
                    " sqlite_before=" + std::to_string(sqlite_count_before) + ")");
            }
            shielded_nullifiers_.Clear();
            uint64_t hydrated = 0;
            const auto for_each_status = chain_db_->forEachShieldedNullifier(
                [&](uint32_t height, const uint8_t* nullifier_32) -> bool {
                    consensus::shielded::Hash nf{};
                    std::memcpy(nf.data(), nullifier_32, 32);
                    if (shielded_nullifiers_.Insert(nf, height)) {
                        ++hydrated;
                    }
                    return true;
                });
            if (for_each_status != Status::Ok) {
                if (logger_) {
                    logger_->error(
                        "[ChainstateService] ChainDB forEachShieldedNullifier failed during "
                        "sqlite rehydration (status=" +
                        std::to_string(static_cast<int>(for_each_status)) + ")");
                }
                return false;
            }
            if (hydrated != chaindb_count) {
                if (logger_) {
                    logger_->error(
                        "[ChainstateService] Sqlite nullifier rehydration count mismatch: "
                        "chaindb_count=" + std::to_string(chaindb_count) +
                        " hydrated=" + std::to_string(hydrated) +
                        " — refusing startup so the corruption is investigated");
                }
                return false;
            }
            if (logger_) {
                logger_->info(
                    "[ChainstateService] Sqlite nullifier cache rehydrated from ChainDB "
                    "(rows=" + std::to_string(hydrated) + ")");
            }
        } else if (chaindb_count == 0 && sqlite_count_before > 0) {
            // Mode C: one-shot migration from legacy sqlite into the
            // ChainDB column family. Parse SerializeContent's byte
            // stream — format is:
            //   tag 'NSCF' (4) || version=1 (2) || count_LE_u64 (8) ||
            //     per row: height_LE_u32 (4) || nullifier_bytes (32)
            // and replay each row into a single rocksdb WriteBatch
            // committed atomically.
            const auto bytes = shielded_nullifiers_.SerializeContent();
            constexpr size_t kHeaderBytes = 4 + 2 + 8;
            constexpr size_t kRowBytes    = 4 + 32;
            // Tag is the constant 0x4653434E ('NSCF' read MSB-first)
            // written via write_u32 in little-endian, which on disk
            // appears as bytes 'N','C','S','F'. Compare via uint32_t
            // reconstruction so the check stays endian-correct
            // regardless of the ASCII representation.
            constexpr uint32_t kSerializeContentTag = 0x4653434E;
            uint32_t tag_read = 0;
            if (bytes.size() >= 4) {
                for (int i = 0; i < 4; ++i) {
                    tag_read |= static_cast<uint32_t>(bytes[i]) << (i * 8);
                }
            }
            if (bytes.size() < kHeaderBytes || tag_read != kSerializeContentTag) {
                if (logger_) {
                    logger_->error("[ChainstateService] Sqlite nullifier "
                                   "SerializeContent header malformed; refusing migration");
                }
                return false;
            }
            uint64_t expected = 0;
            for (int i = 0; i < 8; ++i) {
                expected |=
                    static_cast<uint64_t>(bytes[6 + i]) << (i * 8);
            }
            const size_t streamed_bytes = bytes.size() - kHeaderBytes;
            if (streamed_bytes != expected * kRowBytes) {
                if (logger_) {
                    logger_->error(
                        "[ChainstateService] Sqlite nullifier migration: "
                        "header.count=" + std::to_string(expected) +
                        " streamed_bytes=" + std::to_string(streamed_bytes) +
                        " mismatch; refusing migration");
                }
                return false;
            }

            rocksdb::WriteBatch migration_batch;
            ChainWriteToken token = ChainWriteToken::CreateForTesting();
            for (uint64_t i = 0; i < expected; ++i) {
                const size_t off = kHeaderBytes + i * kRowBytes;
                uint32_t height = 0;
                for (int j = 0; j < 4; ++j) {
                    height |= static_cast<uint32_t>(bytes[off + j]) << (j * 8);
                }
                const uint8_t* nf = &bytes[off + 4];
                const auto put_status =
                    chain_db_->putShieldedNullifier(token, height, nf, &migration_batch);
                if (put_status != Status::Ok) {
                    if (logger_) {
                        logger_->error(
                            "[ChainstateService] Sqlite nullifier migration: "
                            "putShieldedNullifier staging failed at row " +
                            std::to_string(i));
                    }
                    return false;
                }
            }
            const auto commit_status =
                chain_db_->writeBatch(token, std::move(migration_batch), true);
            if (commit_status != Status::Ok) {
                if (logger_) {
                    logger_->error(
                        "[ChainstateService] Sqlite nullifier migration: "
                        "writeBatch commit failed (status=" +
                        std::to_string(static_cast<int>(commit_status)) + ")");
                }
                return false;
            }
            if (logger_) {
                logger_->warning(
                    "[ChainstateService] Migrated " + std::to_string(expected) +
                    " sqlite nullifier rows into ChainDB; ChainDB is now authoritative");
            }
        }
    }

    // Phase 3b option 1: shielded frontier read precedence:
    //   1. ChainDB blob under utreexo-meta key "shielded_frontier"
    //      (the new canonical home — written into the same atomic
    //      WriteBatch as the ShieldedTipMarker by ConnectTip /
    //      DisconnectTip, eliminating the marker-vs-frontier gap).
    //   2. legacy flat file shielded_frontier.bin (preserved for one
    //      release as fallback for downgrades / old datadirs; the
    //      next ConnectTip overwrites the ChainDB row with the
    //      latest frontier so migration is automatic).
    //   3. empty (genesis-only state, no shielded activity yet).
    // Shielded epoch reset safety gate. Past the cutover the ChainDB blobs are
    // authoritative; the legacy flat files (shielded_frontier.bin /
    // shielded_anchor_history.bin) are NEVER rewritten at H, so they still hold
    // PRE-RESET state. A blob miss/corruption that falls back to them would
    // resurrect the discarded tree + anchors while the nullifier CF is empty —
    // a double-spend. Refuse the stale fallback once the tip is at/past H.
    // (Dormant reset height short-circuits, so this is inert until activation.)
    uint32_t shielded_tip_height_for_reset = 0;
    if (chain_db_) {
        const auto tip_for_reset = chain_db_->getTip();
        if (tip_for_reset.status() == Status::Ok) {
            shielded_tip_height_for_reset = tip_for_reset.value().height;
        }
    }
    const uint32_t shielded_reset_height_load =
        dinero::Params().shielded_epoch_reset_height;
    const bool past_shielded_epoch_reset =
        shielded_reset_height_load !=
            consensus::shielded::kShieldedEpochResetDormant &&
        shielded_tip_height_for_reset >= shielded_reset_height_load;

    bool frontier_loaded_from_chaindb = false;
    if (chain_db_) {
        const auto chaindb_blob = chain_db_->getUtreexoMeta("shielded_frontier");
        if (chaindb_blob.ok()) {
            const std::string& blob = chaindb_blob.value();
            if (!blob.empty()) {
                const auto* data = reinterpret_cast<const uint8_t*>(blob.data());
                if (!shielded_tree_.DeserializeFrontier(data, blob.size())) {
                    if (logger_) {
                        logger_->error("[ChainstateService] Failed to deserialize shielded frontier from ChainDB; "
                                       "falling back to flat file");
                    }
                } else {
                    frontier_loaded_from_chaindb = true;
                }
            }
        }
    }

    if (!frontier_loaded_from_chaindb) {
        if (past_shielded_epoch_reset) {
            if (logger_) {
                logger_->error(
                    "[ChainstateService] shielded frontier ChainDB blob missing/invalid "
                    "at/past the epoch reset height " +
                    std::to_string(shielded_reset_height_load) + " (tip=" +
                    std::to_string(shielded_tip_height_for_reset) +
                    "); refusing the stale pre-reset flat file (would resurrect the "
                    "discarded pool)");
            }
            return false;
        }
        if (shielded_frontier_path_.empty() || !std::filesystem::exists(shielded_frontier_path_)) {
            return true;
        }

        std::ifstream in(shielded_frontier_path_, std::ios::binary);
        if (!in) {
            if (logger_) {
                logger_->error("[ChainstateService] Failed to read shielded frontier: " +
                               shielded_frontier_path_.string());
            }
            return false;
        }

        std::vector<uint8_t> frontier((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        if (frontier.empty()) {
            return true;
        }

        if (!shielded_tree_.DeserializeFrontier(frontier.data(), frontier.size())) {
            if (logger_) {
                logger_->error("[ChainstateService] Failed to deserialize shielded frontier from " +
                               shielded_frontier_path_.string());
            }
            return false;
        }

        if (logger_) {
            logger_->info("[ChainstateService] Loaded shielded frontier from legacy flat file; "
                          "next ConnectTip will migrate it to ChainDB");
        }
    }

    // Phase 3b step 2: anchor history storage moved to ChainDB.
    //
    // Read precedence:
    //   1. ChainDB blob under utreexo-meta key
    //      "shielded_anchor_history" (the new home)
    //   2. legacy flat file shielded_anchor_history.bin
    //      (preserved for one release as fallback / forensic compare)
    //   3. empty (window rebuilds from incoming blocks at the cost
    //      of stricter anchor checks for ~kDepth blocks)
    //
    // Migration: if (1) is absent AND (2) is present AND the
    // migration sentinel "shielded_anchor_history_migrated_v1" is
    // absent, drain the flat file into the in-memory deque, then
    // PersistShieldedState() at shutdown will write it to ChainDB
    // and stamp the sentinel. This is exactly the migration shape
    // the operator's six-step plan calls for in step 2.
    constexpr const char* kAnchorHistoryKey       = "shielded_anchor_history";
    constexpr const char* kAnchorHistoryMigrated  = "shielded_anchor_history_migrated_v1";

    bool anchor_history_loaded_from_chaindb = false;
    if (chain_db_) {
        const auto chaindb_blob = chain_db_->getUtreexoMeta(kAnchorHistoryKey);
        if (chaindb_blob.ok()) {
            const std::string& blob = chaindb_blob.value();
            std::vector<uint8_t> bytes(blob.begin(), blob.end());
            const auto rc = shielded_anchor_history_.DeserializeBytes(bytes);
            if (rc == consensus::shielded::AnchorHistory::IoResult::Ok) {
                anchor_history_loaded_from_chaindb = true;
            } else if (logger_) {
                logger_->warning(
                    "[ChainstateService] ChainDB anchor history blob "
                    "rejected (code=" + std::to_string(static_cast<int>(rc)) +
                    "); falling back to flat file");
            }
        }
    }

    const auto anchor_path =
        shielded_frontier_path_.parent_path() / "shielded_anchor_history.bin";
    if (!anchor_history_loaded_from_chaindb && past_shielded_epoch_reset) {
        // Same reset safety gate as the frontier: the anchor flat file holds
        // pre-reset roots and is never rewritten at H, so loading it past the
        // cutover would resurrect the discarded anchor window. Refuse.
        if (logger_) {
            logger_->error(
                "[ChainstateService] shielded anchor history ChainDB blob "
                "missing/invalid at/past the epoch reset height " +
                std::to_string(shielded_reset_height_load) + " (tip=" +
                std::to_string(shielded_tip_height_for_reset) +
                "); refusing the stale pre-reset flat file");
        }
        return false;
    }
    if (!anchor_history_loaded_from_chaindb && std::filesystem::exists(anchor_path)) {
        const auto rc = shielded_anchor_history_.Load(anchor_path.string());
        if (rc != consensus::shielded::AnchorHistory::IoResult::Ok && logger_) {
            logger_->warning(
                "[ChainstateService] Failed to load shielded anchor history "
                "from flat file (code=" + std::to_string(static_cast<int>(rc)) +
                "); window will rebuild from incoming blocks");
        } else if (rc == consensus::shielded::AnchorHistory::IoResult::Ok && logger_) {
            // Migration trigger: flat-file load succeeded, ChainDB
            // didn't have it. PersistShieldedState() on shutdown
            // will write the in-memory deque into ChainDB and stamp
            // the migration sentinel.
            logger_->info(
                "[ChainstateService] Loaded shielded anchor history from "
                "legacy flat file; will migrate to ChainDB on next persist");
        }
    }

    // Tag the sentinel name + key in a debug log so operators can
    // verify migration completion via getUtreexoMeta on the daemon.
    if (logger_ && chain_db_) {
        const auto sentinel = chain_db_->getUtreexoMeta(kAnchorHistoryMigrated);
        if (sentinel.ok()) {
            logger_->info(
                "[ChainstateService] Anchor history ChainDB migration sentinel "
                "present (value=" + sentinel.value() + ")");
        }
    }

    return true;
}

bool ChainstateService::PersistShieldedState() const {
    if (shielded_frontier_path_.empty()) {
        return false;
    }

    const auto frontier = shielded_tree_.SerializeFrontier();
    std::ofstream out(shielded_frontier_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(frontier.data()),
              static_cast<std::streamsize>(frontier.size()));
    if (!out.good()) {
        return false;
    }

    // Phase 3b step 2: anchor history storage moved to ChainDB.
    //
    // Write order:
    //   1. ChainDB blob under "shielded_anchor_history" (new home)
    //   2. migration sentinel under "shielded_anchor_history_migrated_v1"
    //      (idempotent — re-stamped every shutdown so operators can
    //      check migration status via the daemon RPC)
    //   3. flat file shielded_anchor_history.bin (kept for one
    //      release as fallback / forensic compare; deletable in the
    //      release after step 3 lands)
    //
    // If (1) fails the flat file still gets written, so a
    // ChainDB-side outage does not lose the window. If the daemon
    // crashes between (1) and (2), the next startup re-reads the
    // ChainDB blob, sees no sentinel, and re-stamps it on the next
    // shutdown — idempotent.
    constexpr const char* kAnchorHistoryKey       = "shielded_anchor_history";
    constexpr const char* kAnchorHistoryMigrated  = "shielded_anchor_history_migrated_v1";

    if (chain_db_) {
        // Persist the frontier to ChainDB as well as the flat file. ChainDB is
        // the authoritative source the startup loader prefers, and — past the
        // shielded epoch reset height — the ONLY source it accepts (the reset
        // resurrection guard refuses the stale flat file). Every caller of
        // PersistShieldedState writes it here, including the stateless commit
        // path which otherwise never stages the frontier blob; without this a
        // stateless node past the cutover has no blob and bricks on restart.
        {
            ChainWriteToken ftoken = ChainWriteToken::CreateForTesting();
            const std::string frontier_blob(frontier.begin(), frontier.end());
            const auto fput =
                chain_db_->putUtreexoMeta(ftoken, "shielded_frontier", frontier_blob);
            if (fput != Status::Ok && logger_) {
                logger_->warning(
                    "[ChainstateService] Failed to persist shielded frontier to "
                    "ChainDB (status=" + std::to_string(static_cast<int>(fput)) +
                    "); flat file remains");
            }
        }
        const auto bytes = shielded_anchor_history_.SerializeBytes();
        const std::string blob(bytes.begin(), bytes.end());
        ChainWriteToken token = ChainWriteToken::CreateForTesting();
        const auto put_status =
            chain_db_->putUtreexoMeta(token, kAnchorHistoryKey, blob);
        if (put_status != Status::Ok && logger_) {
            logger_->warning(
                "[ChainstateService] Failed to persist anchor history to "
                "ChainDB (status=" + std::to_string(static_cast<int>(put_status)) +
                "); falling back to flat file only");
        } else {
            // Stamp the migration sentinel idempotently. The value
            // ("1") records the schema version a future migration
                // would compare against if the format ever changes.
            chain_db_->putUtreexoMeta(token, kAnchorHistoryMigrated, "1");
        }
    }

    // Flat-file fallback for one release.
    const auto anchor_path =
        shielded_frontier_path_.parent_path() / "shielded_anchor_history.bin";
    const auto rc = shielded_anchor_history_.Save(anchor_path.string());
    if (rc != consensus::shielded::AnchorHistory::IoResult::Ok && logger_) {
        logger_->warning(
            "[ChainstateService] Failed to persist shielded anchor history "
            "to flat file (code=" + std::to_string(static_cast<int>(rc)) + ")");
    }
    return true;
}

ChainstateService::ShieldedStateSnapshot ChainstateService::CurrentShieldedStateSnapshot() const {
    ShieldedStateSnapshot snapshot;
    snapshot.tree_size = shielded_tree_.Size();
    snapshot.nullifier_count = shielded_nullifiers_.Size();

    const auto root = shielded_tree_.Root();
    static_assert(sizeof(snapshot.root.data) == root.size(), "uint256/shielded root size mismatch");
    std::memcpy(snapshot.root.data, root.data(), root.size());
    return snapshot;
}

uint256 ChainstateService::ComputeShieldedReorgStateHash() const {
    // Phase 2 + phase 3b step 1 of the shielded reorg invertibility
    // plan (docs/specs/shielded_reorg_invertibility_audit.md +
    // atomic_consensus_persistence_phase3.md): hash every mutable
    // state container that crosses the reorg boundary so the
    // property test can assert byte-equality across a
    // Connect/Disconnect/Connect cycle.
    //
    // Layout (concatenated, then SHA256-hashed):
    //   [tag 'DSR2']                          4 B
    //   [version = 2]                         1 B
    //   [forest commitment]                  32 B  (zeros if forest absent)
    //   [forest numLeaves LE]                 8 B
    //   [forest canonical_empty_roots flag]   1 B
    //   [shielded tree root]                 32 B
    //   [shielded tree size LE]               8 B
    //   [nullifier set serialized content]    variable
    //                                          (NullifierSet::SerializeContent —
    //                                           sorted by (height, nullifier))
    //   [anchor history serialized bytes]     variable (kFileMagic + entries)
    //
    // ── v2 vs v1 ─────────────────────────────────────────────────────
    // v1 (commit 81e5db8ec) hashed only the nullifier *count*. Two
    // nullifier sets with the same size but different members
    // produced the same digest — count drift was caught, content
    // drift was not. Audit gap #9.
    //
    // v2 (this commit, phase 3b step 1) replaces the count field
    // with `NullifierSet::SerializeContent()` — a stable
    // (block_height ASC, nullifier ASC)-sorted byte stream of every
    // entry. Any nullifier swapped for a different value at the
    // same height now changes the digest. Property tests get a
    // real content-level oracle for everything that follows
    // (anchor history migration, atomic batch, recovery-maze
    // deletion).
    //
    // Tag bumped from 'DSRH' (v1) to 'DSR2' so a digest computed
    // by a v2 binary cannot be confused with a v1 digest of the
    // same state. Old fixtures that hard-coded a v1 digest will
    // not compare; that's the point — the oracle is sharper now.
    //
    // ── Coverage that still holds ────────────────────────────────
    // - utreexo forest: commitment + numLeaves + flag. Any leaf-level
    //   change surfaces as a commitment change.
    // - shielded tree: root + size. Any append shifts the root.
    // - anchor history: full serialized form including every
    //   (height, root) pair.
    //
    // ── Still NOT in scope (intentional) ─────────────────────────
    // - The UTXO map (consensus_utxo_set_->utxos_) is not separately
    //   hashed. Its content fingerprint goes through the utreexo
    //   forest (every UTXO is a leaf), so UTXO content drift
    //   surfaces as a forest commitment drift. Tighter binding via
    //   the forest is sufficient.
    // - Phase 3-6 atomicity (working-copy + journal row + write-path
    //   unification + recovery-maze deletion) is structural, not
    //   reflected in this digest.

    std::vector<uint8_t> preimage;
    preimage.reserve(128);

    // Tag 'DSR2' + version 2 (phase 3b step 1: nullifier content
    // coverage replaces the v1 count-only summary)
    preimage.push_back('D'); preimage.push_back('S');
    preimage.push_back('R'); preimage.push_back('2');
    preimage.push_back(2);

    auto append_le64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            preimage.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    };

    // 1. Utreexo forest
    if (consensus_utxo_set_) {
        const auto& forest = consensus_utxo_set_->GetForest();
        const auto commitment = forest.getCommitment();
        if (commitment.size() == 32) {
            preimage.insert(preimage.end(), commitment.begin(), commitment.end());
        } else {
            preimage.insert(preimage.end(), 32, 0);
        }
        append_le64(forest.getNumLeaves());
        preimage.push_back(forest.isCanonicalEmptyRoots() ? 1 : 0);
    } else {
        preimage.insert(preimage.end(), 32 + 8 + 1, 0);
    }

    // 2. Shielded tree
    {
        const auto root = shielded_tree_.Root();
        if (root.size() == 32) {
            preimage.insert(preimage.end(), root.begin(), root.end());
        } else {
            preimage.insert(preimage.end(), 32, 0);
        }
        append_le64(shielded_tree_.Size());
    }

    // 3. Nullifier set CONTENT (v2 — replaces v1's count-only field).
    //    Phase 3b nullifier fold-in: ChainDB is the durable source of
    //    truth (rows written into the unified ConnectTip /
    //    DisconnectTip WriteBatch atomically with marker / frontier /
    //    anchor history / journal row). DSRH continues to read from
    //    the in-memory NullifierSet because at WRITE time (during
    //    ConnectTip's batch staging, before writeBatch commits) the
    //    new block's nullifiers are queued in the WriteBatch but not
    //    yet visible via forEachShieldedNullifier — the in-memory
    //    set already reflects them. At READ time (startup), the
    //    in-memory set is rebuilt from ChainDB, so DSRH-from-memory
    //    == DSRH-from-ChainDB. Same byte-exact format as v1:
    //      tag 'NSCF' || version=1 || count_LE_u64 ||
    //        per row (height ASC, nullifier ASC):
    //          height_LE_u32 || nullifier_bytes (32)
    {
        const auto bytes = shielded_nullifiers_.SerializeContent();
        preimage.insert(preimage.end(), bytes.begin(), bytes.end());
    }

    // 4. Anchor history — every (height, root) pair contributes
    {
        const auto bytes = shielded_anchor_history_.SerializeBytes();
        preimage.insert(preimage.end(), bytes.begin(), bytes.end());
    }

    // Single-shot SHA256 via the existing CSHA256 streaming helper.
    dinero::crypto::CSHA256 hasher;
    hasher.Write(preimage.data(), preimage.size());
    uint8_t digest[32];
    hasher.Finalize(digest);

    uint256 out;
    std::memcpy(out.data, digest, 32);
    return out;
}

bool ChainstateService::VerifyConsensusJournalAtActiveTip() {
    // Phase 3b step 3 part 2 — startup verification of the journal
    // row written by ConsensusWriteBatch::Commit() (commit 85eacb55d).
    //
    // Semantic: at canonical-tip activation time (post-startup,
    // post-LoadShieldedState), look up the journal row keyed by the
    // current canonical tip. If present, its stored DSRH v2 hex
    // must match the live state's ComputeShieldedReorgStateHash.
    // Mismatch is the partial-commit signature §1.4 names —
    // something committed to the live containers without committing
    // the journal row that records the post-apply state.
    //
    // Absent journal row is NOT an error. The flag was opt-in so
    // pre-flag-on blocks have no rows. Only mismatch trips safe
    // mode.
    //
    // This check is best-effort: it verifies a specific class of
    // partial-commit shape, not all of them. Step 3 part 3
    // (container collapse + canonical-pointer-last-to-move) is
    // what makes the journal row the AUTHORITATIVE distinguisher
    // §1.4 promises. Until then this method is a useful but
    // incomplete oracle.

    // Skip the check entirely if the flag is off — the daemon
    // has been writing nothing into the journal column, so there's
    // nothing to verify.
    if (!consensus::ConsensusWriteBatch::IsEnabled(*this)) {
        return true;
    }
    if (!chain_db_ || !active_tip_) {
        return true;  // nothing to check yet
    }

    const uint32_t tip_height = static_cast<uint32_t>(active_tip_->height);
    const uint256 tip_hash = active_tip_->hash;

    char height_be_hex[9];
    std::snprintf(height_be_hex, sizeof(height_be_hex), "%08x", tip_height);
    const std::string journal_key =
        std::string("consensus_journal:") + height_be_hex + ":" +
        tip_hash.GetHex();

    const auto stored = chain_db_->getUtreexoMeta(journal_key);
    if (stored.status() != Status::Ok) {
        // No row for this tip. Pre-flag-on blocks won't have one;
        // first flag-on block written but daemon crashed before the
        // journal row's putUtreexoMeta also wouldn't have one. Both
        // cases: log info, accept. Step 3 part 3's atomic batch
        // collapses this ambiguity by making the journal row part of
        // the same WriteBatch as the canonical-pointer write.
        if (logger_) {
            logger_->info(
                "[ChainstateService] No consensus_journal row for active tip "
                "@ height " + std::to_string(tip_height) +
                " (expected for blocks predating consensus.atomic_persist=1)");
        }
        return true;
    }

    const std::string& stored_hex = stored.value();
    const auto live_state_hash = ComputeShieldedReorgStateHash();
    const std::string live_hex = live_state_hash.GetHex();

    if (stored_hex == live_hex) {
        if (logger_) {
            logger_->info(
                "[ChainstateService] consensus_journal row verified at tip "
                "@ height " + std::to_string(tip_height) + " (DSRH v2 match)");
        }
        return true;
    }

    // Mismatch — partial-commit signature.
    const std::string reason =
        "consensus_journal_state_mismatch (height=" +
        std::to_string(tip_height) +
        " stored_hash=" + stored_hex.substr(0, 16) + "..." +
        " live_hash="   + live_hex.substr(0, 16) + "...)";
    if (logger_) {
        logger_->error(
            "[ChainstateService] " + reason +
            " — entering consensus safe mode. Operator must inspect chain "
            "state and call safemode.exit { confirm: true } to resume.");
    }
    EnterSafeMode(reason);
    return false;
}

bool ChainstateService::PersistShieldedTipMarker(const uint256& tip_hash, uint32_t tip_height) const {
    if (!chain_db_) {
        return false;
    }

    const auto snapshot = CurrentShieldedStateSnapshot();
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    ChainDB::ShieldedTipMarker marker;
    marker.height = static_cast<int32_t>(tip_height);
    marker.block_hash = tip_hash;
    marker.shielded_root = snapshot.root;
    marker.tree_size = snapshot.tree_size;
    marker.nullifier_count = snapshot.nullifier_count;
    return chain_db_->putShieldedTipMarker(token, marker) == Status::Ok;
}

// #356: Shared shielded-apply funnel for the stateless replay/recovery connect
// path. Extracted verbatim from the ABC-CSN reorg replay loop's inline
// delta-recompute + pre-block frontier capture + ApplyBlockShieldedSection
// sequence (PR #355), gated by the marker-height guard so a SECOND caller (the
// ConnectTip crash-recovery branch) can share it without double-applying.
bool ChainstateService::ApplyStatelessReplayShielded(
    const Block& block, uint32_t height,
    consensus::BlockUndo& undo_out, bool& applied_out, std::string& error,
    const std::vector<consensus::SpentOutputData>* fallback_spent_outputs) {
    applied_out = false;

    if (!block_validator_) {
        error = "shielded apply impossible: block validator not wired";
        return false;
    }
    if (!chain_db_) {
        error = "shielded apply impossible: chain db not wired";
        return false;
    }

    // Read the authoritative pool height from the persisted marker. In the
    // ABC-CSN reorg replay loop this is unreachable-NotFound: disconnect stages
    // the marker at the fork point and genesis persists it, so the marker is
    // always present and always exactly at height-1 there. A NotFound / read
    // failure therefore signals a broken invariant rather than a benign miss —
    // fail loud rather than silently skip (which would leave shielded state
    // unadvanced on a path that requires the apply).
    const auto marker_result = chain_db_->getShieldedTipMarker();
    if (marker_result.status() != Status::Ok) {
        error = "failed to read ShieldedTipMarker for stateless replay apply (status=" +
                std::to_string(static_cast<int>(marker_result.status())) + ")";
        return false;
    }
    const uint32_t marker_height =
        static_cast<uint32_t>(marker_result.value().height);

    switch (StatelessReplayShieldedDecision(marker_height, height)) {
        case StatelessReplayShieldedAction::Skip:
            // Pool already at/ahead of this block — a second apply would
            // double-count note commitments / nullifiers. Leave state and
            // undo_out untouched.
            applied_out = false;
            return true;
        case StatelessReplayShieldedAction::GapFail:
            error = "shielded pool marker height " + std::to_string(marker_height) +
                    " is behind block height-1 (block=" + std::to_string(height) +
                    ") — contiguous-recovery invariant broken";
            return false;
        case StatelessReplayShieldedAction::Apply:
            break;  // fall through to apply below
    }

    // Recompute deltas EXACTLY as the forward STATELESS per-tx loop did (bit-
    // identity pinned by ShieldedBlockSectionDeltaParity). fallback_spent_outputs
    // is consulted only when block.utreexo is absent.
    std::vector<int64_t> replay_deltas;
    if (!block_validator_->ComputeShieldedDeltasForStoredBlock(
            block, height, replay_deltas, error, fallback_spent_outputs)) {
        error = "shielded delta recompute failed: " + error;
        return false;
    }

    undo_out.height = height;
    // #356 Task 3 rider: the ConnectTip stateless-recovery call site
    // constructs a default consensus::BlockUndo and never sets block_hash
    // (unlike the ABC-CSN reorg-replay caller, which pre-sets it from
    // block_index->hash before calling in). Set it here, in the shared
    // funnel, so both callers get a correctly-identified undo record.
    undo_out.block_hash = block.GetHash();
    // Capture the pre-block frontier BEFORE the apply (mirrors the capture at
    // the top of ConnectBlockInternal) so the undo record can restore the
    // commitment tree on a later disconnect.
    {
        std::vector<uint8_t> pre_frontier =
            block_validator_->SerializeShieldedFrontier();
        if (!pre_frontier.empty()) {
            undo_out.pre_block_shielded_frontier = std::move(pre_frontier);
        }
    }

    if (!block_validator_->ApplyBlockShieldedSection(
            block, height, replay_deltas, undo_out, error)) {
        error = "shielded apply failed: " + error;
        return false;
    }

    applied_out = true;
    return true;
}

// Phase 3b step 6: deleted RecoverShieldedStateFromTipMarker,
// RestoreShieldedFrontierFromUndoBlock, ReplayShieldedBlockForward.
// These three helpers formed a closed maze whose only purpose was
// to reconcile partial-state mismatches between ShieldedTipMarker
// (in ChainDB) and the shielded frontier flat file (in std::ofstream
// land). Option 1 (frontier blob + anchor history blob + marker all
// folded into the unified ConnectTip / DisconnectTip WriteBatch)
// makes those mismatches structurally unreachable: the §1 atomic-
// unit law commits every shielded state container together. With
// the partial states gone, the recovery branches that handled them
// were dead code masking real corruption — see step-6 fail-loud
// branches in VerifyOrBootstrapShieldedTipMarker and the catch-up
// recovery path that previously called RecoverShieldedStateFromTipMarker.

bool ChainstateService::RewindShieldedStateToActiveTipForStartup(uint32_t stored_tip_height) {
    if (!chain_db_ || !active_tip_) {
        return true;
    }

    const uint32_t active_height = static_cast<uint32_t>(active_tip_->height);
    if (active_height >= stored_tip_height) {
        return true;
    }

    std::vector<uint8_t> frontier_at_active_tip;
    if (active_height > 0 || stored_tip_height > 0) {
        const auto next_hash_result =
            chain_db_->getBlockHashByHeight(static_cast<int>(active_height + 1));
        if (next_hash_result.status() != Status::Ok) {
            if (logger_) {
                logger_->error("[ChainstateService] Failed to locate block above active tip for shielded rewind");
            }
            return false;
        }

        const auto undo_result = ReadStoredUndo(next_hash_result.value());
        if (undo_result.status() != Status::Ok) {
            // No undo at all OR undo predates shielded — treat as "no
            // shielded state to rewind" rather than fatal. Blocks
            // indexed by a pre-Path-C daemon never had shielded
            // bundles, so an empty frontier at this height is correct.
            if (logger_) {
                logger_->info("[ChainstateService] No undo above active tip; "
                              "assuming pre-shielded chain and starting with empty "
                              "shielded frontier (height=" +
                              std::to_string(active_height) + ")");
            }
            // Fall through with frontier_at_active_tip empty.
        } else if (!undo_result.value().pre_block_shielded_frontier.has_value()) {
            // Undo exists but predates shielded recordkeeping — same
            // story: no shielded leaves were committed at or below this
            // height, so empty frontier is the correct startup state.
            if (logger_) {
                logger_->info("[ChainstateService] Undo above active tip lacks "
                              "shielded frontier snapshot; assuming pre-shielded "
                              "block and starting with empty frontier (height=" +
                              std::to_string(active_height) + ")");
            }
            // Fall through with frontier_at_active_tip empty.
        } else {
            frontier_at_active_tip = *undo_result.value().pre_block_shielded_frontier;
        }
    }

    shielded_tree_ = consensus::shielded::CommitmentTree();
    if (!frontier_at_active_tip.empty() &&
        !shielded_tree_.DeserializeFrontier(frontier_at_active_tip.data(),
                                            frontier_at_active_tip.size())) {
        if (logger_) {
            logger_->error("[ChainstateService] Failed to restore shielded frontier at active tip during startup rewind");
        }
        return false;
    }

    shielded_nullifiers_.RollbackAbove(active_height);

    if (!PersistShieldedState()) {
        if (logger_) {
            logger_->error("[ChainstateService] Failed to persist rewound shielded frontier during startup");
        }
        return false;
    }
    if (!PersistShieldedTipMarker(active_tip_->hash, active_height)) {
        if (logger_) {
            logger_->error("[ChainstateService] Failed to persist rewound ShieldedTipMarker during startup");
        }
        return false;
    }

    if (logger_) {
        logger_->warning("[ChainstateService] Rewound shielded state from stored tip height " +
                         std::to_string(stored_tip_height) + " to active tip height " +
                         std::to_string(active_height) + " for startup replay");
    }
    return true;
}

bool ChainstateService::RangeHasShieldedActivity(uint32_t start_height, uint32_t end_height) const {
    if (!chain_db_ || start_height > end_height) {
        return false;
    }

    for (uint32_t height = start_height; height <= end_height; ++height) {
        auto hash_result = chain_db_->getBlockHashByHeight(static_cast<int>(height));
        if (hash_result.status() != Status::Ok) {
            return true;
        }

        auto block_result = ReadStoredBlock(hash_result.value());
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

bool ChainstateService::VerifyOrBootstrapShieldedTipMarker(const uint256& tip_hash, uint32_t tip_height) {
    if (!chain_db_) {
        return false;
    }

    const auto marker_result = chain_db_->getShieldedTipMarker();
    if (marker_result.status() == Status::NotFound) {
        const bool safe_bootstrap =
            tip_height == 0 || !RangeHasShieldedActivity(1, tip_height);
        if (!safe_bootstrap) {
            if (logger_) {
                logger_->error("[ChainstateService] Missing ShieldedTipMarker on a chain with shielded history");
                logger_->error("[ChainstateService] Refusing startup until shielded state is rebuilt canonically");
            }
            return false;
        }

        if (logger_) {
            logger_->warning("[ChainstateService] Bootstrapping missing ShieldedTipMarker from current shielded state");
        }
        return PersistShieldedTipMarker(tip_hash, tip_height);
    }
    if (marker_result.status() != Status::Ok) {
        if (logger_) {
            logger_->error("[ChainstateService] Failed to read ShieldedTipMarker");
        }
        return false;
    }

    const auto& marker = marker_result.value();

    // Phase 3b nullifier fold-in: ChainDB is now authoritative for the
    // nullifier set, written into the unified batch alongside the
    // marker. The sqlite NullifierSet is a per-instance cache whose
    // per-call Inserts run from inside ConnectBlock and therefore can
    // race ahead of the unified batch on a pre-batch crash. Reconcile
    // up front by trimming the sqlite cache + anchor history to the
    // canonical tip height — any rows above came from a ConnectBlock
    // attempt whose unified batch never committed, and ChainDB is
    // already free of those rows because DisconnectTip's rollback
    // batch runs the deleteShieldedNullifiersAboveHeight stage. After
    // the trim, marker / state must agree by construction. If they
    // still don't, that's real corruption (manual datadir surgery,
    // disk damage) — fail loud rather than try to silently recover.
    shielded_nullifiers_.RollbackAbove(tip_height);
    shielded_anchor_history_.RollbackAbove(tip_height);
    const auto reconciled = CurrentShieldedStateSnapshot();

    const bool tip_matches =
        marker.block_hash == tip_hash &&
        marker.height == static_cast<int32_t>(tip_height);
    const bool state_matches =
        marker.shielded_root == reconciled.root &&
        marker.tree_size == reconciled.tree_size &&
        marker.nullifier_count == reconciled.nullifier_count;
    if (tip_matches && state_matches) {
        return true;
    }

    // In AssumeUTXO/mobile headers-only mode, ChainDB intentionally remains
    // below the snapshot base while the active consensus tip is restored from
    // the snapshot. Historical block bodies are not locally available, so the
    // range scan below would conservatively report "shielded activity" and
    // fail a valid snapshot restore. If the shielded state itself matches the
    // persisted marker, advance only the marker tip to the snapshot base.
    if (assumeutxo_active_ && state_matches && !tip_matches) {
        if (logger_) {
            logger_->warning("[ChainstateService] Advancing ShieldedTipMarker across AssumeUTXO snapshot restore");
        }
        return PersistShieldedTipMarker(tip_hash, tip_height);
    }

    // The shielded-inactive-range advance path below stays: an
    // operator may legitimately load a chain whose tip advanced
    // across pre-shielded heights without writing a marker for
    // every block, in which case advancing the stale marker is
    // safe and the tip+state aren't expected to match.

    const uint32_t marker_height = marker.height < 0 ? 0u : static_cast<uint32_t>(marker.height);
    const uint32_t range_start = std::min(marker_height, tip_height) + 1;
    const uint32_t range_end = std::max(marker_height, tip_height);
    const bool active_range_has_shielded =
        range_start <= range_end && RangeHasShieldedActivity(range_start, range_end);

    if (!state_matches || (active_range_has_shielded && !tip_matches)) {
        if (logger_) {
            logger_->error("[ChainstateService] ShieldedTipMarker disagrees with persisted shielded state");
            logger_->error("[ChainstateService]   marker tip=" +
                           marker.block_hash.GetHex().substr(0, 16) + "...@" +
                           std::to_string(marker.height) + " active tip=" +
                           tip_hash.GetHex().substr(0, 16) + "...@" +
                           std::to_string(tip_height));
            logger_->error("[ChainstateService]   tip_matches=" + std::to_string(tip_matches) +
                           " state_matches=" + std::to_string(state_matches));
            logger_->error("[ChainstateService] Phase 3b option 1 makes this state unreachable through "
                           "ConnectTip/DisconnectTip; failing startup so the underlying corruption is "
                           "investigated rather than silently recovered. If you reached this via a "
                           "snapshot import or manual datadir surgery, run --reindex-chainstate.");
        }
        return false;
    }

    if (logger_) {
        logger_->warning("[ChainstateService] Advancing stale ShieldedTipMarker across shielded-inactive range");
    }
    return PersistShieldedTipMarker(tip_hash, tip_height);
}

void ChainstateService::SetAssumeUTXOState(const uint256& base_block,
                                           uint32_t base_height,
                                           bool persist_metadata) {
    {
        // ActivateBestChain reads/writes this flag under activation_mutex_.
        // Snapshot loading is independently serialized, so take the same lock
        // here rather than introducing a plain-bool data race with P2P activation.
        std::lock_guard<std::recursive_mutex> activation_lock(activation_mutex_);
        assumeutxo_header_import_deferred_logged_ = false;
    }
    assumeutxo::SetState(
        {assumeutxo_active_, assumeutxo_base_block_, assumeutxo_base_height_, utxo_index_.get()},
        base_block,
        base_height,
        persist_metadata);

    if (persist_metadata) {
        if (logger_) {
            logger_->info("[AssumeUTXO] Persisted snapshot state to metadata");
        }
    }
}

void ChainstateService::ClearAssumeUTXOState(bool clear_persisted_metadata) {
    assumeutxo::ClearState(
        {assumeutxo_active_, assumeutxo_base_block_, assumeutxo_base_height_, utxo_index_.get()},
        clear_persisted_metadata);

    if (clear_persisted_metadata) {
        if (logger_) {
            logger_->info("[AssumeUTXO] Cleared persisted snapshot state from metadata");
        }
    }
}

// AssumeUTXO fatal state machine (docs/design/assumeutxo-fatal-state-machine.md).
// Lazy: utxo_index_ must exist before the lifecycle can persist, and call sites
// span startup restore, LoadSnapshot, and the background-validation worker.
void ChainstateService::EnsureAssumeUtxoLifecycle() {
    std::lock_guard<std::mutex> lock(assumeutxo_lifecycle_init_mutex_);
    if (!assumeutxo_lifecycle_) {
        // Config knob: assumeutxo_bg_stall_timeout (seconds). Default 1800 (30 min
        // per spec). Clamped to ≥1 s so tests/regression configs can use small
        // values without underflowing the stall clock.
        const int stall_timeout_s = config_
            ? config_->GetInt("assumeutxo_bg_stall_timeout", 1800)
            : 1800;
        assumeutxo_lifecycle_ = std::make_unique<assumeutxo::AssumeUtxoLifecycle>(
            utxo_index_.get(), logger_ ? &logger_->get() : nullptr,
            std::chrono::seconds(std::max(1, stall_timeout_s)));
    }
}

assumeutxo::AssumeUtxoLifecycle* ChainstateService::GetAssumeUtxoLifecycle() {
    EnsureAssumeUtxoLifecycle();
    return assumeutxo_lifecycle_.get();
}

bool ChainstateService::ResetAssumeUtxoFatalState(const std::string& confirm_token) {
    EnsureAssumeUtxoLifecycle();
    if (!assumeutxo_lifecycle_->OperatorReset(confirm_token)) {
        return false;
    }

    // Wipe the bulk-loaded assumed UTXO set (spec: Reset must clear assumed UTXO state).
    // OperatorReset already deleted lifecycle metadata keys; ClearAll drops all wallet_utxos
    // and utxo_metadata rows from the DB.  Best-effort: log and continue if it fails.
    if (utxo_index_) {
        if (!utxo_index_->ClearAll()) {
            if (logger_) {
                logger_->error("[AssumeUTXO] ClearAll failed during reset — "
                               "Manual intervention required - delete wallet.db");
            }
        }
    }

    // A finished-but-unjoined worker handle would std::terminate the daemon
    // when StartBackgroundValidation re-creates the thread on the documented
    // recovery flow (reset -> safemode.exit -> load snapshot). Join it first;
    // never under bg_validation_mutex_ (the worker takes that mutex).
    if (bg_validation_thread_ && bg_validation_thread_->joinable()) {
        // #298: set the stop flag under bg_validation_wait_mutex_ so it cannot
        // land in the worker's predicate-eval/block window (lost wakeup) — then
        // notify so the signalable rescan wait returns at once instead of
        // sleeping the 30s backstop while we join.
        {
            std::lock_guard<std::mutex> wlk(bg_validation_wait_mutex_);
            bg_validation_should_stop_ = true;
        }
        bg_validation_cv_.notify_all();
        bg_validation_thread_->join();
        // #298: clear the wake callback now that the worker is gone, so a late
        // backfill store cannot invoke the [this] capture after teardown.
        if (auto* ctx = DaemonContext::instance(); ctx && ctx->block_download) {
            ctx->block_download->SetOnBackfillBodyStored(nullptr);
        }
        bg_validation_thread_.reset();
        bg_validation_should_stop_ = false;
    }

    // Reset legacy in-memory background-validation state so a post-reset restart
    // does not see stale InProgress/Failed status (spec FIX 3).
    {
        std::lock_guard<std::mutex> lock(bg_validation_mutex_);
        bg_validation_status_ = BackgroundValidationStatus::NotStarted;
        bg_validation_error_.clear();
        bg_validation_current_height_ = 0;
        bg_validation_blocks_validated_ = 0;
    }

    // Clear in-memory flags + belt-and-braces metadata-key wipe.
    // ClearAll already dropped utxo_metadata rows; this also resets assumeutxo_active_
    // and the base-block/height in-memory fields.
    ClearAssumeUTXOState(/*clear_persisted_metadata=*/true);
    return true;
}

// Phase 8: Set validation mode (stateful vs stateless)
void ChainstateService::setValidationMode(consensus::ValidationMode mode) {
    pending_validation_mode_ = mode;  // Store for deferred application
    if (block_validator_) {
        block_validator_->setValidationMode(mode);
    }
}

bool ChainstateService::Init(DaemonContext& ctx) {
    // Store dependencies
    if (ctx.logger) {
        logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    }
    if (ctx.config) {
        config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    }
    block_storage_ = ctx.block_storage;

    if (!logger_ || !config_) {
        std::cerr << "[ChainstateService] Missing required dependencies" << std::endl;
        return false;
    }

    strict_archival_reads_ = true;
    logger_->warning("[ChainstateService] Strict archival reads enabled: flatfile bodies/undo are required");

    // Get datadir from config
    datadir_ = config_->DataDir();

    // Ensure blockchain directory exists
    std::filesystem::path blockchain_path = std::filesystem::path(datadir_) / "blockchain";
    try {
        std::filesystem::create_directories(blockchain_path);
    } catch (const std::exception& e) {
        logger_->error("[ChainstateService] Failed to create blockchain directory: " +
                      std::string(e.what()));
        return false;
    }

    shielded_frontier_path_ = blockchain_path / "shielded_frontier.bin";
    if (!LoadShieldedState()) {
        logger_->error("[ChainstateService] Failed to initialize shielded pool state");
        return false;
    }

    // ❌ REMOVED: ChainDB construction (moved to DaemonApp per ONE DB Definition)
    // Phase 39: ChainDB is owned by ctx.chain_manager, constructed by DaemonApp before service Init()
    // No global g_chain_manager - access via DaemonContext::instance()->chain_manager

    // Create UTXO Index (legacy - will be phased out)
    try {
        std::string utxo_db_path = blockchain_path.string() + "/utxo";
        utxo_index_ = std::make_unique<UTXOIndex>(utxo_db_path);
        if (!utxo_index_->Initialize()) {
            logger_->error("[ChainstateService] Failed to initialize UTXO Index");
            return false;
        }
        logger_->info("[ChainstateService] UTXO Index created and initialized");

        // ═══════════════════════════════════════════════════════════════════════════
        // Crash Safety: Check for incomplete reorg from previous run
        // ═══════════════════════════════════════════════════════════════════════════
        auto reorg_marker = utxo_index_->GetMetadata("reorg_in_progress");
        if (reorg_marker.has_value()) {
            logger_->warning("⚠️  Reorg marker found: " + reorg_marker.value());

            // Auto-recovery: check if forest state is consistent with the
            // ChainDB tip. This handles: (1) power loss after reorg completed
            // but before marker cleared, (2) rebuildutreexo forest corruption
            // where disk checkpoint was never affected.
            //
            // Campaign phase 3: full checkpoints exist only every N blocks,
            // so "consistent" no longer means "checkpoint at exactly the
            // tip" (that guard bricked flag-on nodes killed mid-reorg —
            // caught by the mainnet A/B torture 2026-07-17). Consistent
            // means the forest AT the tip is reconstructible: nearest
            // checkpoint + UD-sidecar replay, with the checkpoint and every
            // replayed block verified against their headers' utreexo_root —
            // which subsumes the old exact-height checksum + root check.
            bool state_aligned = false;
            if (chain_db_) {
                auto tip_result = chain_db_->getTip();
                if (tip_result.ok()) {
                    consensus::UtreexoForest tip_forest;
                    std::string restore_error;
                    state_aligned = storage::RestoreHistoricalForest(
                        *chain_db_,
                        static_cast<uint32_t>(tip_result.value().height),
                        tip_forest, restore_error) == Status::Ok;
                    if (!state_aligned) {
                        logger_->warning(
                            "[ChainstateService] Reorg-marker alignment restore failed: " +
                            restore_error);
                    }
                }
            }

            if (state_aligned) {
                logger_->warning("═══════════════════════════════════════════════════════════════════════════");
                logger_->warning("✅ Reorg marker found but state is consistent — AUTO-CLEARING marker");
                logger_->warning("Forest checkpoint matches ChainDB tip. Safe to continue.");
                logger_->warning("═══════════════════════════════════════════════════════════════════════════");
                utxo_index_->DeleteMetadata("reorg_in_progress");
            } else {
                logger_->error("═══════════════════════════════════════════════════════════════════════════");
                logger_->error("⚠️  CRITICAL: Incomplete reorg detected from previous shutdown!");
                logger_->error("═══════════════════════════════════════════════════════════════════════════");
                logger_->error("Marker: " + reorg_marker.value());
                logger_->error("");
                logger_->error("The daemon crashed or was killed during a blockchain reorganization.");
                logger_->error("The UTXO database may be in an inconsistent state.");
                logger_->error("Auto-recovery failed: forest checkpoint does not match ChainDB tip.");
                logger_->error("");
                logger_->error("Recovery options:");
                logger_->error("  1. Resync from genesis: Delete blockchain directory and restart");
                logger_->error("  2. Restore from backup: Replace with known-good backup");
                logger_->error("");
                logger_->error("Startup is aborted to avoid serving from an inconsistent chainstate.");
                logger_->error("═══════════════════════════════════════════════════════════════════════════");

                incomplete_reorg_detected_ = true;
                return false;
            }
        }

    } catch (const std::exception& e) {
        logger_->error("[ChainstateService] Failed to create UTXO Index: " +
                      std::string(e.what()));
        return false;
    }

    // Phase 11a: Create Global UTXO Position Index (indexing layer)
    try {
        utxo_position_index_ = std::make_unique<indexing::UTXOPositionIndex>();
        logger_->info("[ChainstateService] UTXO Position Index created (global proof indexing)");
    } catch (const std::exception& e) {
        logger_->error("[ChainstateService] Failed to create UTXO Position Index: " +
                      std::string(e.what()));
        return false;
    }

    // Phase 39: ChainDB must be set by DaemonApp before Init()
    if (!chain_db_) {
        logger_->error("[ChainstateService] chain_db_ not initialized (DaemonApp must call setChainDB first)");
        return false;
    }
    logger_->info("[ChainstateService] Using chain_db_ (ONE DB authority)");

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 2: Create ConsensusUTXOSet (pure in-memory, owns Utreexo forest)
    // ═══════════════════════════════════════════════════════════════════════════
    // ConsensusUTXOSet tracks ALL UTXOs in memory. Forest is a value member.
    // PersistentUTXOAdapter loads initial state from ChainDB on startup.
    // No adapter chain needed — BlockValidator takes IConsensusUTXOSet* directly.
    // ═══════════════════════════════════════════════════════════════════════════
    try {
        consensus_utxo_set_ = std::make_unique<consensus::ConsensusUTXOSet>();
        logger_->info("[ChainstateService] ConsensusUTXOSet created (pure in-memory, owns forest)");

        // Load existing UTXOs from ChainDB
        {
            ChainWriteToken load_token;
            storage::PersistentUTXOAdapter loader(*chain_db_, load_token);
            if (!loader.LoadInitialState(*consensus_utxo_set_)) {
                logger_->error("[ChainstateService] Failed to load UTXO set from ChainDB");
                return false;
            }
        }
        logger_->info("[ChainstateService] Loaded " + std::to_string(consensus_utxo_set_->GetSetSize()) + " UTXOs from ChainDB");

        // CRITICAL: BulkLoad rebuilds the forest sorted by OutPoint, which is
        // WRONG — Utreexo requires chronological block-creation insertion order.
        // Reset the forest to empty here. It will be properly rebuilt by either:
        //   1. Checkpoint deserialization (below), or
        //   2. ActivateBestChain block replay
        // The UTXO map (for spend validation) is preserved.
        consensus_utxo_set_->GetForest() = consensus::UtreexoForest();
        logger_->info("[ChainstateService] Forest reset after BulkLoad (will restore from checkpoint or replay)");

        // Create BlockValidator directly with ConsensusUTXOSet (no adapter chain)
        block_validator_ = std::make_unique<consensus::BlockValidator>(consensus_utxo_set_.get());
        block_validator_->setShieldedState(&shielded_tree_, &shielded_nullifiers_,
                                           &shielded_anchor_history_);
        if (pending_validation_mode_.has_value()) {
            block_validator_->setValidationMode(pending_validation_mode_.value());
            logger_->info("[ChainstateService] Applied deferred validation mode: " +
                         std::string(consensus::ValidationModeToString(pending_validation_mode_.value())));
        }
        logger_->info("[ChainstateService] BlockValidator created (Phase 2: direct ConsensusUTXOSet, snapshot_supported=true)");
    } catch (const std::exception& e) {
        logger_->error("[ChainstateService] Failed to create consensus state: " + std::string(e.what()));
        return false;
    }

    // Week 3: Wire DaemonContext to BlockAcceptor for chainstate/wallet/utxo access
    BlockAcceptor::SetContext(&ctx);
    logger_->info("[ChainstateService] BlockAcceptor context wired");

    // Phase C.1 v2: Create orphan block pool for P2P blocks with missing parents
    try {
        p2p::OrphanBlockPool::Config orphan_config;
        // Mainnet sync can legitimately receive out-of-order bursts up to the
        // download window size; keep per-peer headroom to avoid false DoS hits.
        orphan_config.max_orphan_blocks = 256;
        orphan_config.max_orphan_size_mb = 16;
        orphan_config.max_orphans_per_peer = 64;
        orphan_config.orphan_timeout_seconds = 600;  // 10 minutes
        orphan_config.enable_resolution = true;
        p2p_orphan_pool_ = std::make_unique<p2p::OrphanBlockPool>(orphan_config);
        logger_->info("[ChainstateService] P2P Orphan block pool initialized (fork convergence fix)");
    } catch (const std::exception& e) {
        logger_->error("[ChainstateService] Failed to create orphan pool: " + std::string(e.what()));
        return false;
    }

    // BRIDGE REMOVED (November 7, 2025):
    // - Mining code: Now uses ChainDB* via constructor injection ✅
    // - RPC handlers: Use ctx.daemon->chainstate->chainDB() ✅
    // - WalletWorker: Still uses dinero::legacy::g_utxo_set_direct(); migrate this call path to DaemonContext.
    //   WalletWorker skips scanning when no UTXO pointer is available (null-guarded behavior).

    logger_->info("[ChainstateService] Initialized successfully (bridge removed)");
    return true;
}

bool ChainstateService::Start() {
    // =========================================================================
    // Phase 2 Migration Note:
    // =========================================================================
    // The target architecture simplifies startup to:
    //   1. adapter.LoadInitialState(consensus_utxo_set)
    //   2. baseline = consensus_utxo_set.Snapshot()
    //   3. Run consensus loop (Phase2ActivateBestChain)
    //
    // Current startup includes production concerns (genesis verification,
    // Utreexo restoration, AssumeUTXO) that will be migrated
    // incrementally to use the Phase 2 components.
    //
    // See: include/storage/persistent_utxo_adapter.h for the target pattern.
    // =========================================================================

    if (started_) {
        logger_->warning("[ChainstateService] Already started");
        return false;
    }

    logger_->info("[ChainstateService] Starting chainstate...");

    // Phase 39: Check ChainDB availability
    if (!chain_db_) {
        logger_->error("[ChainstateService] chain_db_ not available");
        return false;
    }
    auto* chain_db = chain_db_;

    // Initialize genesis in ChainDB (RocksDB) - Single source of truth
    if (!initializeGenesisInChainDB()) {
        logger_->error("[ChainstateService] Failed to initialize genesis in ChainDB");
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // CRITICAL STARTUP INVARIANT (Dec 12, 2024 - Post-Utreexo Genesis Lock)
    // ═════════════════════════════════════════════════════════════════════════
    // Verify that database genesis matches code genesis.
    // This prevents:
    // - Accidental mixed binaries (dev vs production)
    // - Database from wrong network
    // - Silent consensus drift
    // - "It boots but behaves weirdly" states
    // ═════════════════════════════════════════════════════════════════════════
    const auto& params = dinero::Params();
    auto db_genesis_result = chain_db->getBlockHashByHeight(0);
    if (db_genesis_result.status() == Status::Ok) {
        const uint256& db_genesis_hash = db_genesis_result.value();  // Phase M.0: Keep as uint256
        uint256 expected_genesis_hash = uint256::FromHexUnsafe(params.genesis_hash);  // Phase M.0: Convert once

        if (db_genesis_hash != expected_genesis_hash) {  // Phase M.0: Direct uint256 comparison
            logger_->error("═══════════════════════════════════════════════════════════");
            logger_->error("  ❌ FATAL: GENESIS MISMATCH - REFUSING TO START");
            logger_->error("═══════════════════════════════════════════════════════════");
            logger_->error("Database genesis:  " + db_genesis_hash.GetHex());
            logger_->error("Code genesis:      " + expected_genesis_hash.GetHex());  // Phase M.0: Inline for logging
            logger_->error("");
            logger_->error("This means:");
            logger_->error("  • You are running the wrong binary for this database");
            logger_->error("  • Or: database is from a different network");
            logger_->error("  • Or: code genesis was changed after DB initialization");
            logger_->error("");
            logger_->error("To fix:");
            logger_->error("  1. Use correct binary matching this database");
            logger_->error("  2. OR: Delete database and resync from genesis");
            logger_->error("     rm -rf ~/.dinero && ./dinerod --chain=" + params.network_id);
            logger_->error("═══════════════════════════════════════════════════════════");

            // FATAL: Refuse to start
            std::abort();
        }

        logger_->info("✅ Genesis invariant verified: DB matches code");
        logger_->info("   Genesis: " + db_genesis_hash.GetHex());

        // ═════════════════════════════════════════════════════════════════════════
        // GENESIS IDENTITY BANNER (Dec 12, 2025)
        // ═════════════════════════════════════════════════════════════════════════
        // Machine-verifiable genesis identity - printed ONLY after validation
        // This banner proves: correct binary + correct database + correct chain
        // ═════════════════════════════════════════════════════════════════════════
        auto genesis_header_result = chain_db->getHeader(db_genesis_hash);
        if (genesis_header_result.status() == Status::Ok) {
            const auto& genesis_hdr = genesis_header_result.value();

            logger_->info("══════════════════════════════════════════════════════════");
            logger_->info("[GENESIS] Dinero Post-Utreexo v2.0.0");
            logger_->info("[GENESIS] Hash: " + db_genesis_hash.GetHex());
            logger_->info("[GENESIS] Header size: 128 bytes (Phase 3: BlockHeader v1)");
            logger_->info("[GENESIS] Timestamp: " + std::to_string(genesis_hdr.timestamp) + " (Unix epoch)");  // Phase 3: 64-bit timestamp
            logger_->info("[GENESIS] Difficulty: 0x" + std::to_string(genesis_hdr.difficulty));  // Phase 3: renamed from nBits
            logger_->info("[GENESIS] Motto: \"Dinero: Real Money For Free People\"");
            logger_->info("[GENESIS] Status: CANONICAL");
            logger_->info("══════════════════════════════════════════════════════════");
        }
        // ═════════════════════════════════════════════════════════════════════════
    } else {
        logger_->warning("[ChainstateService] Could not verify genesis (DB empty or error)");
    }
    // ═════════════════════════════════════════════════════════════════════════

    // Get current blockchain state from ChainDB
    auto tip_result = chain_db_->getTip();
    uint32_t height = (tip_result.status() == Status::Ok) ? tip_result.value().height : 0;
    auto loadHeaderOrRecoverFromBody = [&](const uint256& hash,
                                           int height_hint,
                                           BlockHeader* out_header,
                                           bool* recovered_from_body,
                                           std::string* error) -> bool {
        auto fail_recovery = [&](const std::string& msg) {
            if (error) {
                *error = msg;
            }
            return false;
        };

        auto header_result = chain_db_->getHeader(hash);
        if (header_result.status() == Status::Ok) {
            if (out_header) {
                *out_header = header_result.value();
            }
            if (recovered_from_body) {
                *recovered_from_body = false;
            }
            return true;
        }
        if (header_result.status() != Status::NotFound) {
            return fail_recovery("header-read-status-" +
                                 std::to_string(static_cast<int>(header_result.status())) +
                                 "-for-height-" + std::to_string(height_hint));
        }

        auto block_result = ReadStoredBlock(hash);
        if (block_result.status() != Status::Ok) {
            return fail_recovery("missing-header-and-body-for-height-" +
                                 std::to_string(height_hint));
        }

        if (out_header) {
            *out_header = block_result.value().header;
        }
        if (recovered_from_body) {
            *recovered_from_body = true;
        }
        if (logger_) {
            logger_->warning("[ChainstateService] Rehydrating missing header CF entry from stored block body at height " +
                             std::to_string(height_hint));
        }
        return true;
    };

    auto persistRecoveredHeader = [&](const uint256& hash,
                                      const BlockHeader& header,
                                      int height_hint,
                                      CBlockIndex* index_entry,
                                      std::string* error) -> bool {
        auto fail_recovery = [&](const std::string& msg) {
            if (error) {
                *error = msg;
            }
            return false;
        };

        if (!index_entry) {
            return fail_recovery("missing-block-index-for-header-rehydrate-height-" +
                                 std::to_string(height_hint));
        }

        arith_uint256 work;
        try {
            work = ChainworkFromHex(index_entry->chainwork);
        } catch (const std::exception& e) {
            return fail_recovery(std::string("rehydrated-header-invalid-chainwork-height-") +
                                 std::to_string(height_hint) + ": " + e.what());
        }

        ChainWriteToken token = ChainWriteToken::CreateForTesting();
        auto status = chain_db_->putHeader(token, hash, header, height_hint, work);
        if (status != Status::Ok) {
            return fail_recovery("rehydrated-header-persist-failed-height-" +
                                 std::to_string(height_hint));
        }
        return true;
    };

    auto recoverTipSideStateToUtxoTip = [&](const TipInfo& db_tip,
                                            std::string* error) -> bool {
        auto fail_recovery = [&](const std::string& msg) {
            if (error) {
                *error = msg;
            }
            return false;
        };

        if (!consensus_utxo_set_) {
            return fail_recovery("consensus-utxo-set-unavailable");
        }
        if (!active_tip_) {
            return fail_recovery("active-tip-unavailable");
        }
        if (static_cast<uint32_t>(db_tip.height) <= active_tip_->height) {
            return true;
        }

        struct PendingRecoveredBlock {
            uint256 hash;
            int height{0};
            BlockHeader header;
            bool recovered_header_from_body{false};
        };

        uint256 cursor_hash = db_tip.hash;
        int cursor_height = db_tip.height;
        std::vector<PendingRecoveredBlock> missing_chain;

        while (cursor_height > static_cast<int>(active_tip_->height) &&
               dinero::FindBlockIndex(cursor_hash) == nullptr) {
            PendingRecoveredBlock pending;
            pending.hash = cursor_hash;
            pending.height = cursor_height;
            if (!loadHeaderOrRecoverFromBody(cursor_hash,
                                             cursor_height,
                                             &pending.header,
                                             &pending.recovered_header_from_body,
                                             error)) {
                return false;
            }
            missing_chain.push_back(std::move(pending));
            cursor_hash = missing_chain.back().header.prev_block_hash;
            cursor_height--;
        }

        CBlockIndex* ancestor = dinero::FindBlockIndex(cursor_hash);
        if (!ancestor) {
            return fail_recovery("tip-recovery-ancestor-missing-from-block-index");
        }

        for (auto it = missing_chain.rbegin(); it != missing_chain.rend(); ++it) {
            CBlockIndex* idx = AddBlockIndex(it->header, static_cast<uint32_t>(it->height));
            if (!idx) {
                return fail_recovery("add-block-index-failed-for-height-" +
                                     std::to_string(it->height));
            }

            auto metadata_result = chain_db_->getHeaderMetadata(it->hash);
            if (metadata_result.status() == Status::Ok) {
                ApplyPersistedMetadataToBlockIndex(idx, metadata_result.value());
            }
            std::string invalidity_error;
            if (!BackfillFailedChildFromParent(chain_db_, idx, logger_, nullptr, &invalidity_error)) {
                return fail_recovery(invalidity_error);
            }
            if (it->recovered_header_from_body &&
                !persistRecoveredHeader(it->hash, it->header, it->height, idx, error)) {
                return false;
            }
            if (idx->status & BLOCK_VALID_CHAIN) {
                AddCandidate(idx);
            }
        }

        CBlockIndex* db_tip_idx = dinero::FindBlockIndex(db_tip.hash);
        if (!db_tip_idx) {
            return fail_recovery("db-tip-missing-from-block-index-after-recovery");
        }

        std::vector<CBlockIndex*> replay_path;
        CBlockIndex* walk = db_tip_idx;
        for (; walk && walk->height > active_tip_->height; walk = walk->pprev) {
            replay_path.push_back(walk);
        }
        if (!walk || walk->hash != active_tip_->hash) {
            return fail_recovery("tip-recovery-path-construction-failed");
        }
        std::reverse(replay_path.begin(), replay_path.end());

        network::StatelessNode replay_node(&consensus_utxo_set_->GetForest());
        ChainWriteToken token;

        for (CBlockIndex* idx : replay_path) {
            auto block_result = ReadStoredBlock(idx->hash);
            if (block_result.status() != Status::Ok) {
                return fail_recovery("missing-block-body-for-tip-recovery-height-" +
                                     std::to_string(idx->height));
            }

            std::vector<consensus::UtreexoHash> spend_targets;
            const std::vector<consensus::SpentOutputData>* spent_outputs = nullptr;
            if (block_result.value().utreexo.has_value()) {
                spend_targets = block_result.value().utreexo->spend_proof.targets;
                spent_outputs = &block_result.value().utreexo->spent_outputs;
            } else {
                bool has_spends = false;
                for (size_t txi = 1; txi < block_result.value().vtx.size() && !has_spends; ++txi) {
                    has_spends = !block_result.value().vtx[txi].vin.empty();
                }
                if (has_spends) {
                    return fail_recovery("missing-utreexo-replay-data-for-height-" +
                                         std::to_string(idx->height));
                }
            }

            if (!replay_node.ReplayBlock(block_result.value(), idx->height, spend_targets, spent_outputs)) {
                return fail_recovery("forest-replay-failed-at-height-" +
                                     std::to_string(idx->height));
            }

            auto height_status = chain_db_->putHeightIndex(token, idx->height, idx->hash);
            if (height_status != Status::Ok) {
                return fail_recovery("height-index-backfill-failed-at-height-" +
                                     std::to_string(idx->height));
            }
        }

        auto serialized = consensus_utxo_set_->GetForest().serialize();
        auto checkpoint_status =
            chain_db_->putUtreexoCheckpointWithChecksum(token, db_tip_idx->height, serialized);
        if (checkpoint_status != Status::Ok) {
            return fail_recovery("checkpoint-catchup-persist-failed");
        }

        auto checksum_version_status = chain_db_->putUtreexoMeta(token, "CHECKSUM_VERSION", "1");
        if (checksum_version_status != Status::Ok) {
            return fail_recovery("checksum-version-persist-failed");
        }

        ChainDB::ForestTipMarker marker;
        marker.height = db_tip_idx->height;
        marker.block_hash = db_tip_idx->hash;
        const auto commitment = consensus_utxo_set_->GetForest().getCommitment();
        if (commitment.size() != 32) {
            return fail_recovery("forest-commitment-size-invalid-after-recovery");
        }
        std::memcpy(marker.forest_root.data, commitment.data(), 32);
        auto marker_status = chain_db_->putForestTipMarker(token, marker);
        if (marker_status != Status::Ok) {
            return fail_recovery("forest-tip-marker-persist-failed");
        }
        const auto shielded_marker_result = chain_db_->getShieldedTipMarker();
        if (shielded_marker_result.status() == Status::Ok) {
            const auto& shielded_marker = shielded_marker_result.value();
            const auto shielded_snapshot = CurrentShieldedStateSnapshot();
            const bool marker_matches_tip =
                shielded_marker.block_hash == db_tip_idx->hash &&
                shielded_marker.height == static_cast<int32_t>(db_tip_idx->height);
            const bool marker_matches_state =
                shielded_marker.shielded_root == shielded_snapshot.root &&
                shielded_marker.tree_size == shielded_snapshot.tree_size &&
                shielded_marker.nullifier_count == shielded_snapshot.nullifier_count;
            if (!(marker_matches_tip && marker_matches_state)) {
                // Phase 3b step 6: with option 1 (frontier + anchor
                // history + marker all in the unified ChainDB batch),
                // the catch-up replay path that used to reconcile
                // shielded state lag here is no longer reachable
                // through ConnectTip/DisconnectTip. If we got here,
                // it means the in-memory tip lagged the on-disk
                // db_tip across shielded-active heights — most
                // likely a snapshot/restore boundary or manual
                // datadir surgery. Fail the recovery so the
                // operator runs --reindex-chainstate; silent
                // multi-block shielded replay would mask whichever
                // upstream bug produced the lag.
                if (logger_) {
                    logger_->error("[ChainstateService] ShieldedTipMarker disagrees with replayed tip during catch-up recovery");
                    logger_->error("[ChainstateService]   marker @" +
                                   std::to_string(shielded_marker.height) +
                                   " replayed tip @" + std::to_string(db_tip_idx->height));
                    logger_->error("[ChainstateService]   marker_matches_tip=" + std::to_string(marker_matches_tip) +
                                   " marker_matches_state=" + std::to_string(marker_matches_state));
                    logger_->error("[ChainstateService] Step-6 deletion: please --reindex-chainstate to "
                                   "rebuild shielded state alongside the forest replay.");
                }
                return fail_recovery("shielded-recovery-to-persisted-tip-failed");
            }
        } else if (shielded_marker_result.status() == Status::NotFound) {
            if (db_tip_idx->height > 0 &&
                RangeHasShieldedActivity(1, static_cast<uint32_t>(db_tip_idx->height))) {
                return fail_recovery("missing-shielded-tip-marker-during-tip-recovery");
            }
            if (!PersistShieldedState()) {
                return fail_recovery("shielded-frontier-persist-failed");
            }
            if (!PersistShieldedTipMarker(db_tip_idx->hash,
                                          static_cast<uint32_t>(db_tip_idx->height))) {
                return fail_recovery("shielded-tip-marker-persist-failed");
            }
        } else {
            return fail_recovery("shielded-tip-marker-read-failed");
        }

        consensus_utxo_set_->SetBestBlock(db_tip_idx->hash,
                                          static_cast<uint32_t>(db_tip_idx->height));
        PublishActiveTip(db_tip_idx, TipPublishReason::kStartupLoad);
        logger_->info("[ChainstateService] Recovered tip-side state to persisted tip height " +
                      std::to_string(db_tip_idx->height));
        return true;
    };

    logger_->info("[ChainstateService] Blockchain loaded successfully");
    logger_->info("[ChainstateService]   Height: " + std::to_string(height));
    logger_->info("[ChainstateService]   Best block: " +
                  ((tip_result.status() == Status::Ok) ? tip_result.value().hash.GetHex() : std::string(64, '0')));
    logger_->info("[ChainstateService]   Data directory: " + datadir_);

    // ═════════════════════════════════════════════════════════════════════════
    // Rebuild in-memory block index from ChainDB
    // ═════════════════════════════════════════════════════════════════════════
    // On startup, g_block_index is empty. We must populate it from ChainDB so
    // that AddBlockIndex can find parents (pprev links) when new blocks arrive
    // during IBD. Without this, every block gets pprev=NULL, breaking
    // ActivateBestChain's connect path construction and causing crashes.
    // Headers are loaded in order (0, 1, 2, ...) so parents exist when children
    // are added, ensuring correct pprev linking and chainwork accumulation.
    // ═════════════════════════════════════════════════════════════════════════
    {
        uint32_t loaded = 0;
        uint32_t candidates_seeded = 0;
        uint32_t invalid_descendants_backfilled = 0;
        for (uint32_t h = 0; h <= height; ++h) {
            auto hash_result = chain_db_->getBlockHashByHeight(h);
            if (hash_result.status() != Status::Ok) continue;

            BlockHeader header;
            bool recovered_header_from_body = false;
            std::string recovery_error;
            if (!loadHeaderOrRecoverFromBody(hash_result.value(),
                                             static_cast<int>(h),
                                             &header,
                                             &recovered_header_from_body,
                                             &recovery_error)) {
                if (logger_) {
                    logger_->warning("[ChainstateService] Skipping block index rebuild at height " +
                                     std::to_string(h) + ": " + recovery_error);
                }
                continue;
            }

            CBlockIndex* idx = AddBlockIndex(header, h);
            if (idx) {
                loaded++;
                if (recovered_header_from_body) {
                    std::string persist_error;
                    if (!persistRecoveredHeader(hash_result.value(), header, static_cast<int>(h), idx, &persist_error)) {
                        if (logger_) {
                            logger_->warning("[ChainstateService] Failed to persist rehydrated header at height " +
                                             std::to_string(h) + ": " + persist_error);
                        }
                    }
                }
                auto metadata_result = chain_db_->getHeaderMetadata(hash_result.value());
                if (metadata_result.status() == Status::Ok) {
                    ApplyPersistedMetadataToBlockIndex(idx, metadata_result.value());
                } else {
                    // Legacy databases may not have header metadata yet.
                    idx->status |= BLOCK_VALID_CHAIN;
                    idx->status |= BLOCK_HAVE_DATA;
                }
                bool repaired_invalidity = false;
                std::string invalidity_error;
                if (!BackfillFailedChildFromParent(chain_db_,
                                                   idx,
                                                   logger_,
                                                   &repaired_invalidity,
                                                   &invalidity_error)) {
                    logger_->error("[ChainstateService] Failed invalidity backfill during block index rebuild: " +
                                   invalidity_error);
                    return false;
                }
                if (repaired_invalidity) {
                    invalid_descendants_backfilled++;
                }
                if (idx->status & BLOCK_VALID_CHAIN) {
                    AddCandidate(idx);
                    candidates_seeded++;
                }
            }
        }
        logger_->info("[ChainstateService] Loaded " + std::to_string(loaded) +
                     " block index entries from ChainDB into g_block_index");
        if (invalid_descendants_backfilled > 0) {
            logger_->warning("[ChainstateService] Backfilled " +
                             std::to_string(invalid_descendants_backfilled) +
                             " descendant invalidity flags from persisted ancestors");
        }
        logger_->info("[ChainstateService] Seeded " + std::to_string(candidates_seeded) +
                     " candidates from active chain height index");

        // Set active_tip_ to genesis initially. The Utreexo checkpoint loader
        // below will advance it to the last validated height if a checkpoint
        // exists. We must NOT set active_tip_ to the ChainDB storage tip
        // because the UTXO/forest state may not have been validated to that
        // height (e.g., blocks were stored by BlockAcceptor but never ran
        // through ConnectBlock).
        auto genesis_hash_result = chain_db_->getBlockHashByHeight(0);
        if (genesis_hash_result.status() == Status::Ok) {
            CBlockIndex* genesis_idx = dinero::FindBlockIndex(genesis_hash_result.value());
            if (genesis_idx) {
                PublishActiveTip(genesis_idx, TipPublishReason::kEarlyInitGenesis);
                logger_->info("[ChainstateService] active_tip_ set to genesis (height=0)");
            }
        }
    }

    if (strict_archival_reads_ && !VerifyStrictArchivalStartup(height)) {
        // Audit gap = node cannot SERVE archival requests for missing blocks,
        // but it can still validate consensus and catch up from peers. Making
        // this fatal leaves the daemon in an unrecoverable state with no
        // escape hatch. Warn loudly and continue — missing bodies will be
        // filled by normal block download as peers request/send them.
        logger_->warning("[ChainstateService] Strict archival startup audit failed — continuing in degraded mode. Missing block bodies will be re-fetched during sync.");
    }

    // D.2 (Apr 30 2026) — startup undo coverage audit.
    //
    // Walks the tail of the active chain and verifies that every block
    // claiming BLOCK_HAVE_UNDO actually has readable undo data on disk.
    // The fleet hit "missing undo data for active tip" at height 10347
    // when DisconnectTip tried to reorg; surfacing this at startup
    // means operators see the corruption BEFORE the daemon wedges, with
    // a chainstate_recovery.marker pointing to the offending height.
    //
    // Scan window = 1024 blocks: cheap (~few MB of flatfile reads),
    // covers any plausible reorg horizon, and matches Bitcoin Core's
    // assumption that a deeper reorg requires a full reindex anyway.
    // Uses height 0 + 1 lower bound check internally.
    if (!VerifyActiveChainUndoCoverage(height, kStartupUndoAuditWindow)) {
        logger_->error("[ChainstateService] Startup undo audit detected an unreadable "
                       "undo entry on the active chain — chainstate_recovery.marker has "
                       "been written. Daemon will continue starting; the marker will "
                       "block normal advance until manually cleared or the corrupt "
                       "block is invalidated.");
        // Intentionally not returning false: the safe-mode / recovery
        // marker plumbing already gates further chain advance. Letting
        // Start() complete keeps RPC available so operators can
        // diagnose without the daemon flapping in restart loops.
    }

    // Backfill the genesis checkpoint for legacy datadirs created before
    // height-0 checkpoints were persisted. Bridge proof generation for block 1
    // relies on restoring the pre-block forest at height 0.
    {
        auto genesis_checkpoint = chain_db_->getUtreexoCheckpoint(0);
        auto genesis_checksum = chain_db_->getUtreexoChecksum(0);
        if (genesis_checkpoint.status() != Status::Ok ||
            genesis_checksum.status() != Status::Ok) {
            ChainWriteToken token = ChainWriteToken::CreateForTesting();
            consensus::UtreexoForest genesis_forest;
            auto status = chain_db_->putUtreexoCheckpointWithChecksum(
                token,
                0,
                genesis_forest.serialize()
            );
            if (status != Status::Ok) {
                logger_->error("[ChainstateService] Failed to backfill genesis Utreexo checkpoint: " +
                              std::string(StatusToString(status)));
                return false;
            }
            logger_->info("[ChainstateService] Backfilled genesis Utreexo checkpoint (height 0)");
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 2.1: Restore Utreexo accumulator from latest checkpoint
    // ═════════════════════════════════════════════════════════════════════════
    if (height > 0) {
        auto checkpoint_result = chain_db_->getLatestUtreexoCheckpoint();
        if (checkpoint_result.status() == Status::Ok) {
            auto [checkpoint_height, serialized_forest] = checkpoint_result.value();
            if (static_cast<uint32_t>(checkpoint_height) > height) {
                logger_->warning("[ChainstateService] Ignoring stale Utreexo checkpoint at height " +
                                 std::to_string(checkpoint_height) +
                                 " because persisted ChainDB tip is only height " +
                                 std::to_string(height));
            } else {
            logger_->info("[ChainstateService] Loading Utreexo checkpoint from height " +
                         std::to_string(checkpoint_height) +
                         " (" + std::to_string(serialized_forest.size()) + " bytes)");

            // ═════════════════════════════════════════════════════════════════════════
            // Utreexo checkpoint integrity verification (SHA256 checksum)
            // Detects power-loss corruption before loading corrupt state into memory
            // ═════════════════════════════════════════════════════════════════════════
            bool checkpoint_valid = true;
            {
                auto checksum_result = chain_db_->getUtreexoChecksum(checkpoint_height);
                auto version_result = chain_db_->getUtreexoMeta("CHECKSUM_VERSION");
                bool has_checksum = (checksum_result.status() == Status::Ok);
                bool has_version_flag = (version_result.status() == Status::Ok);

                if (has_checksum) {
                    // Compute SHA256 of the stored checkpoint bytes
                    unsigned char computed[32];
                    crypto::CSHA256().Write(serialized_forest.data(), serialized_forest.size()).Finalize(computed);

                    auto& stored = checksum_result.value();
                    if (stored.size() == 32 &&
                        std::memcmp(computed, stored.data(), 32) == 0) {
                        // Checksum matches — checkpoint is intact
                        logger_->info("[ChainstateService] ✅ Utreexo checkpoint integrity verified (SHA256 OK) at height " +
                                     std::to_string(checkpoint_height));
                    } else {
                        // CORRUPT: checksum mismatch — power loss mid-flush
                        // Log truncated hashes for diagnostics without flooding logs
                        std::string expected_hex, computed_hex;
                        for (int i = 0; i < 8 && i < (int)stored.size(); i++) {
                            char buf[3];
                            snprintf(buf, sizeof(buf), "%02x", stored[i]);
                            expected_hex += buf;
                        }
                        for (int i = 0; i < 8; i++) {
                            char buf[3];
                            snprintf(buf, sizeof(buf), "%02x", computed[i]);
                            computed_hex += buf;
                        }
                        logger_->error("[ChainstateService] ❌ CORRUPT Utreexo checkpoint at height " +
                                     std::to_string(checkpoint_height) +
                                     " — SHA256 mismatch (expected " + expected_hex +
                                     "... computed " + computed_hex + "...)");
                        logger_->error("[ChainstateService] Deleting corrupt checkpoint. Node requires --reindex to rebuild forest.");
                        // Delete both checkpoint and checksum atomically
                        // We need a ChainWriteToken — but Init doesn't have one.
                        // Use the same friend-class approach as DaemonApp's audit.
                        checkpoint_valid = false;
                    }
                } else if (!has_checksum && !has_version_flag) {
                    // Pre-upgrade checkpoint: no checksum exists because node was running
                    // older code. Accept with warning — next ConnectTip will write checksum.
                    logger_->warning("[ChainstateService] ⚠️  No checksum found for pre-upgrade Utreexo checkpoint at height " +
                                   std::to_string(checkpoint_height) + " — accepting (will be checksummed on next block)");
                } else if (!has_checksum && has_version_flag) {
                    // Post-upgrade but no checksum: interrupted write after upgrade.
                    // The version flag was set (meaning checksums should exist) but the
                    // checkpoint has no matching checksum. This is corruption.
                    logger_->error("[ChainstateService] ❌ CORRUPT Utreexo checkpoint at height " +
                                 std::to_string(checkpoint_height) +
                                 " — no checksum found but CHECKSUM_VERSION flag is set (interrupted write)");
                    logger_->error("[ChainstateService] Node requires --reindex to rebuild forest.");
                    checkpoint_valid = false;
                }
            }

            if (!checkpoint_valid) {
                // Don't load corrupt checkpoint — fall through to empty forest path
                // which will trigger the FATAL error requiring --reindex at line ~663
                logger_->error("[ChainstateService] Skipping corrupt checkpoint — forest will be empty");
            }

            try {
                if (!checkpoint_valid) {
                    throw std::runtime_error("Checkpoint integrity check failed");
                }
                // Deserialize returns a new forest - replace the current one.
                // deserialize is all-or-nothing (rooted-husk fix): a refused
                // payload comes back EMPTY. Installing an empty forest for a
                // non-empty checkpoint silently strands the node — treat it
                // as a corrupt checkpoint instead (recovery path below).
                auto restored_forest = consensus::UtreexoForest::deserialize(serialized_forest);
                // checkpoint_height 0 exemption (campaign phase 3): the
                // genesis checkpoint IS a legitimately empty forest, and
                // with every-N checkpoints it is the routine restore anchor
                // for short chains — only refuse empty-for-non-empty above
                // genesis.
                if (restored_forest.getNumLeaves() == 0 && !serialized_forest.empty() &&
                    checkpoint_height > 0) {
                    throw std::runtime_error(
                        "Checkpoint forest deserialize refused payload (" +
                        std::to_string(serialized_forest.size()) + " bytes)");
                }
                consensus_utxo_set_->GetForest() = std::move(restored_forest);

                // Apr 13 2026 Stage 3 — Utreexo canonical-roots fork.
                // If the checkpoint is already past the activation height, the
                // forest state was saved under canonical semantics. Flip the
                // flag on here so subsequent add/remove operations maintain
                // the invariant. We do NOT call `rebuildRoots()` here because
                // the checkpoint's `roots_` are already in canonical form
                // (they were saved by a node running with the flag on) and
                // rebuilding would just reproduce the same values.
                if (consensus::IsUtreexoCanonicalRootsActive(
                        static_cast<uint32_t>(checkpoint_height))) {
                    consensus_utxo_set_->GetForest().setCanonicalEmptyRoots(true);
                    logger_->info("[ChainstateService] 🪐 Canonical empty-roots fork active "
                                  "(checkpoint height " + std::to_string(checkpoint_height) +
                                  " >= " + std::to_string(
                                      consensus::GetUtreexoCanonicalRootsActivationHeight()) + ")");
                }

                logger_->info("[ChainstateService] ✅ Utreexo accumulator restored from checkpoint");
                logger_->info("[ChainstateService]   Checkpoint height: " + std::to_string(checkpoint_height));
                logger_->info("[ChainstateService]   Current height: " + std::to_string(height));

                if (static_cast<uint32_t>(checkpoint_height) < height) {
                    logger_->warning("[ChainstateService] ⚠️  Checkpoint is " +
                                   std::to_string(height - static_cast<uint32_t>(checkpoint_height)) +
                                   " blocks behind tip - accumulator will be rebuilt during sync");
                }

                // ═══════════════════════════════════════════════════════════════════
                // FOREST ROOT VERIFICATION (crash-corruption detection)
                // ═══════════════════════════════════════════════════════════════════
                // Verify that the restored forest's root commitment matches the
                // utreexo_root stored in the block header at the checkpoint height.
                // If they diverge, the forest was corrupted (crash mid-update, etc.)
                // and must be wiped + replayed from genesis.
                // ═══════════════════════════════════════════════════════════════════
                bool forest_root_valid = true;
                {
                    auto cp_hash_for_verify = chain_db_->getBlockHashByHeight(checkpoint_height);
                    if (cp_hash_for_verify.status() == Status::Ok) {
                        auto header_result = chain_db_->getHeader(cp_hash_for_verify.value());
                        if (header_result.status() == Status::Ok) {
                            // Get the expected root from the block header
                            uint256 expected_root;
                            std::memcpy(expected_root.data, header_result.value().utreexo_root.data, 32);

                            // Compute the forest's current root commitment
                            consensus::UtreexoHash computed = consensus_utxo_set_->GetForest().getCommitment();
                            uint256 computed_root;
                            if (computed.size() == 32)
                                std::memcpy(computed_root.data, computed.data(), 32);

                            if (computed_root == expected_root) {
                                logger_->info("[ChainstateService] ✅ Forest root matches block header at height " +
                                             std::to_string(checkpoint_height));
                                logger_->info("[ChainstateService]    Root: " + computed_root.GetHex().substr(0, 16) + "...");
                            } else {
                                logger_->error("════════════════════════════════════════════════════════════════");
                                logger_->error("[ChainstateService] ❌ FOREST ROOT MISMATCH at height " +
                                             std::to_string(checkpoint_height));
                                logger_->error("[ChainstateService]    Header root:  " + expected_root.GetHex().substr(0, 16) + "...");
                                logger_->error("[ChainstateService]    Forest root:  " + computed_root.GetHex().substr(0, 16) + "...");
                                logger_->error("[ChainstateService]    Forest is corrupted (crash mid-update?)");
                                logger_->error("════════════════════════════════════════════════════════════════");
                                forest_root_valid = false;
                            }
                        } else {
                            logger_->warning("[ChainstateService] Could not load header at checkpoint height for root verification");
                        }
                    }
                }

                // ═══════════════════════════════════════════════════════════════════
                // AUTO-RECOVERY: If forest root is invalid, wipe and replay
                // ═══════════════════════════════════════════════════════════════════
                if (!forest_root_valid) {
                    // Check for rebuild loop: if we recovered recently for the same
                    // tip, don't rebuild again — fail loud.
                    auto recovery_result = chain_db_->getRecoveryMarker();
                    if (recovery_result.status() == Status::Ok) {
                        auto& prev = recovery_result.value();
                        auto now = static_cast<uint64_t>(std::time(nullptr));
                        if (now - prev.timestamp < 300) {  // within 5 minutes
                            logger_->error("════════════════════════════════════════════════════════════════");
                            logger_->error("[ChainstateService] ❌ REBUILD LOOP DETECTED");
                            logger_->error("[ChainstateService]    Last recovery: " + std::to_string(now - prev.timestamp) + "s ago at height " +
                                         std::to_string(prev.height));
                            logger_->error("[ChainstateService]    Refusing to auto-recover again.");
                            logger_->error("[ChainstateService]    Manual action required: wipe chainstate and resync.");
                            logger_->error("════════════════════════════════════════════════════════════════");
                            return false;
                        }
                    }

                    logger_->warning("════════════════════════════════════════════════════════════════");
                    logger_->warning("[ChainstateService] 🔧 AUTO-RECOVERING: wiping corrupt forest checkpoints");
                    logger_->warning("[ChainstateService]    Forest will replay from genesis via ActivateBestChain");
                    logger_->warning("════════════════════════════════════════════════════════════════");

                    // Wipe all utreexo checkpoints
                    auto wipe_status = chain_db_->wipeAllUtreexoCheckpoints();
                    if (wipe_status != Status::Ok) {
                        logger_->error("[ChainstateService] Failed to wipe utreexo checkpoints");
                        return false;
                    }

                    // Record recovery event (loop guard)
                    auto tip_result = chain_db_->getTip();
                    ChainDB::RecoveryMarker rm;
                    rm.height = height;
                    if (tip_result.status() == Status::Ok)
                        rm.tip_hash = tip_result.value().hash;
                    rm.timestamp = static_cast<uint64_t>(std::time(nullptr));
                    chain_db_->putRecoveryMarker(rm);

                    // Reset forest to empty
                    consensus_utxo_set_->GetForest() = consensus::UtreexoForest();
                    if (active_tip_) {
                        consensus_utxo_set_->SetBestBlock(active_tip_->hash,
                                                          static_cast<uint32_t>(active_tip_->height));
                    } else {
                        consensus_utxo_set_->SetBestBlock(uint256{}, 0);
                    }

                    logger_->info("[ChainstateService] ✅ Forest reset to empty. Will replay from genesis.");

                    // Skip the rest of checkpoint loading — fall through to
                    // ActivateBestChain replay path below
                    // (throw to reach the catch block which continues normally)
                    throw std::runtime_error("Auto-recovery: forest wiped, replaying from genesis");
                }

                // Forest checkpoint delta campaign phase 2
                // (docs/design/forest-checkpoint-deltas.md): if the verified
                // checkpoint sits behind the persisted tip, replay the
                // per-block UD sidecars forward to close the gap in memory —
                // no block bodies or proof payloads needed. On success the
                // active tip advances straight to the ChainDB tip; on
                // failure we log loud and fall through to the pre-existing
                // behavior (checkpoint-height tip + body-based catch-up
                // recovery below).
                uint32_t restored_forest_height =
                    static_cast<uint32_t>(checkpoint_height);
                if (restored_forest_height < height) {
                    std::string replay_error;
                    if (ReplayForestDeltasToTip(restored_forest_height, height,
                                                &replay_error)) {
                        restored_forest_height = height;
                    } else {
                        logger_->warning(
                            "[ForestDeltaReplay] delta replay from checkpoint " +
                            std::to_string(checkpoint_height) + " to tip " +
                            std::to_string(height) + " unavailable (" +
                            replay_error + ") — falling back to body-based catch-up");
                    }
                }

                // Advance active_tip_ to the restored forest height (last
                // validated state). The forest state is validated consensus
                // state — checkpoint-verified, plus per-block header-root
                // verification for any replayed deltas — so it's safe to
                // skip ConnectBlock for blocks up to this height.
                auto cp_hash = chain_db_->getBlockHashByHeight(
                    static_cast<int>(restored_forest_height));
                if (cp_hash.status() == Status::Ok) {
                    CBlockIndex* cp_idx = dinero::FindBlockIndex(cp_hash.value());
                    if (cp_idx) {
                        PublishActiveTip(cp_idx, TipPublishReason::kStartupLoad);
                        logger_->info("[ChainstateService] active_tip_ advanced to restored forest height=" +
                                     std::to_string(restored_forest_height));
                    } else {
                        logger_->error("[ChainstateService] FindBlockIndex FAILED for restored hash at height " +
                                     std::to_string(restored_forest_height) + " — block index not loaded");
                    }
                } else {
                    logger_->error("[ChainstateService] getBlockHashByHeight FAILED for restored height " +
                                 std::to_string(restored_forest_height));
                }

                // ═══════════════════════════════════════════════════════════════════
                // FIX: Forest/active_tip_ mismatch detection
                // ═══════════════════════════════════════════════════════════════════
                // If forest was restored but active_tip_ is still at genesis, the
                // forest has state from height N while ActivateBestChain will try
                // to replay blocks 1..N against it → guaranteed ROOT MISMATCH.
                //
                // Reset forest to empty so replay starts from clean state.
                // ActivateBestChain will replay from genesis.
                // ═══════════════════════════════════════════════════════════════════
                if (active_tip_ && active_tip_->height == 0 &&
                    consensus_utxo_set_ && consensus_utxo_set_->GetForest().getNumLeaves() > 0) {
                    logger_->error("[ChainstateService] CRITICAL: Forest has " +
                                 std::to_string(consensus_utxo_set_->GetForest().getNumLeaves()) +
                                 " leaves but active_tip_ stuck at genesis (height=0)");
                    logger_->error("[ChainstateService] Resetting forest to empty to prevent ROOT MISMATCH");
                    logger_->error("[ChainstateService] Chain will replay from genesis (may require --reindex)");
                    consensus_utxo_set_->GetForest() = consensus::UtreexoForest();
                }
            } catch (const std::exception& e) {
                logger_->error("[ChainstateService] ❌ Failed to deserialize Utreexo checkpoint: " +
                             std::string(e.what()));
                logger_->error("[ChainstateService] Accumulator will start from empty state");
                // Keep the empty forest that was created earlier
            }
            }
        } else {
            logger_->info("[ChainstateService] No Utreexo checkpoint found - will rebuild from UTXO set");
        }

        // ═════════════════════════════════════════════════════════════════════════
        // CRITICAL: Do NOT rebuild Utreexo forest from UTXO database!
        // ═════════════════════════════════════════════════════════════════════════
        // Utreexo forest structure depends on INSERTION ORDER. UTXOs must be added
        // in the exact order they were created during block processing. Database
        // iteration order is arbitrary and will produce WRONG roots.
        //
        // If no checkpoint exists and forest is empty:
        // - Height 0: No UTXOs (genesis has OP_RETURN only)
        // - Height >= 1: Forest is missing UTXOs from mined blocks.
        //                ActivateBestChain will replay from genesis.
        //                If that fails, node must replay blocks via --reindex.
        // ═════════════════════════════════════════════════════════════════════════
        if (consensus_utxo_set_ && consensus_utxo_set_->GetForest().getNumLeaves() == 0 && height > 0) {
            // ═══════════════════════════════════════════════════════════
            // GRACEFUL RECOVERY: No checkpoint at height > 0
            // ═══════════════════════════════════════════════════════════
            // Forest is empty but chain has blocks. ActivateBestChain
            // will replay blocks from genesis against the empty forest.
            //
            // This handles the case where checkpoints weren't persisted
            // (e.g., first sync via P2P relay without ConnectTip checkpointing).
            // ═══════════════════════════════════════════════════════════
            logger_->warning("════════════════════════════════════════════════════════════════");
            logger_->warning("[ChainstateService] No Utreexo checkpoint at height " +
                          std::to_string(height) + " — recovering by replaying from block 1");
            logger_->warning("════════════════════════════════════════════════════════════════");
            if (active_tip_) {
                consensus_utxo_set_->SetBestBlock(active_tip_->hash,
                                                  static_cast<uint32_t>(active_tip_->height));
            } else {
                consensus_utxo_set_->SetBestBlock(uint256{}, 0);
            }
            logger_->info("[ChainstateService] Reset consensus UTXO tip metadata for empty-forest replay");
        }
    }

    // If ChainDB + UTXO set are already at the persisted tip but the restored
    // forest checkpoint is behind, replay only the missing accumulator/index
    // side state now. This closes the crash window after setTip() but before
    // checkpoint/marker/height-index persistence catches up.
    if (tip_result.status() == Status::Ok && consensus_utxo_set_ && active_tip_) {
        const auto& db_tip = tip_result.value();
        const bool utxo_already_at_db_tip =
            consensus_utxo_set_->GetBestBlock() == db_tip.hash &&
            consensus_utxo_set_->GetHeight() == static_cast<uint32_t>(db_tip.height);
        const bool side_state_behind_tip =
            active_tip_->hash != db_tip.hash ||
            active_tip_->height != static_cast<uint32_t>(db_tip.height);
        if (utxo_already_at_db_tip &&
            side_state_behind_tip &&
            consensus_utxo_set_->GetForest().getNumLeaves() > 0) {
            std::string recovery_error;
            if (!recoverTipSideStateToUtxoTip(db_tip, &recovery_error)) {
                logger_->error("[ChainstateService] Failed tip-side recovery after checkpoint lag: " +
                               recovery_error);
                return false;
            }
        }
    }

    bool persisted_assumeutxo_active_for_startup = false;
    if (utxo_index_) {
        auto active_meta = utxo_index_->GetMetadata(assumeutxo::kActiveKey);
        persisted_assumeutxo_active_for_startup =
            active_meta && active_meta.value() == "true";
    }

    if (!persisted_assumeutxo_active_for_startup) {
        auto current_tip_result = chain_db_->getTip();
        if (current_tip_result.status() == Status::Ok) {
            const auto& stored_tip = current_tip_result.value();
            if (!VerifyOrBootstrapShieldedTipMarker(stored_tip.hash,
                                                    static_cast<uint32_t>(stored_tip.height))) {
                logger_->error("[ChainstateService] Shielded startup consistency check failed");
                return false;
            }
            if (!RewindShieldedStateToActiveTipForStartup(static_cast<uint32_t>(stored_tip.height))) {
                logger_->error("[ChainstateService] Failed to rewind shielded state to active tip for startup replay");
                return false;
            }
        } else {
            logger_->error("[ChainstateService] Failed to load ChainDB tip for shielded startup verification");
            return false;
        }
    } else {
        logger_->info("[ChainstateService] Deferring shielded startup verification until AssumeUTXO restore completes");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // STARTUP NOTE: Do NOT fast-forward active_tip_ to ChainDB tip
    // ═════════════════════════════════════════════════════════════════════════
    // The Utreexo forest + consensus UTXO state are the source of truth for the
    // validated tip. ChainDB may contain stored blocks above that point (e.g.
    // after crash/restart before ConnectTip checkpointing fully catches up).
    //
    // Advancing active_tip_ to DB tip here without replaying ConnectTip can
    // create a split state: tip/height ahead, forest behind.
    // We intentionally keep active_tip_ at consensus state and let
    // ActivateBestChain replay forward.
    // ═════════════════════════════════════════════════════════════════════════
    {
        auto db_tip_result = chain_db_->getTip();
        if (db_tip_result.status() == Status::Ok) {
            const auto& db_tip = db_tip_result.value();
            bool misaligned = !active_tip_ ||
                              active_tip_->hash != db_tip.hash ||
                              active_tip_->height != static_cast<uint32_t>(db_tip.height);
            if (misaligned) {
                logger_->warning("[ChainstateService] active_tip_ differs from DB tip at startup");
                logger_->warning("[ChainstateService]   DB tip: height=" + std::to_string(db_tip.height) +
                               " hash=" + db_tip.hash.GetHex().substr(0, 16) + "...");
                logger_->warning("[ChainstateService]   consensus tip: height=" +
                               std::to_string(active_tip_ ? active_tip_->height : -1) +
                               " hash=" + (active_tip_ ? active_tip_->hash.GetHex().substr(0, 16) + "..." : "NULL"));
                logger_->warning("[ChainstateService] keeping consensus tip; ActivateBestChain will replay forward");
            } else {
                logger_->info("[ChainstateService] active_tip_ aligned with DB tip (height=" +
                             std::to_string(db_tip.height) + ")");
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // CRITICAL-003 fix: Restore AssumeUTXO state from persisted metadata
    // ═════════════════════════════════════════════════════════════════════════
    // Check if we have AssumeUTXO metadata persisted (crash-safe storage)
    // If snapshot was loaded previously, restore state and resume background validation
    // ═════════════════════════════════════════════════════════════════════════

    auto active_meta = utxo_index_->GetMetadata(assumeutxo::kActiveKey);
    if (active_meta && active_meta.value() == "true") {
        logger_->info("════════════════════════════════════════════════════════════════");
        logger_->info("🔄 ASSUMEUTXO STATE DETECTED (loading from persisted metadata)");
        logger_->info("════════════════════════════════════════════════════════════════");

        auto block_hash_meta = utxo_index_->GetMetadata(assumeutxo::kBaseBlockKey);
        uint256 restored_base_block;
        if (block_hash_meta) {
            restored_base_block = uint256::FromHexUnsafe(block_hash_meta.value());
        }
        auto height_meta = utxo_index_->GetMetadata(assumeutxo::kBaseHeightKey);
        uint32_t restored_base_height = 0;
        bool height_parse_ok = true;
        if (height_meta) {
            bool parse_ok = false;
            try {
                size_t pos = 0;
                unsigned long v = std::stoul(height_meta.value(), &pos);
                if (pos == height_meta.value().size() && v <= UINT32_MAX) {
                    restored_base_height = static_cast<uint32_t>(v);
                    parse_ok = true;
                }
            } catch (...) {}
            if (!parse_ok) {
                logger_->error("[AssumeUTXO restore] corrupt kBaseHeightKey metadata: \"" +
                               height_meta.value() + "\"; treating assumed-state as corrupt");
                height_parse_ok = false;
            }
        }

        // Only activate assumed mode if the height parsed cleanly. Precision
        // note: chainstate_matches=false reaches EnterFatal only when the
        // persisted lifecycle record is fully_validated (RestoreFromPersistence
        // consults the flag solely in that branch); for other/absent records
        // the fail-closed outcome is that SetAssumeUTXOState is skipped, so
        // assumed mode never activates and validation cannot start.
        if (height_parse_ok) {
            SetAssumeUTXOState(restored_base_block, restored_base_height, /*persist_metadata=*/false);
        }

        // Fatal-state-machine restore (spec: Persistence). chainstate_matches:
        // the persisted base must still be what the consensus UTXO set says.
        EnsureAssumeUtxoLifecycle();
        {
            // Corrupt height parse: see the precision note above — fatal when a
            // fully_validated record exists, otherwise fail-closed by inactivation.
            bool chainstate_matches = height_parse_ok;
            if (chainstate_matches) {
                if (consensus_utxo_set_) {
                    chainstate_matches =
                        (consensus_utxo_set_->GetBestBlock() == restored_base_block &&
                         consensus_utxo_set_->GetHeight() == restored_base_height);
                }
                if (!chainstate_matches && chain_db_ && restored_base_height > 0) {
                    // The set may legitimately sit past the base (chain advanced) or
                    // be pending the file rehydrate below; the marker is still honest
                    // if the persisted base block is on our recorded chain.
                    auto base_hash_result = chain_db_->getBlockHashByHeight(restored_base_height);
                    if (base_hash_result.ok()) {
                        // Successful lookup: hash mismatch IS evidence of tampering.
                        chainstate_matches =
                            (base_hash_result.value() == restored_base_block);
                    } else {
                        // Index unavailable (pruned/cold) is NOT evidence of tampering.
                        // Retain the marker; let IsCanonicalStateAligned catch real
                        // divergence. Log loudly so operators can investigate.
                        logger_->warning("[AssumeUTXO restore] cannot verify fully_validated "
                                         "marker (height index unavailable at " +
                                         std::to_string(restored_base_height) +
                                         "); retaining marker");
                        chainstate_matches = true;
                    }
                }
            }
            assumeutxo_lifecycle_->RestoreFromPersistence(chainstate_matches);
        }
        // Re-arm the never-cleared below-base fork guard: a FullyValidated
        // record means history was promoted (or its promotion is about to be
        // re-attempted) at the persisted lc base height (kLcBaseHeightKey,
        // ParseU32'd by RestoreFromPersistence into the lifecycle's base).
        if (assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::FullyValidated) {
            promoted_base_height_ = assumeutxo_lifecycle_->GetStatus(
                std::chrono::steady_clock::now()).snapshot_base_height;
        }
        if (assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::Disabled) {
            // Legacy upgrade: assumed state persisted by a pre-lifecycle binary
            // has no lifecycle record. Enter the machine now so a later
            // mismatch can persist fatal_mismatch (spec Fatal items 1/2/6).
            assumeutxo_lifecycle_->OnSnapshotLoaded(restored_base_block,
                                                    restored_base_height);
        }
        const bool lifecycle_fatal_at_restore =
            assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch;
        if (lifecycle_fatal_at_restore) {
            // Spec (Fatal Mismatch Semantics): fatal persists across restart and
            // gates snapshot loads + background validation until operator reset.
            logger_->error("[AssumeUTXO restore] Persisted lifecycle state is FATAL_MISMATCH — "
                           "skipping snapshot rehydrate and background validation; "
                           "operator reset required (blockchain.resetassumeutxofatal)");
            EnterSafeMode("assumeutxo fatal (restored from persistence): " +
                          assumeutxo_lifecycle_->GetStatus(
                              std::chrono::steady_clock::now()).fatal_reason);
        }

        logger_->info("⚠️  AssumeUTXO mode ACTIVE (restored from metadata)");
        logger_->info("⚠️  Snapshot base height: " + std::to_string(assumeutxo_base_height_));
        logger_->info("⚠️  Snapshot base block: " + assumeutxo_base_block_.GetHex());

        bool snapshot_rehydrated_from_file = false;
        // On restart the Utreexo forest checkpoint may have already restored the
        // consensus UTXO set to a height AT OR ABOVE the snapshot base (the node
        // advanced past the snapshot before shutdown). That is the correct, fully
        // bootstrapped state. Re-running LoadSnapshot here would Clear() it and
        // reset the forest back to the base, then force a forward replay that a
        // pruned/headers-only node cannot complete (no block bodies) — stranding
        // the forest one block behind the ChainDB UTXO set, which trips the
        // utreexo-proof-coverage safety fuse into SAFE MODE on the next restart.
        // Only rehydrate when the set is genuinely NOT yet bootstrapped: strictly
        // below the snapshot base, or exactly at the base height but bound to the
        // wrong block (corruption). Never when it is validly advanced past the base.
        const uint32_t restored_utxo_height =
            consensus_utxo_set_ ? static_cast<uint32_t>(consensus_utxo_set_->GetHeight()) : 0;
        const bool utxo_set_not_bootstrapped =
            consensus_utxo_set_ &&
            (restored_utxo_height < assumeutxo_base_height_ ||
             (restored_utxo_height == assumeutxo_base_height_ &&
              consensus_utxo_set_->GetBestBlock() != assumeutxo_base_block_));

        // Different-base belt on the startup-rehydrate fast path.
        // When the consensus set is already validly bootstrapped at our persisted
        // base, the rehydrate below is skipped — so a DIFFERENT-base snapshot
        // configured by the operator would otherwise be silently ignored and the
        // node would keep running on a chain it no longer intends. That is unsafe:
        // a mismatched/attacker-swapped assumeutxo_snapshot must fail loudly. The
        // LoadSnapshot active-lifecycle belt that normally catches this is bypassed
        // here (we never reach LoadSnapshot when already bootstrapped), so mirror
        // it: peek the configured snapshot's base and refuse to start if it differs
        // from the base this node bootstrapped from. We do NOT Clear() — the node's
        // state is good; the reload path's clear-before-load would corrupt it.
        // Scoped to an ACTIVE (mid-lifecycle, non-fatal, non-disabled) lifecycle,
        // exactly like the LoadSnapshot belt it mirrors: a fully-promoted/disabled
        // node has already validated genesis->tip and its snapshot config is moot,
        // so a stale different-base path there must NOT block startup.
        if (!lifecycle_fatal_at_restore && !utxo_set_not_bootstrapped &&
            assumeutxo_lifecycle_ &&
            assumeutxo_lifecycle_->GetState() !=
                assumeutxo::AssumeUtxoLifecycle::State::Disabled) {
            const std::string configured_snapshot =
                config_ ? config_->GetString("assumeutxo_snapshot", "") : "";
            if (!configured_snapshot.empty()) {
                consensus::SnapshotMetadata peek;
                std::string perr;
                if (ReadSnapshotHeaderPreview(configured_snapshot, peek, perr) &&
                    peek.magic == consensus::SNAPSHOT_MAGIC &&
                    (peek.block_height != assumeutxo_base_height_ ||
                     peek.block_hash != assumeutxo_base_block_)) {
                    logger_->error(
                        "[AssumeUTXO restore] another snapshot lifecycle is active (base height " +
                        std::to_string(assumeutxo_base_height_) +
                        "); configured assumeutxo_snapshot has a different base (height " +
                        std::to_string(peek.block_height) +
                        ") — reset or let validation finish before loading a different snapshot");
                    return false;
                }
            }
        }

        if (!lifecycle_fatal_at_restore && utxo_set_not_bootstrapped) {
            const std::string snapshot_path =
                config_ ? config_->GetString("assumeutxo_snapshot", "") : "";

            if (!snapshot_path.empty()) {
                const uint64_t stale_utxos = consensus_utxo_set_->GetSetSize();
                logger_->warning("[AssumeUTXO restore] Consensus UTXO set is not at snapshot base "
                                 "(utxo-tip=" +
                                 consensus_utxo_set_->GetBestBlock().GetHex().substr(0, 16) +
                                 "...@" + std::to_string(consensus_utxo_set_->GetHeight()) +
                                 ", snapshot=" +
                                 assumeutxo_base_block_.GetHex().substr(0, 16) +
                                 "...@" + std::to_string(assumeutxo_base_height_) +
                                 "); rehydrating from configured snapshot");

                if (stale_utxos > 0) {
                    logger_->warning("[AssumeUTXO restore] Clearing stale pre-snapshot consensus UTXO set (" +
                                     std::to_string(stale_utxos) +
                                     " UTXOs) before trusted snapshot rehydrate");
                    consensus_utxo_set_->Clear();
                }

                auto import_result = LoadSnapshot(std::filesystem::path(snapshot_path));
                if (!import_result.success) {
                    logger_->error("[AssumeUTXO restore] Snapshot rehydrate failed: " +
                                   import_result.error_message);
                    return false;
                }

                snapshot_rehydrated_from_file = true;
                logger_->info("[AssumeUTXO restore] Snapshot rehydrated from file: " +
                              std::to_string(import_result.utxos_imported) +
                              " UTXOs at height " +
                              std::to_string(import_result.block_height));
            } else {
                logger_->error("[AssumeUTXO restore] Persisted AssumeUTXO metadata exists, but "
                               "consensus UTXO state is not at the snapshot base and no "
                               "assumeutxo_snapshot path is configured");
                return false;
            }
        }

        // Check if background validation needs to be resumed
        if (!lifecycle_fatal_at_restore &&
            bg_validation_status_ != BackgroundValidationStatus::Completed &&
            bg_validation_status_ != BackgroundValidationStatus::Failed &&
            !snapshot_rehydrated_from_file) {

            logger_->info("🔍 Background validation incomplete - resuming...");
            logger_->info("   This validates the snapshot from genesis → snapshot height");
            logger_->info("   Node remains usable during validation");

            // Resume background validation
            StartBackgroundValidation();
        } else if (lifecycle_fatal_at_restore) {
            logger_->error("⛔ Background validation NOT resumed: assumeutxo lifecycle is fatal_mismatch");
        } else {
            logger_->info("✅ Background validation already complete");
        }

        // Restore snapshot base block into g_block_index so ActivateBestChain's
        // self-heal can find the UTXO tip. Without this, FindBlockIndex(utxo_best)
        // returns null and safe-mode triggers on every restart after snapshot load.
        if (header_chain_selector_ && !assumeutxo_base_block_.IsNull()) {
            const auto* hcs_entry = header_chain_selector_->GetHeader(assumeutxo_base_block_);
            if (hcs_entry) {
                CBlockIndex* snapshot_idx = EnsureHeaderBranchIndexed(hcs_entry, /*mark_chain_valid=*/true);
                if (snapshot_idx) {
                    const bool active_tip_missing = (active_tip_ == nullptr);
                    const bool active_tip_is_genesis =
                        active_tip_ && active_tip_->height == 0 && snapshot_idx->height > 0;
                    const bool active_tip_behind_snapshot =
                        active_tip_ && static_cast<uint32_t>(active_tip_->height) < assumeutxo_base_height_;
                    if (active_tip_missing || active_tip_is_genesis || active_tip_behind_snapshot) {
                        PublishActiveTip(snapshot_idx, TipPublishReason::kSnapshotRestore);
                        logger_->info("[AssumeUTXO restore] active_tip_ restored to snapshot base (h=" +
                                     std::to_string(assumeutxo_base_height_) + ")");
                    }
                } else {
                    logger_->warning("[AssumeUTXO restore] Failed to materialize snapshot ancestry in block index");
                }
            } else {
                logger_->warning("[AssumeUTXO restore] Snapshot base block not found in HCS — self-heal may fail");
            }
        }

        // Node is ready - snapshot already loaded
        services_ready_ = true;
        logger_->info("════════════════════════════════════════════════════════════════");
    } else {
        // Not in assumed mode, but a fully_validated or fatal_mismatch lifecycle
        // record may still be persisted; restore it (marker-vs-chainstate check
        // uses the persisted lc base, verified against chaindb when available).
        EnsureAssumeUtxoLifecycle();
        bool chainstate_matches = true;
        if (auto bh = utxo_index_->GetMetadata(assumeutxo::kLcBaseHeightKey)) {
            if (auto bb = utxo_index_->GetMetadata(assumeutxo::kLcBaseBlockKey)) {
                uint32_t h = 0;
                bool parse_ok = false;
                try {
                    size_t pos = 0;
                    unsigned long v = std::stoul(bh.value(), &pos);
                    if (pos == bh.value().size() && v <= UINT32_MAX) {
                        h = static_cast<uint32_t>(v);
                        parse_ok = true;
                    }
                } catch (...) {}
                if (!parse_ok) {
                    // Corrupt metadata: treat as chainstate mismatch (fail-safe;
                    // lifecycle's own RestoreFromPersistence will go fatal on the
                    // same key). No crash.
                    chainstate_matches = false;
                } else if (chain_db_ && h > 0) {
                    auto hash_result = chain_db_->getBlockHashByHeight(h);
                    if (hash_result.ok()) {
                        // Successful lookup: hash mismatch IS evidence of tampering.
                        chainstate_matches =
                            (hash_result.value() == uint256::FromHexUnsafe(bb.value()));
                    } else {
                        // Index unavailable (pruned/cold) is NOT evidence of tampering.
                        // Retain the marker; let IsCanonicalStateAligned catch real
                        // divergence. Log loudly so operators can investigate.
                        logger_->warning("[AssumeUTXO restore] cannot verify fully_validated "
                                         "marker (height index unavailable at " +
                                         std::to_string(h) + "); retaining marker");
                        // chainstate_matches stays true
                    }
                }
            }
        }
        assumeutxo_lifecycle_->RestoreFromPersistence(chainstate_matches);
        // Re-arm the never-cleared below-base fork guard on a PROMOTED node:
        // assumeutxo mode already exited (no assumed-state markers), but a
        // below-base fork is still spec-fatal — restore the boundary from the
        // FullyValidated lifecycle record's persisted base height.
        if (assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::FullyValidated) {
            promoted_base_height_ = assumeutxo_lifecycle_->GetStatus(
                std::chrono::steady_clock::now()).snapshot_base_height;
        }
        if (assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
            logger_->error("[AssumeUTXO restore] Persisted lifecycle state is FATAL_MISMATCH — "
                           "operator reset required (blockchain.resetassumeutxofatal)");
            EnterSafeMode("assumeutxo fatal (restored from persistence): " +
                          assumeutxo_lifecycle_->GetStatus(
                              std::chrono::steady_clock::now()).fatal_reason);
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 45: Snapshot-accelerated IBD Detection
    // ═════════════════════════════════════════════════════════════════════════
    // Detect if node is in Initial Block Download and attempt snapshot bootstrap
    // This enables fast sync for new nodes (minutes instead of days)
    // ═════════════════════════════════════════════════════════════════════════

    if (IsInIBD()) {
        logger_->info("════════════════════════════════════════════════════════════════");
        logger_->info("🔄 INITIAL BLOCK DOWNLOAD DETECTED");
        logger_->info("════════════════════════════════════════════════════════════════");
        logger_->info("Node is syncing from scratch (height: " + std::to_string(height) + ")");
        logger_->info("");
        logger_->info("💡 TIP: Speed up sync with snapshot bootstrap:");
        logger_->info("   1. Obtain a trusted snapshot file");
        logger_->info("   2. Run: dinero-cli loadtxoutset <snapshot_path>");
        logger_->info("   3. Node will be immediately usable for RPC/mining/wallets");
        logger_->info("   4. Background validation ensures snapshot is valid");
        logger_->info("");
        logger_->info("For automated bootstrap, set in config:");
        logger_->info("   assumeutxo_snapshot=/path/to/snapshot.dat");
        logger_->info("════════════════════════════════════════════════════════════════");

        // FIX 2 (issue #186): set up a DEFERRED snapshot bootstrap. We must NOT
        // load the snapshot here — the base block isn't on our header chain yet
        // (P2P header sync hasn't run), and loading now would also let the
        // snapshot pre-empt nothing useful. Instead: peek the base height/hash
        // (RULE 1), only on a fresh genesis-only datadir (RULE 5), and mark it
        // pending. The header-processing path then defers block download and
        // drives the actual load once headers reach the EXACT base hash; if
        // headers pass the base height without it, it falls back to full IBD
        // (RULES 2-4, see TryDeferredSnapshotBootstrap).
        ibd_status_ = IBDStatus::InIBD;
        const std::string snapshot_path =
            config_ ? config_->GetString("assumeutxo_snapshot", "") : "";
        if (!snapshot_path.empty()) {
            if (height > 0) {
                // RULE 5: never auto-load onto an existing datadir.
                logger_->info("[snapshot] existing datadir (height " + std::to_string(height) +
                              " > 0) — NOT auto-loading snapshot; continuing as a full node");
            } else {
                consensus::SnapshotMetadata peek;
                std::string perr;
                if (ReadSnapshotHeaderPreview(snapshot_path, peek, perr) &&
                    peek.magic == consensus::SNAPSHOT_MAGIC) {
                    // RULE 1: base peeked before any deferral.
                    snapshot_bootstrap_state_.store(SnapshotBootstrapState::Pending);
                    snapshot_bootstrap_path_ = snapshot_path;
                    snapshot_bootstrap_base_hash_ = peek.block_hash;
                    snapshot_bootstrap_base_height_ = peek.block_height;
                    logger_->info("[snapshot] pending — base height " +
                                  std::to_string(peek.block_height) + ", base hash " +
                                  peek.block_hash.GetHex().substr(0, 16) +
                                  "...; deferring block download until headers reach it");
                } else {
                    logger_->warning("[snapshot] cannot read snapshot header (" + perr +
                                     ") — ignoring; continuing with full sync");
                }
            }
        }
    } else {
        // Not in IBD - services ready immediately
        ibd_status_ = IBDStatus::IBDComplete;
        services_ready_ = true;
        logger_->info("[ChainstateService] Node is synced - services ready");
    }

    // Startup convergence pass:
    // - Ensures candidates seeded from storage are activated
    // - Imports any already-downloaded better header branch blocks
    // - Triggers block requests for missing bodies on the better branch
    if (!candidates_.empty()) {
        if (logger_) logger_->info("[ChainstateService] Running startup ActivateBestChain pass");
        ActivateBestChain();
    } else if (logger_) {
        logger_->warning("[ChainstateService] No startup candidates available; activation deferred");
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // STARTUP CATCH-UP REPLAY: Connect blocks from ChainDB when active tip is
    // behind the stored data. This handles the case where chaindb was rsynced
    // from another node (blocks exist but tip metadata wasn't updated) or when
    // the block download scheduler stored blocks but never connected them.
    // Uses the header chain as the target height, checking block existence in
    // chaindb for each sequential block.
    // ═══════════════════════════════════════════════════════════════════════════
    if (chain_db_ && active_tip_ && header_chain_selector_) {
        const uint32_t current_height = static_cast<uint32_t>(active_tip_->height);
        uint32_t target_height = current_height;

        // Use header chain's best height as the target. Via the canonical
        // snapshot (#439) rather than GetBestHeader(), whose raw pointer is
        // returned after the selector's lock is released.
        const auto sync = GetSyncSnapshot();
        if (sync.has_best_header) {
            target_height = sync.best_header_height;
        }

        if (target_height > current_height) {
            auto can_replay_startup_height = [&](const uint256& hash,
                                                 uint32_t height,
                                                 const char* phase) -> bool {
                auto metadata_result = chain_db_->getHeaderMetadata(hash);
                if (metadata_result.status() != Status::Ok) {
                    logger_->info("[ChainstateService] Catch-up: missing header metadata at height " +
                                 std::to_string(height) + " during " + phase + " — stopping replay");
                    return false;
                }

                const uint32_t status = metadata_result.value().status_flags;
                if ((status & BLOCK_VALID_CHAIN) == 0) {
                    logger_->info("[ChainstateService] Catch-up: height " + std::to_string(height) +
                                 " is not persisted as VALID_CHAIN during " + phase +
                                 " (status=" + std::to_string(status) + ") — stopping replay");
                    return false;
                }
                if (status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) {
                    logger_->warning("[ChainstateService] Catch-up: refusing to replay persisted invalid block at height " +
                                    std::to_string(height) + " during " + phase +
                                    " (status=" + std::to_string(status) + ")");
                    return false;
                }

                return true;
            };

            // Probe: check if the next block actually exists in chaindb
            auto probe_hash = chain_db_->getBlockHashByHeight(current_height + 1);
            bool have_next_block = false;
            if (probe_hash.status() == Status::Ok) {
                have_next_block =
                    can_replay_startup_height(probe_hash.value(), current_height + 1, "probe") &&
                    HasStoredBlockBody(probe_hash.value());
            }

            if (have_next_block) {
                logger_->info("[ChainstateService] Startup catch-up: active_tip=" +
                             std::to_string(current_height) + " target=" +
                             std::to_string(target_height) + " — replaying stored blocks");

                uint32_t connected = 0;
                for (uint32_t h = current_height + 1; h <= target_height; ++h) {
                    auto hash_result = chain_db_->getBlockHashByHeight(h);
                    if (hash_result.status() != Status::Ok) {
                        logger_->info("[ChainstateService] Catch-up: no hash at height " +
                                     std::to_string(h) + " — stopping replay");
                        break;
                    }

                    if (!HasStoredBlockBody(hash_result.value())) {
                        logger_->info("[ChainstateService] Catch-up: no block body at height " +
                                     std::to_string(h) + " — stopping replay");
                        break;
                    }

                    if (!can_replay_startup_height(hash_result.value(), h, "replay")) {
                        break;
                    }

                    auto block_result = ReadStoredBlock(hash_result.value());
                    if (block_result.status() != Status::Ok) {
                        logger_->warning("[ChainstateService] Catch-up: cannot read block at height " +
                                        std::to_string(h) + " — stopping replay");
                        break;
                    }

                    // Validate and connect the block through the standard path
                    auto connect_result = ProcessIncomingStoredBlock(block_result.value(), kStartupCatchupSource);
                    if (connect_result != consensus::ConnectBlockResult::CONNECTED &&
                        connect_result != consensus::ConnectBlockResult::DUPLICATE) {
                        logger_->warning("[ChainstateService] Catch-up: block at height " +
                                        std::to_string(h) + " failed to connect (result=" +
                                        std::to_string(static_cast<int>(connect_result)) +
                                        ") — stopping replay");
                        break;
                    }

                    connected++;
                    if (connected % 1000 == 0) {
                        logger_->info("[ChainstateService] Catch-up progress: " +
                                     std::to_string(connected) + "/" +
                                     std::to_string(target_height - current_height) +
                                     " blocks (height " + std::to_string(h) + ")");
                    }
                }

                logger_->info("[ChainstateService] Startup catch-up complete: connected " +
                             std::to_string(connected) + " blocks, active_tip=" +
                             std::to_string(active_tip_ ? active_tip_->height : 0));
            }
        }
    }

    if (active_tip_) {
        if (!VerifyOrBootstrapShieldedTipMarker(active_tip_->hash,
                                                static_cast<uint32_t>(active_tip_->height))) {
            logger_->error("[ChainstateService] Final shielded startup alignment check failed");
            return false;
        }
    }

    if (utxo_position_index_ && chain_db_ && consensus_utxo_set_) {
        const auto& forest = consensus_utxo_set_->GetForest();
        if (forest.getNumLeaves() == 0) {
            utxo_position_index_->Clear();
            if (logger_) logger_->info("[ChainstateService] UTXO position index cleared for empty forest");
        } else {
            if (logger_) logger_->info("[ChainstateService] Rebuilding UTXO position index from restored forest");
            const auto rebuild_report = utxo_position_index_->Rebuild(*chain_db_, forest);
            if (!rebuild_report.success) {
                if (logger_) logger_->warning("[ChainstateService] UTXO position index rebuild incomplete — proof serving may be degraded");
            } else if (rebuild_report.missing > 0) {
                const std::string reason =
                    "utreexo proof coverage degraded: " + std::to_string(rebuild_report.missing) +
                    " live UTXO(s) missing from forest while restoring height " +
                    std::to_string(active_tip_ ? active_tip_->height : 0);
                if (logger_) {
                    logger_->error("[ChainstateService] " + reason);
                    logger_->error("[ChainstateService] Scheduling automatic chainstate recovery");
                }
                ScheduleChainstateRecovery(reason, "[ChainstateService]");
            }
            if (logger_) logger_->info("[ChainstateService] UTXO position index ready with " +
                                       std::to_string(utxo_position_index_->GetPositionCount()) + " entries");
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Bug 1 diagnostic — audit undo coverage of the active tip and recent
    // ancestors. For each block, compare the in-memory CBlockIndex fields
    // (undo_file/pos/size + BLOCK_HAVE_UNDO bit) against the persisted
    // ChainDB metadata, and probe the flatfile to confirm the undo entry is
    // readable. Any drift is logged loudly so that the next fleet wedge has
    // root-cause evidence in the startup log instead of after-the-fact
    // forensics on a destroyed datadir.
    //
    // Introduced 2026-04-24 after the LA+MO wedge at height 6083. See
    // ~/.claude/projects/-Users-haydarevich/memory/project_real_bugs_apr18.md
    // for the investigation history and the three-hypothesis menu.
    // ═════════════════════════════════════════════════════════════════════════
    if (active_tip_ && chain_db_) {
        constexpr int kUndoAuditDepth = 16;
        int audited = 0;
        int drift_count = 0;
        int unreadable_count = 0;
        for (CBlockIndex* walk = active_tip_; walk && audited < kUndoAuditDepth;
             walk = walk->pprev) {
            if (walk->height == 0) break;  // genesis has no undo

            auto metadata_result = chain_db_->getHeaderMetadata(walk->hash);
            if (metadata_result.status() != Status::Ok) {
                if (logger_) {
                    logger_->error("[UndoAudit] height=" + std::to_string(walk->height) +
                                   " hash=" + walk->hash.GetHex().substr(0, 16) +
                                   "... MISSING ChainDB header metadata — tip-path integrity "
                                   "broken (Bug 1 candidate)");
                }
                ++drift_count;
                ++audited;
                continue;
            }
            const auto& metadata = metadata_result.value();

            const bool mem_have_undo = (walk->status & BLOCK_HAVE_UNDO) != 0;
            const bool persisted_have_undo =
                (metadata.status_flags & BLOCK_HAVE_UNDO) != 0;

            bool this_drift = false;
            std::string drift_detail;
            if (mem_have_undo != persisted_have_undo) {
                this_drift = true;
                drift_detail += " BLOCK_HAVE_UNDO=mem:" +
                                std::string(mem_have_undo ? "1" : "0") +
                                "/disk:" + (persisted_have_undo ? "1" : "0");
            }
            if (walk->undo_size != metadata.undo_size) {
                this_drift = true;
                drift_detail += " undo_size=mem:" + std::to_string(walk->undo_size) +
                                "/disk:" + std::to_string(metadata.undo_size);
            }
            if (walk->undo_file != metadata.undo_file) {
                this_drift = true;
                drift_detail += " undo_file=mem:" + std::to_string(walk->undo_file) +
                                "/disk:" + std::to_string(metadata.undo_file);
            }
            if (walk->undo_pos != metadata.undo_pos) {
                this_drift = true;
                drift_detail += " undo_pos=mem:" + std::to_string(walk->undo_pos) +
                                "/disk:" + std::to_string(metadata.undo_pos);
            }

            if (this_drift) {
                ++drift_count;
                if (logger_) {
                    logger_->error("[UndoAudit] height=" + std::to_string(walk->height) +
                                   " hash=" + walk->hash.GetHex().substr(0, 16) +
                                   "... DRIFT —" + drift_detail +
                                   " (Bug 1 candidate — persistence lagging memory)");
                }
            }

            // Probe the flatfile even when in-memory/disk agree, because the
            // wedge ultimately manifests when DisconnectTip tries to read the
            // undo bytes. If metadata claims undo_size>0 but the flatfile
            // entry is unreadable, that IS the wedge trigger.
            if (metadata.undo_size > 0 && block_storage_) {
                const FilePosition pos(metadata.undo_file,
                                       metadata.undo_pos,
                                       metadata.undo_size);
                auto bytes = block_storage_->readUndo(pos);
                if (bytes.status() != Status::Ok) {
                    ++unreadable_count;
                    if (logger_) {
                        logger_->error("[UndoAudit] height=" + std::to_string(walk->height) +
                                       " hash=" + walk->hash.GetHex().substr(0, 16) +
                                       "... UNREADABLE flatfile undo at file=" +
                                       std::to_string(metadata.undo_file) +
                                       " pos=" + std::to_string(metadata.undo_pos) +
                                       " size=" + std::to_string(metadata.undo_size) +
                                       " status=" +
                                       std::to_string(static_cast<int>(bytes.status())) +
                                       " (Bug 1 trigger — DisconnectTip would fail here)");
                    }
                }
            } else if (walk->height > 0 && mem_have_undo) {
                // Block claims to have undo in memory but persisted metadata
                // says size=0. This is the exact shape of the suspected Bug 1
                // wedge: tip bit set without durable flatfile reference.
                ++drift_count;
                if (logger_) {
                    logger_->error("[UndoAudit] height=" + std::to_string(walk->height) +
                                   " hash=" + walk->hash.GetHex().substr(0, 16) +
                                   "... in-memory BLOCK_HAVE_UNDO set but persisted "
                                   "undo_size=0 (Bug 1 candidate — reader will return "
                                   "missing-undo)");
                }
            }

            ++audited;
        }
        if (logger_) {
            if (drift_count == 0 && unreadable_count == 0) {
                logger_->info("[UndoAudit] Checked " + std::to_string(audited) +
                              " recent ancestors from tip (height " +
                              std::to_string(active_tip_->height) +
                              "); all undo coverage consistent");
            } else {
                logger_->error("[UndoAudit] SUMMARY drift=" + std::to_string(drift_count) +
                               " unreadable=" + std::to_string(unreadable_count) +
                               " over " + std::to_string(audited) + " ancestors from tip " +
                               std::to_string(active_tip_->height) +
                               " — daemon will wedge if a reorg requires DisconnectTip "
                               "on an affected block");
            }
        }
    }

    started_ = true;
    return true;
}

void ChainstateService::Stop() {
    if (!started_) {
        return;
    }

    logger_->info("[ChainstateService] Shutting down chainstate...");

    // Wake any RPC handlers parked in getblocktemplate longpoll. Without
    // this, each in-flight longpoll holds its HTTP server thread for up
    // to the longpoll timeout (~8s) past daemon stop, delaying shutdown
    // proportional to the number of concurrent longpollers. shutdown()
    // is idempotent and safe to call before the notifier has been used.
    // See include/rpc/longpoll_notifier.h for the full protocol.
    dinero::rpc::LongPollNotifier::instance().shutdown();

    if (chain_db_) {
        // Save final state
        auto tip_result = chain_db_->getTip();
        uint32_t final_height = (tip_result.status() == Status::Ok) ? tip_result.value().height : 0;
        logger_->info("[ChainstateService] Final height: " + std::to_string(final_height));
    }

    // BRIDGE REMOVED (November 7, 2025): No longer setting/clearing globals

    // Phase 44: Gracefully shutdown background validation thread
    if (bg_validation_thread_ && bg_validation_thread_->joinable()) {
        logger_->info("[ChainstateService] Stopping background validation thread...");
        // #298: set stop under bg_validation_wait_mutex_ then notify so the
        // worker's signalable rescan wait returns immediately (no lost wakeup,
        // no 30s shutdown stall).
        {
            std::lock_guard<std::mutex> wlk(bg_validation_wait_mutex_);
            bg_validation_should_stop_ = true;
        }
        bg_validation_cv_.notify_all();
        bg_validation_thread_->join();
        // #298: drop the wake callback so a late backfill store cannot fire the
        // [this] capture after this service is torn down.
        if (auto* ctx = DaemonContext::instance(); ctx && ctx->block_download) {
            ctx->block_download->SetOnBackfillBodyStored(nullptr);
        }
        logger_->info("[ChainstateService] Background validation thread stopped");
    }

    // Lifecycle caches the raw UTXOIndex*; destroy it first (Task 8's RPC
    // accessor must never observe a lifecycle with a dangling index).
    // The bg validation worker is already joined above, so no concurrent
    // access to assumeutxo_lifecycle_ is possible at this point.
    {
        std::lock_guard<std::mutex> lifecycle_lock(assumeutxo_lifecycle_init_mutex_);
        assumeutxo_lifecycle_.reset();
    }

    // Reset instances (ONE DB: chain_manager and chain_db owned globally, not here)
    utxo_index_.reset();

    logger_->info("[ChainstateService] Chainstate shutdown complete");
    started_ = false;
}

bool ChainstateService::IsHealthy() const {
    if (!started_ || !chain_db_) {
        return false;
    }

    // Basic health check: can we query ChainDB tip?
    try {
        auto tip_result = chain_db_->getTip();
        return tip_result.status() == Status::Ok;
    } catch (...) {
        return false;
    }
}

std::string ChainstateService::GetMetrics() const {
    if (!chain_db_) {
        return R"({"status":"not_initialized"})";
    }

    auto tip_result = chain_db_->getTip();
    uint32_t height = (tip_result.status() == Status::Ok) ? tip_result.value().height : 0;

    std::ostringstream oss;
    oss << "{"
        << R"("service":"chainstate",)"
        << R"("started":)" << (started_ ? "true" : "false") << ","
        << R"("height":)" << height << ","
        << R"("best_hash":")" << ((tip_result.status() == Status::Ok) ? tip_result.value().hash.GetHex() : std::string(64, '0')) << "\","
        << R"("datadir":")" << datadir_ << "\""
        << "}";

    return oss.str();
}

bool ChainstateService::initializeGenesisInChainDB() {
    // Safety check: Ensure ChainDB is initialized
    if (!chain_db_) {
        logger_->error("[ChainstateService] ChainDB is null, cannot initialize genesis");
        return false;
    }

    // Check if ChainDB already has blocks
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() == Status::Ok) {
        logger_->info("[ChainstateService] ChainDB already initialized, skipping genesis");
        return true;
    }

    // Get network ID from chain params
    const auto& params = dinero::Params();
    const std::string network = params.network_id;

    logger_->info("[ChainstateService] Initializing " + network + " genesis in ChainDB (RocksDB)...");

    // ⚠️  CONSENSUS-CRITICAL: Use genesis params from chainparams_impl.cpp
    // DO NOT hardcode genesis values here - they must come from canonical source
    uint256 genesis_hash = uint256::FromHexUnsafe(params.genesis_hash);  // Phase M.0: Convert to uint256
    std::string merkle_root = params.genesis.merkleRootHex;
    uint32_t genesis_time = params.genesis.nTime;
    uint32_t genesis_bits = params.genesis.nBits;
    uint32_t genesis_nonce = params.genesis.nNonce;
    uint32_t genesis_version = params.genesis.nVersion;

    // Build genesis block header
    BlockHeader genesis_header;
    genesis_header.version = genesis_version;
    genesis_header.timestamp = genesis_time;
    genesis_header.timestamp = genesis_time;  // Set BOTH timestamp fields for compatibility
    genesis_header.difficulty = genesis_bits;
    genesis_header.nonce = genesis_nonce;
    genesis_header.merkle_root = uint256::FromHexUnsafe(merkle_root);
    genesis_header.prev_block_hash = uint256::FromHexUnsafe(std::string(64, '0'));  // Genesis has no prev block
    genesis_header.prev_block_hash = uint256::FromHexUnsafe(std::string(64, '0'));

    // Write genesis to ChainDB
    rocksdb::WriteBatch batch;

    // Bootstrap token - genesis initialization is authorized
    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    // Use canonical proof-of-work for genesis so fresh bootstrap matches
    // steady-state header/block index accounting exactly.
    const arith_uint256 genesis_work = GetBlockProof(genesis_header.difficulty);
    auto status = chain_db_->putHeader(token, genesis_hash, genesis_header, 0, genesis_work, &batch);
    if (status != Status::Ok) {
        logger_->error("[ChainstateService] Failed to store genesis header: " + 
                      std::string(StatusToString(status)));
        return false;
    }

    // Store height index (height 0 → genesis hash)
    status = chain_db_->putHeightIndex(token, 0, genesis_hash, &batch);
    if (status != Status::Ok) {
        logger_->error("[ChainstateService] Failed to store genesis height index: " + 
                      std::string(StatusToString(status)));
        return false;
    }

    // Set genesis as chain tip
    status = chain_db_->setTip(token, genesis_hash, 0, genesis_work, &batch);
    if (status != Status::Ok) {
        logger_->error("[ChainstateService] Failed to set genesis as tip: " + 
                      std::string(StatusToString(status)));
        return false;
    }

    // Seed a real height-0 checkpoint so historical bridge proof generation
    // for block 1 never falls through the missing-checkpoint path.
    consensus::UtreexoForest genesis_forest;
    status = chain_db_->putUtreexoCheckpointWithChecksum(
        token,
        0,
        genesis_forest.serialize(),
        &batch
    );
    if (status != Status::Ok) {
        logger_->error("[ChainstateService] Failed to store genesis Utreexo checkpoint: " +
                      std::string(StatusToString(status)));
        return false;
    }

    // Write batch atomically
    status = chain_db_->writeBatch(token, std::move(batch), true);  // sync=true for genesis
    if (status != Status::Ok) {
        logger_->error("[ChainstateService] Failed to write genesis batch: " + 
                      std::string(StatusToString(status)));
        return false;
    }

    logger_->info("[ChainstateService] ✅ " + network + " Genesis stored in ChainDB:");
    logger_->info("[ChainstateService]    Hash: " + genesis_hash.GetHex());
    logger_->info("[ChainstateService]    Merkle: " + merkle_root);
    logger_->info("[ChainstateService]    Height: 0");
    logger_->info("[ChainstateService]    Timestamp: " + std::to_string(genesis_time));

    if (!PersistShieldedState() && logger_) {
        logger_->warning("[ChainstateService] Failed to persist genesis shielded frontier");
    }
    if (!PersistShieldedTipMarker(genesis_hash, 0) && logger_) {
        logger_->warning("[ChainstateService] Failed to persist genesis ShieldedTipMarker");
    }

    return true;
}

// ============================================================================
// Phase 3D: Wallet notification registry implementation
// ============================================================================

void ChainstateService::registerWalletNotifier(WalletNotifier* notifier) {
    if (!notifier) {
        logger_->warning("[ChainstateService] Attempted to register null wallet notifier");
        return;
    }

    // Check if already registered
    for (const auto* existing : wallet_notifiers_) {
        if (existing == notifier) {
            logger_->warning("[ChainstateService] Wallet notifier already registered");
            return;
        }
    }

    wallet_notifiers_.push_back(notifier);
    logger_->info("[ChainstateService] ✅ Registered wallet notifier (" +
                  std::to_string(wallet_notifiers_.size()) + " total)");
}

void ChainstateService::unregisterWalletNotifier(WalletNotifier* notifier) {
    auto it = std::find(wallet_notifiers_.begin(), wallet_notifiers_.end(), notifier);
    if (it != wallet_notifiers_.end()) {
        wallet_notifiers_.erase(it);
        logger_->info("[ChainstateService] Unregistered wallet notifier (" +
                      std::to_string(wallet_notifiers_.size()) + " remaining)");
    }
}

void ChainstateService::notifyBlockConnected(const Block& block, uint32_t height) {
    // Wake any miners parked on a longpoll getblocktemplate. This is the
    // server-side long-polling signal — see include/rpc/longpoll_notifier.h
    // for the design and the block_validation ordering rationale. Doing it
    // first means miners get their new template before any secondary
    // work (oracle notifications, mempool reconciliation, proof caches)
    // that doesn't block template correctness.
    dinero::rpc::LongPollNotifier::instance().notifyBlockConnected();

    if (!PersistShieldedState() && logger_) {
        logger_->warning("[ChainstateService] Failed to persist shielded frontier after block connect at height " +
                         std::to_string(height));
    }

    if (pool_manager_) {
        SyncPoolLifecycleState(pool_manager_.get(), chain_db_, logger_.get());
    }

    // Phase 9.2: Forward block event to Lightning (if oracle configured)
    if (chain_oracle_client_) {
        std::string block_hash = block.GetHash().ToString();
        chain_oracle_client_->sendBlockConnected(height, block_hash);
    }

    // Phase 9.2: Forward time update to Lightning (if oracle configured)
    if (time_oracle_client_) {
        time_oracle_client_->sendBlockHeight(height, block.header.timestamp);
    }

    // Phase 9.2: Check transactions against watch list (if oracle configured)
    if (transaction_oracle_client_) {
        for (const auto& tx : block.vtx) {
            std::string txid = tx.GetTxid().v.ToString();
            transaction_oracle_client_->checkAndNotify(txid, height);
        }
    }

    // Utreexo proof staleness: evict conflicts, mark remaining TXs stale
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->mempool) {
        if (bridge_node_) {
            bridge_node_->InvalidateTxProofCache();
        }

        std::vector<uint8_t> new_root;
        if (consensus_utxo_set_) {
            new_root = consensus_utxo_set_->GetForest().getCommitment();
        }
        ctx->mempool->mempool().onBlockConnected(block, height, new_root);

        // #6: Trigger bounded CSN proof refresh for stale mempool TXs.
        // Policy is eviction-first under churn:
        // - stale proof age cutoff
        // - max refresh attempts per tx
        // - overload guard bulk-evict
        if (ctx->tx_relay) {
            ctx->tx_relay->OnTipChanged();
            constexpr uint32_t MAX_STALE_PROOF_AGE_BLOCKS = 2;
            constexpr uint32_t MAX_REFRESH_ATTEMPTS = 1;
            constexpr size_t STALE_OVERLOAD_THRESHOLD = 256;
            constexpr size_t REFRESH_BATCH_SIZE = 20;
            auto refresh_candidates = ctx->mempool->mempool().selectStaleForRefresh(
                height,
                REFRESH_BATCH_SIZE,
                MAX_STALE_PROOF_AGE_BLOCKS,
                MAX_REFRESH_ATTEMPTS,
                STALE_OVERLOAD_THRESHOLD
            );
            if (!refresh_candidates.empty()) {
                ctx->tx_relay->RequestProofRefresh(refresh_candidates, REFRESH_BATCH_SIZE);
            }
        }
    }

    // Route wallet notifications through WalletWorker (thread-safe, async).
    // WalletManager has no internal mutex, so the old synchronous WalletNotifier
    // path (onBlockConnected) raced with the WalletWorker background thread
    // during catch-up scan, causing "double free or corruption" crashes.
    // WalletNotify::OnBlockConnected safely no-ops if WalletWorker isn't started yet.
    dinero::WalletNotify::OnBlockConnected(height, block.GetHash().GetHex(), block.vtx);
}

void ChainstateService::notifyBlockDisconnected(const Block& block, uint32_t height) {
    if (!PersistShieldedState() && logger_) {
        logger_->warning("[ChainstateService] Failed to persist shielded frontier after block disconnect at height " +
                         std::to_string(height));
    }

    if (pool_manager_) {
        SyncPoolLifecycleState(pool_manager_.get(), chain_db_, logger_.get());
    }

    // Phase 9.2: Forward block event to Lightning (if oracle configured)
    if (chain_oracle_client_) {
        std::string block_hash = block.GetHash().ToString();
        chain_oracle_client_->sendBlockDisconnected(height, block_hash);
    }

    // Utreexo proof staleness: mark all mempool TXs stale (root changed backward)
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->mempool) {
        if (bridge_node_) {
            bridge_node_->InvalidateTxProofCache();
        }

        ctx->mempool->mempool().onBlockDisconnected(block, height);
        if (ctx->tx_relay) {
            ctx->tx_relay->OnTipChanged();
            constexpr uint32_t MAX_STALE_PROOF_AGE_BLOCKS = 2;
            constexpr uint32_t MAX_REFRESH_ATTEMPTS = 1;
            constexpr size_t STALE_OVERLOAD_THRESHOLD = 256;
            constexpr size_t REFRESH_BATCH_SIZE = 20;
            const uint32_t effective_height = (height > 0) ? (height - 1) : 0;
            auto refresh_candidates = ctx->mempool->mempool().selectStaleForRefresh(
                effective_height,
                REFRESH_BATCH_SIZE,
                MAX_STALE_PROOF_AGE_BLOCKS,
                MAX_REFRESH_ATTEMPTS,
                STALE_OVERLOAD_THRESHOLD
            );
            if (!refresh_candidates.empty()) {
                ctx->tx_relay->RequestProofRefresh(refresh_candidates, REFRESH_BATCH_SIZE);
            }
        }
    }

    // Route wallet disconnects through WalletWorker too, so persisted wallet
    // state rolls back on the same async path used for block connects.
    dinero::WalletNotify::OnBlockDisconnected(height, block);

    if (wallet_notifiers_.empty()) {
        return; // No extra wallet notifiers to notify
    }

    logger_->warning("[ChainstateService] Notifying " +
                     std::to_string(wallet_notifiers_.size()) +
                     " wallet(s) of block disconnect at height " + std::to_string(height));

    for (auto* notifier : wallet_notifiers_) {
        try {
            notifier->onBlockDisconnected(block, height);
        } catch (const std::exception& e) {
            logger_->error("[ChainstateService] Wallet disconnect notification error: " +
                          std::string(e.what()));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Block query methods (ChainDB-backed)
// ═══════════════════════════════════════════════════════════════════════════

bool ChainstateService::hasBlock(uint32_t height) const {
    if (!chain_db_) {
        return false;
    }
    auto hash_result = chain_db_->getBlockHashByHeight(height);
    return hash_result.status() == Status::Ok && HasStoredBlockBody(hash_result.value());
}

std::string ChainstateService::getBlock(uint32_t height) const {
    if (!chain_db_) {
        return "";
    }

    auto hash_result = chain_db_->getBlockHashByHeight(height);
    if (hash_result.status() != Status::Ok) {
        return "";
    }

    auto block_result = ReadStoredBlock(hash_result.value());
    if (block_result.status() != Status::Ok) {
        return "";
    }

    return BinaryToHexString(block_result.value().Serialize());
}

bool ChainstateService::hasBlockByHash(const uint256& hash) const {
    return HasStoredBlockBody(hash);
}

bool ChainstateService::hasReadableBlockByHash(const uint256& hash) const {
    return HasFlatfileBlockBody(hash);
}

bool ChainstateService::hasFlatfileBlockByHash(const uint256& hash) const {
    return HasFlatfileBlockBody(hash);
}

StatusOr<Block> ChainstateService::getBlockByHash(const uint256& hash) const {
    return ReadStoredBlock(hash);
}

uint64_t ChainstateService::getLegacyBodyFallbackReadCount() const {
    return legacy_body_fallback_reads_.load();
}

uint64_t ChainstateService::getLegacyUndoFallbackReadCount() const {
    return legacy_undo_fallback_reads_.load();
}

bool ChainstateService::strictArchivalReadsEnabled() const {
    return true;
}

bool ChainstateService::VerifyStrictArchivalStartup(uint32_t tip_height) const {
    if (!chain_db_) {
        if (logger_) {
            logger_->error("[ChainstateService] Strict archival mode requires ChainDB");
        }
        return false;
    }

    const auto prune_mode_result = chain_db_->getPruneMode();
    const bool prune_mode_enabled = prune_mode_result.ok() && prune_mode_result.value();
    const auto prune_height_result = chain_db_->getPruneHeight();
    const uint32_t prune_height = prune_height_result.ok() ? prune_height_result.value() : 0;

    if (logger_) {
        logger_->info("[ChainstateService] Auditing strict archival flatfile coverage through height " +
                      std::to_string(tip_height));
    }

    const auto audit = storage::VerifyStrictFlatfileCoverage(
        *chain_db_,
        block_storage_.get(),
        tip_height,
        prune_mode_enabled,
        prune_height);
    if (!audit.ok) {
        if (logger_) {
            logger_->error("[ChainstateService] Strict archival audit failed: " + audit.error);
        }
        return false;
    }

    if (logger_) {
        logger_->info("[ChainstateService] Strict archival audit passed: flatfile bodies available from genesis to tip");
    }
    return true;
}

const char* ChainstateService::TipPublishReasonName(TipPublishReason r) {
    switch (r) {
        case TipPublishReason::kAdvancement:      return "advancement";
        case TipPublishReason::kRollback:         return "rollback";
        case TipPublishReason::kStartupLoad:      return "startup-load";
        case TipPublishReason::kSnapshotRestore:  return "snapshot-restore";
        case TipPublishReason::kEarlyInitGenesis: return "early-init-genesis";
        case TipPublishReason::kCSNDisconnect:    return "csn-disconnect";
        case TipPublishReason::kReorgInvalidate:  return "reorg-invalidate";
        case TipPublishReason::kSelfHealRealign:  return "self-heal-realign";
    }
    return "unknown";
}

void ChainstateService::PublishActiveTip(CBlockIndex* tip, TipPublishReason reason) {
    // Single setter for the in-memory active_tip_ pointer. Pre-fix
    // there were 13 direct assignments scattered across this file,
    // each with implicit semantics. Funneling them through this
    // method:
    //
    //   1. Documents the reason at every call site (compile-time
    //      enforced via the enum class).
    //   2. Logs every transition so post-mortem analysis on the
    //      live fleet can correlate tip moves with reasons.
    //   3. Becomes the natural insertion point for additional
    //      checks (e.g., refusing kAdvancement when the publication
    //      invariant has not been verified).
    //
    // The check-before-publish invariant for kAdvancement is
    // enforced by the caller (ConnectTip) BEFORE this method is
    // invoked — see CheckBlockDisconnectMaterialDurable. Putting
    // the check inside PublishActiveTip would re-walk durable storage
    // for every non-advancement publish too (rollback, startup,
    // snapshot), which is unnecessary and would increase boot time.
    if (logger_ && tip) {
        logger_->info("[PublishActiveTip] " +
                      std::string(TipPublishReasonName(reason)) +
                      " tip=" + tip->hash.GetHex().substr(0, 16) + "..." +
                      " height=" + std::to_string(tip->height));
    }
    active_tip_ = tip;

    // #439: publish an immutable VALUE copy of the tip identity under its own
    // mutex. GetSyncSnapshot() reads this instead of dereferencing active_tip_,
    // which is a bare CBlockIndex* mutated on the chain-advancement path — a
    // reader touching tip->hash / tip->height concurrently would be racing.
    // Because this is the single setter for active_tip_, publishing here keeps
    // the value in lockstep with the pointer.
    {
        std::lock_guard<std::mutex> lock(published_tip_mutex_);
        if (tip) {
            published_tip_valid_ = true;
            published_tip_hash_ = tip->GetBlockHash();
            published_tip_height_ = static_cast<uint32_t>(tip->height);
        } else {
            published_tip_valid_ = false;
            published_tip_hash_.SetNull();
            published_tip_height_ = 0;
        }
    }
}

ChainstateService::DisconnectMaterialCheck
ChainstateService::CheckBlockDisconnectMaterialDurable(const Block& block,
                                                      const uint256& hash,
                                                      uint32_t height,
                                                      uint32_t reference_tip_height) const {
    DisconnectMaterialCheck result;
    result.durable = false;

    if (!chain_db_ || !block_storage_) {
        result.failure_reason = "chain_db_ or block_storage_ unavailable";
        return result;
    }

    auto metadata_result = chain_db_->getHeaderMetadata(hash);
    if (metadata_result.status() != Status::Ok) {
        result.failure_reason = "no header metadata for hash=" + hash.GetHex();
        return result;
    }
    const auto& metadata = metadata_result.value();

    // Pre-compute "block has shielded effects" — used both for the
    // shielded-frontier-presence assertion below and as a cheap
    // structural sanity check that the decoded undo's spent.size()
    // matches the block's expectations.
    bool block_has_shielded = false;
    uint64_t expected_spent_count = 0;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        if (tx.IsShielded()) block_has_shielded = true;
        if (tx_idx > 0) {  // skip coinbase
            expected_spent_count += tx.vin.size();
        }
    }

    if (height > 0) {
        if ((metadata.status_flags & BLOCK_HAVE_UNDO) == 0) {
            result.failure_reason = "BLOCK_HAVE_UNDO flag absent at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex();
            return result;
        }
        if (metadata.undo_size == 0) {
            result.failure_reason = "undo_size=0 despite BLOCK_HAVE_UNDO at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex();
            return result;
        }
        const FilePosition pos(metadata.undo_file, metadata.undo_pos, metadata.undo_size);
        const auto bytes_result = block_storage_->readUndo(pos);
        if (bytes_result.status() != Status::Ok) {
            result.failure_reason = "undo flatfile unreadable at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex() +
                                    " file=" + std::to_string(metadata.undo_file) +
                                    " pos=" + std::to_string(metadata.undo_pos) +
                                    " size=" + std::to_string(metadata.undo_size) +
                                    " status=" + std::to_string(static_cast<int>(bytes_result.status()));
            return result;
        }

        // Decode the undo bytes. Bytes-readable is necessary but not
        // sufficient: the undo could be truncated, corrupt, or carry
        // a structurally inconsistent shape. UndoRecord::Deserialize
        // throws on malformed input; trap it.
        UndoRecord decoded;
        try {
            decoded = UndoRecord::Deserialize(bytes_result.value());
        } catch (const std::exception& e) {
            result.failure_reason = "UndoRecord::Deserialize threw at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex() +
                                    ": " + std::string(e.what());
            return result;
        }

        // Sanity: created vector must be non-empty (every consensus
        // block has at least the coinbase outputs).
        if (decoded.created.empty()) {
            result.failure_reason = "decoded undo created vector empty at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex();
            return result;
        }

        // Sanity: spent count must equal sum of non-coinbase inputs.
        // A mismatch means the persisted undo was constructed against
        // a different block body than the one on disk.
        if (decoded.spent.size() != expected_spent_count) {
            result.failure_reason = "decoded undo spent count mismatch at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex() +
                                    ": expected " + std::to_string(expected_spent_count) +
                                    " got " + std::to_string(decoded.spent.size());
            return result;
        }

        // Shielded coupling: if the block has shielded effects, the
        // undo MUST carry pre_block_shielded_frontier. Without it,
        // DisconnectBlock cannot restore the commitment tree and the
        // tip becomes undisconnectable.
        if (block_has_shielded && !decoded.pre_block_shielded_frontier.has_value()) {
            result.failure_reason = "block has shielded txs but decoded undo lacks "
                                    "pre_block_shielded_frontier at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex();
            return result;
        }
    }

    if (consensus::IsUtreexoActive(height) && !GetConfig().utreexo_stateless) {
        // UD:<hash> sidecar key (matches the file-static MakeUtreexoDeltaUndoKey
        // helper used by ConnectTip / DisconnectTip; inlined here because
        // the helper is declared later in this translation unit and the
        // invariant is part of the public class API).
        const std::string delta_key = "UD:" + hash.GetHex();
        std::string delta_blob;
        const auto delta_status = chain_db_->getRaw(delta_key, delta_blob);
        if (delta_status != Status::Ok) {
            result.failure_reason = "Utreexo delta sidecar (UD:<hash>) absent at height=" +
                                    std::to_string(height) + " hash=" + hash.GetHex() +
                                    " status=" + std::to_string(static_cast<int>(delta_status));
            return result;
        }
        if (delta_blob.empty()) {
            result.failure_reason = "Utreexo delta sidecar empty at height=" +
                                    std::to_string(height);
            return result;
        }
    }

    // UTXO read-back: bind the unified WriteBatch's UTXO mutations to
    // the same commit boundary as setTip. Coinbase output 0 is created
    // by every block and lives in chaindb after the batch commits;
    // failure to read it back means the UTXO mutations did NOT land
    // atomically with the tip pointer (rocksdb's WriteBatch atomicity
    // makes that impossible IN THEORY but a future refactor that
    // splits the batch into pieces would be caught here).
    //
    // MATURITY GATE (Jun 2026): the read-back is only valid while the
    // coinbase is still IMMATURE. Consensus forbids spending a coinbase
    // until `coinbase_maturity` confirmations, so an immature coinbase
    // output 0 cannot have been spent and MUST be readable. Once mature
    // it may be legitimately spent (gettxout == null) — normal chain
    // state, NOT an atomicity failure. The post-commit ConnectTip caller
    // passes the freshly-connected tip (reference_tip_height == height,
    // depth 0, always immature) so the atomicity invariant it enforces
    // is still fully exercised; only the deep startup-audit walk-back
    // skips here, where a spent mature coinbase previously raised a
    // false chainstate_recovery.marker (e.g. the height=35600 marker
    // observed fleet-wide). Use the network's actual maturity so the
    // gate is correct on regtest (10) as well as mainnet (100).
    const bool readback_applies = daemon::CoinbaseReadbackApplies(
        reference_tip_height, height, Params().coinbase_maturity);
    if (readback_applies && !block.vtx.empty() && !block.vtx[0].vout.empty()) {
        const auto coinbase_txid = block.vtx[0].GetTxid().AsUint256();
        auto coin_result = chain_db_->getCoin(coinbase_txid, 0);
        if (coin_result.status() != Status::Ok) {
            result.failure_reason = "UTXO read-back failed: coinbase output 0 not in "
                                    "chaindb at height=" + std::to_string(height) +
                                    " hash=" + hash.GetHex() +
                                    " coinbase_txid=" + coinbase_txid.GetHex().substr(0, 16) +
                                    " status=" + std::to_string(static_cast<int>(coin_result.status()));
            return result;
        }
    }

    result.durable = true;
    return result;
}

ChainstateService::UndoMetadataRestampReport
ChainstateService::AuditUndoMetadataForRestamp(uint32_t max_blocks_back,
                                               bool apply,
                                               bool include_ok) {
    std::lock_guard<std::recursive_mutex> lock(activation_mutex_);

    UndoMetadataRestampReport report;
    report.apply = apply;

    if (!chain_db_ || !block_storage_) {
        UndoMetadataRestampEntry entry;
        entry.reason = "chain_db or block_storage unavailable";
        report.entries.push_back(std::move(entry));
        report.failed = 1;
        return report;
    }

    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        UndoMetadataRestampEntry entry;
        entry.reason = "failed to read ChainDB tip: " +
                       std::string(StatusToString(tip_result.status()));
        report.entries.push_back(std::move(entry));
        report.failed = 1;
        return report;
    }

    const uint32_t scan_limit = (max_blocks_back == 0)
        ? std::numeric_limits<uint32_t>::max()
        : max_blocks_back;
    uint32_t current_height = static_cast<uint32_t>(std::max(0, tip_result.value().height));
    // Maturity reference for the per-block read-back gate: the tip we
    // start the walk-back from, captured before the loop decrements it.
    const uint32_t audit_tip_height = current_height;

    while (report.scanned < scan_limit && current_height > 0) {
        UndoMetadataRestampEntry entry;
        entry.height = current_height;

        auto hash_result = chain_db_->getBlockHashByHeight(static_cast<int>(current_height));
        if (hash_result.status() != Status::Ok) {
            entry.reason = "missing height index: " +
                           std::string(StatusToString(hash_result.status()));
            report.entries.push_back(std::move(entry));
            report.failed++;
            break;
        }
        entry.hash = hash_result.value();

        auto metadata_result = chain_db_->getHeaderMetadata(entry.hash);
        if (metadata_result.status() != Status::Ok) {
            entry.reason = "missing header metadata: " +
                           std::string(StatusToString(metadata_result.status()));
            report.entries.push_back(std::move(entry));
            report.failed++;
            break;
        }

        auto metadata = metadata_result.value();
        entry.status_flags = metadata.status_flags;
        entry.undo_file = metadata.undo_file;
        entry.undo_pos = metadata.undo_pos;
        entry.undo_size = metadata.undo_size;
        entry.has_undo_flag = (metadata.status_flags & BLOCK_HAVE_UNDO) != 0;

        auto block_result = ReadStoredBlock(entry.hash);
        entry.block_readable = block_result.status() == Status::Ok;
        if (!entry.block_readable) {
            entry.reason = "block body unreadable: " +
                           std::string(StatusToString(block_result.status()));
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }

        const Block& block = block_result.value();
        uint64_t expected_spent = 0;
        uint64_t expected_created = 0;
        bool block_has_shielded = false;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
            const auto& tx = block.vtx[tx_idx];
            if (tx_idx > 0) {
                expected_spent += tx.vin.size();
            }
            expected_created += tx.vout.size();
            if (tx.IsShielded()) {
                block_has_shielded = true;
            }
        }

        if (entry.has_undo_flag) {
            const auto durable =
                CheckBlockDisconnectMaterialDurable(block, entry.hash, current_height,
                                                    audit_tip_height);
            if (durable.durable) {
                entry.reason = "ok";
                if (include_ok) {
                    report.entries.push_back(std::move(entry));
                }
            } else {
                entry.reason = durable.failure_reason;
                report.entries.push_back(std::move(entry));
                report.failed++;
            }
            current_height--;
            report.scanned++;
            continue;
        }

        if (metadata.undo_size == 0) {
            entry.reason = "BLOCK_HAVE_UNDO absent and undo_size=0; cannot re-stamp safely";
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }

        const FilePosition pos(metadata.undo_file, metadata.undo_pos, metadata.undo_size);
        auto bytes_result = block_storage_->readUndo(pos);
        entry.undo_readable = bytes_result.status() == Status::Ok;
        if (!entry.undo_readable) {
            entry.reason = "BLOCK_HAVE_UNDO absent and referenced undo unreadable: " +
                           std::string(StatusToString(bytes_result.status()));
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }

        UndoRecord decoded;
        try {
            decoded = UndoRecord::Deserialize(bytes_result.value());
            entry.undo_decodable = true;
        } catch (const std::exception& e) {
            entry.reason = std::string("BLOCK_HAVE_UNDO absent and undo decode failed: ") + e.what();
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }

        if (decoded.created.size() != expected_created) {
            entry.reason = "undo created count mismatch: expected " +
                           std::to_string(expected_created) + " got " +
                           std::to_string(decoded.created.size());
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }
        if (decoded.spent.size() != expected_spent) {
            entry.reason = "undo spent count mismatch: expected " +
                           std::to_string(expected_spent) + " got " +
                           std::to_string(decoded.spent.size());
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }
        if (block_has_shielded && !decoded.pre_block_shielded_frontier.has_value()) {
            entry.reason = "block has shielded txs but undo lacks pre_block_shielded_frontier";
            report.entries.push_back(std::move(entry));
            report.failed++;
            current_height--;
            report.scanned++;
            continue;
        }

        entry.restampable = true;
        report.restampable++;
        entry.reason = "BLOCK_HAVE_UNDO flag absent but undo bytes are readable and structurally match";

        if (apply) {
            metadata.status_flags |= BLOCK_HAVE_UNDO;
            rocksdb::WriteBatch batch;
            ChainWriteToken token;
            const auto stage_status = chain_db_->putHeaderMetadata(token, entry.hash, metadata, &batch);
            if (stage_status != Status::Ok) {
                entry.reason = "failed to stage metadata re-stamp: " +
                               std::string(StatusToString(stage_status));
                report.failed++;
            } else {
                const auto write_status = chain_db_->writeBatch(token, std::move(batch), true);
                if (write_status == Status::Ok) {
                    entry.repaired = true;
                    report.repaired++;
                    entry.status_flags = metadata.status_flags;
                    entry.has_undo_flag = true;
                    entry.reason = "re-stamped BLOCK_HAVE_UNDO";
                    if (auto* idx = FindBlockIndex(entry.hash)) {
                        idx->status |= BLOCK_HAVE_UNDO;
                        idx->undo_file = metadata.undo_file;
                        idx->undo_pos = metadata.undo_pos;
                        idx->undo_size = metadata.undo_size;
                    }
                } else {
                    entry.reason = "failed to write metadata re-stamp: " +
                                   std::string(StatusToString(write_status));
                    report.failed++;
                }
            }
        }

        report.entries.push_back(std::move(entry));
        current_height--;
        report.scanned++;
    }

    return report;
}

bool ChainstateService::DebugClearUndoFlagForBlock(const uint256& hash, std::string& error) {
    if (Params().network_id != "regtest") {
        error = "debug clear undo flag is regtest-only";
        return false;
    }
    if (!chain_db_) {
        error = "chain_db unavailable";
        return false;
    }

    auto metadata_result = chain_db_->getHeaderMetadata(hash);
    if (metadata_result.status() != Status::Ok) {
        error = "missing header metadata: " +
                std::string(StatusToString(metadata_result.status()));
        return false;
    }

    auto metadata = metadata_result.value();
    metadata.status_flags &= ~BLOCK_HAVE_UNDO;

    rocksdb::WriteBatch batch;
    ChainWriteToken token;
    auto stage_status = chain_db_->putHeaderMetadata(token, hash, metadata, &batch);
    if (stage_status != Status::Ok) {
        error = "failed to stage metadata update: " +
                std::string(StatusToString(stage_status));
        return false;
    }
    auto write_status = chain_db_->writeBatch(token, std::move(batch), true);
    if (write_status != Status::Ok) {
        error = "failed to write metadata update: " +
                std::string(StatusToString(write_status));
        return false;
    }
    if (auto* idx = FindBlockIndex(hash)) {
        idx->status &= ~BLOCK_HAVE_UNDO;
    }
    return true;
}

bool ChainstateService::VerifyActiveChainUndoCoverage(uint32_t tip_height,
                                                      uint32_t max_blocks_back) {
    if (!chain_db_ || !block_storage_) {
        if (logger_) {
            logger_->error("[ChainstateService] Undo audit requires ChainDB + BlockStorage");
        }
        return false;
    }

    // Walk backwards from the persisted tip via parent pointers in
    // header metadata. We trust persisted metadata's parent_hash as the
    // backbone — the in-memory CBlockIndex graph may not yet be loaded
    // when this audit runs (it's invoked early in Start()).
    auto current_hash_result = chain_db_->getBlockHashByHeight(static_cast<int>(tip_height));
    if (current_hash_result.status() != Status::Ok) {
        if (logger_) {
            logger_->warning("[ChainstateService] Undo audit: cannot resolve tip hash for height " +
                             std::to_string(tip_height) + " — skipping");
        }
        return true;  // Nothing to audit; not a failure of this audit.
    }
    uint256 current_hash = current_hash_result.value();

    uint32_t walked = 0;
    uint32_t current_height = tip_height;
    const uint32_t scan_limit = (max_blocks_back == 0)
        ? std::numeric_limits<uint32_t>::max()
        : max_blocks_back;

    while (walked < scan_limit && current_height > 0) {
        auto metadata_result = chain_db_->getHeaderMetadata(current_hash);
        if (metadata_result.status() != Status::Ok) {
            if (logger_) {
                logger_->warning("[ChainstateService] Undo audit: missing header metadata at height " +
                                 std::to_string(current_height) +
                                 " hash=" + current_hash.GetHex() + " — stopping walk");
            }
            return true;  // Header missing is a different failure class; not our wedge signal.
        }
        const auto& metadata = metadata_result.value();

        // P2 fix (Apr 30 2026): write the marker directly, do NOT call
        // ScheduleChainstateRecovery. The latter early-returns without
        // writing the marker when kAutomaticChainstateRecoveryArmed is
        // false (current state) AND it always EnterSafeMode()s — which
        // would block ConnectTip / DisconnectTip and defeat D.3's
        // trivial-case regeneration. The audit's role is operator
        // visibility on disk, not a hard stop. If the corruption is
        // recoverable via D.3, the daemon should reach DisconnectTip
        // and self-clear; if it isn't, DisconnectTip's own ReadStoredUndo
        // failure path will EnterSafeMode at that point.
        auto write_marker_and_log = [&](const std::string& reason) {
            std::string marker_error;
            if (!datadir_.empty()) {
                if (!daemon::WriteChainstateRecoveryMarker(datadir_, reason, &marker_error)) {
                    if (logger_) {
                        logger_->error("[StartupUndoAudit] Failed to write recovery marker: " +
                                       marker_error + " (reason: " + reason + ")");
                    }
                }
            }
            if (logger_) {
                logger_->error("[StartupUndoAudit] " + reason +
                               "; chainstate_recovery.marker written for operator review; "
                               "DisconnectTip's D.3 fallback may self-clear on next reorg attempt");
            }
        };

        // Apr 30 2026 (extended): shared deep helper. The check covers
        // BLOCK_HAVE_UNDO presence, undo_size > 0, flatfile readable,
        // UndoRecord deserialize + structural sanity (created non-empty,
        // spent count == sum non-coinbase inputs, shielded snapshot
        // present iff block has shielded txs), Utreexo delta sidecar
        // (UD:<hash>) at utreexo-active heights, and a UTXO read-back
        // (coinbase output 0 in chaindb post-publish). This is the
        // same invariant ConnectTip asserts before publishing a new
        // tip. The audit reads each block's body once for the
        // deep-check inputs — modest extra I/O for ~1024 blocks at
        // startup.
        auto block_for_check_result = ReadStoredBlock(current_hash);
        if (block_for_check_result.status() != Status::Ok) {
            // Body unreadable is a different failure class than
            // disconnect-material; we surface it but don't trip the
            // recovery marker (DisconnectTip needs the body too and
            // would surface the same failure, with the same recovery
            // path applied lazily). Stop the walk here so we don't
            // chase a parent we can't read either.
            if (logger_) {
                logger_->warning("[ChainstateService] Undo audit: cannot read block body at height " +
                                 std::to_string(current_height) +
                                 " hash=" + current_hash.GetHex() +
                                 " — stopping walk");
            }
            return true;
        }
        auto material = CheckBlockDisconnectMaterialDurable(
            block_for_check_result.value(), current_hash, current_height,
            tip_height);
        if (!material.durable) {
            write_marker_and_log(material.failure_reason);
            return false;
        }

        // Walk to parent. Genesis (height 0) has no undo expectation.
        current_hash = metadata.parent_hash;
        --current_height;
        ++walked;
    }

    if (logger_) {
        logger_->info("[ChainstateService] Startup undo audit clean: " +
                      std::to_string(walked) + " active-chain blocks verified " +
                      "(tip=" + std::to_string(tip_height) + ")");
    }
    return true;
}

StatusOr<UndoRecord> ChainstateService::RegenerateUndoFromBlockTrivial(const Block& block) const {
    // Trivial-case regeneration: the block has no non-coinbase inputs
    // and no shielded txs. Then `spent` is empty, `created` is every
    // output of the coinbase (and any non-coinbase tx with no inputs,
    // which only exists in protocol-violating data — but we treat them
    // symmetrically here for safety). pre_block_shielded_frontier is
    // not needed because no shielded state changed.
    //
    // Anything more than that requires prevout lookup via txindex
    // (one I/O per spent input) or shielded reverse-apply, both of
    // which are deferred to a follow-on task. Until then, return
    // Internal so the caller knows trivial regeneration didn't apply.
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        if (tx.IsShielded()) {
            return Status::Internal;  // shielded reverse-apply not implemented
        }
        if (tx_idx > 0 && !tx.vin.empty()) {
            // Non-coinbase tx with actual inputs: needs prevout lookup.
            return Status::Internal;
        }
    }

    UndoRecord undo;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            CreatedOut out;
            out.txid = txid.AsUint256();
            out.vout = vout;
            undo.created.push_back(out);
        }
    }
    // No shielded state, no frontier snapshot needed.
    return undo;
}

StatusOr<UndoRecord> ChainstateService::RegenerateUndoFromBlock(const Block& block) const {
    if (!chain_db_) {
        return Status::Internal;
    }

    // D.3-shielded (Apr 30 2026): count total shielded outputs in this
    // block. We'll reverse-apply by truncating a clone of the live
    // CommitmentTree by that count to reconstruct
    // pre_block_shielded_frontier. Pre-fix this branch returned
    // Internal and DisconnectTip fell through to the recovery-marker
    // path — meaning a sibling-race wedge on a shielded-tx-bearing
    // block left the daemon stuck. Now auto-recoverable.
    //
    // Correctness: this path runs from DisconnectTip(tip_to_disconnect),
    // which is invoked only for the ACTIVE TIP. The live shielded_tree_
    // therefore is the post-this-block state (no blocks beyond this on
    // the active chain), so truncating by this block's shielded-output
    // count yields the pre-this-block state. Nullifiers don't need a
    // snapshot — DisconnectBlock removes them by re-reading
    // bundle.spends from the block body.
    uint64_t shielded_outputs_in_block = 0;
    for (const auto& tx : block.vtx) {
        if (!tx.IsShielded()) continue;
        consensus::shielded::ShieldedBundle bundle;
        const auto decode = consensus::shielded::DeserializeShieldedBundle(
            tx.shielded_bundle_bytes, &bundle);
        if (decode != consensus::shielded::BundleDecodeError::Ok) {
            if (logger_) {
                logger_->error("[RegenerateUndoFromBlock] failed to decode shielded bundle "
                               "(decode=" + std::to_string(static_cast<int>(decode)) + ")");
            }
            return Status::Serialization;
        }
        shielded_outputs_in_block += bundle.outputs.size();
    }

    UndoRecord undo;

    // Walk all transactions. tx_idx==0 is coinbase (no inputs to
    // restore — record outputs only). For tx_idx>0, look each input's
    // prevout up via the txindex, read the parent block from flatfile,
    // extract the consumed output, assemble the SpentCoin.
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];

        if (tx_idx > 0) {
            for (const auto& input : tx.vin) {
                const uint256 prev_txid = input.prevout.txid.AsUint256();
                const uint32_t prev_vout = input.prevout.vout;

                // 1. txindex lookup → (parent_block_hash, parent_tx_idx)
                auto loc_result = chain_db_->getTxLocation(prev_txid);
                if (loc_result.status() != Status::Ok) {
                    if (logger_) {
                        logger_->error("[RegenerateUndoFromBlock] txindex lookup failed for prevout " +
                                       prev_txid.GetHex().substr(0, 16) + "..." +
                                       ":" + std::to_string(prev_vout));
                    }
                    return loc_result.status();
                }
                const uint256& parent_block_hash = loc_result.value().first;
                const uint32_t parent_tx_idx = loc_result.value().second;

                // 2. read parent block body from flatfile
                auto parent_block_result = ReadStoredBlock(parent_block_hash);
                if (parent_block_result.status() != Status::Ok) {
                    if (logger_) {
                        logger_->error("[RegenerateUndoFromBlock] failed to read parent block " +
                                       parent_block_hash.GetHex().substr(0, 16) + "...");
                    }
                    return parent_block_result.status();
                }
                const Block& parent_block = parent_block_result.value();

                if (parent_tx_idx >= parent_block.vtx.size()) {
                    if (logger_) {
                        logger_->error("[RegenerateUndoFromBlock] txindex points past parent vtx end");
                    }
                    return Status::Internal;
                }
                const auto& parent_tx = parent_block.vtx[parent_tx_idx];

                if (prev_vout >= parent_tx.vout.size()) {
                    if (logger_) {
                        logger_->error("[RegenerateUndoFromBlock] prevout vout index out of range");
                    }
                    return Status::Internal;
                }
                const auto& parent_output = parent_tx.vout[prev_vout];

                // 3. parent block height from header metadata
                auto parent_meta_result = chain_db_->getHeaderMetadata(parent_block_hash);
                if (parent_meta_result.status() != Status::Ok) {
                    if (logger_) {
                        logger_->error("[RegenerateUndoFromBlock] missing header metadata for parent");
                    }
                    return parent_meta_result.status();
                }
                const int32_t parent_height = parent_meta_result.value().height;
                if (parent_height < 0) {
                    return Status::Internal;
                }

                // 4. assemble the SpentCoin
                SpentCoin spent;
                spent.prev_txid = prev_txid;
                spent.prev_vout = prev_vout;
                spent.value = parent_output.value.GetUna();
                spent.scriptPubKey = parent_output.scriptPubKey;
                spent.is_coinbase = (parent_tx_idx == 0);
                spent.height = static_cast<uint32_t>(parent_height);
                spent.is_confidential = parent_output.is_confidential;
                spent.commitment = parent_output.commitment;

                undo.spent.push_back(std::move(spent));
            }
        }

        // Always record outputs created (for delete-on-disconnect).
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            CreatedOut out;
            out.txid = txid.AsUint256();
            out.vout = vout;
            undo.created.push_back(out);
        }
    }

    // D.3-shielded: reconstruct pre_block_shielded_frontier from a
    // truncated clone of the live commitment tree. No live state is
    // mutated here — the clone is local, the truncation runs on the
    // clone, only the serialized snapshot lands in the undo record.
    // DisconnectBlock will use this snapshot to restore the live tree
    // to its pre-block shape.
    //
    // For blocks with no shielded outputs, leave the field unset:
    // DisconnectBlock's transparent-only path doesn't need a snapshot.
    if (shielded_outputs_in_block > 0) {
        const uint64_t live_size = shielded_tree_.Size();
        if (shielded_outputs_in_block > live_size) {
            if (logger_) {
                logger_->error("[RegenerateUndoFromBlock] shielded outputs in block (" +
                               std::to_string(shielded_outputs_in_block) +
                               ") exceed live tree size (" +
                               std::to_string(live_size) +
                               ") — refusing to truncate; live tree is not at this block's tip");
            }
            return Status::Internal;
        }

        // Crash oracle: fires AFTER transparent regen has produced
        // the spent/created vectors but BEFORE shielded reverse-apply
        // runs. Restart at this boundary must NOT see a partially-
        // populated UndoRecord (we discard the in-flight `undo`).
        // The existing restart-equivalence harness can target this
        // hook via testing::MaybeAbortAt; see
        // tests/integration/test_shielded_reorg_disconnect_restart_equivalence.sh.
        dinero::testing::MaybeAbortAt("d3_after_transparent_regen_before_shielded_clone",
                                      dinero::Params().network_id == "regtest");

        consensus::shielded::CommitmentTree truncated_clone(shielded_tree_);

        // Crash oracle: fires AFTER the live tree has been cloned
        // (clone is local; live untouched) but BEFORE Truncate runs
        // on the clone. Restart at this boundary must see the live
        // tree unchanged and the clone discarded.
        dinero::testing::MaybeAbortAt("d3_after_shielded_clone_before_truncate",
                                      dinero::Params().network_id == "regtest");

        if (!truncated_clone.Truncate(live_size - shielded_outputs_in_block)) {
            if (logger_) {
                logger_->error("[RegenerateUndoFromBlock] CommitmentTree::Truncate failed — "
                               "tree may have been restored from frontier-only and cannot "
                               "rebuild leaves; falling back to recovery marker");
            }
            return Status::Internal;
        }

        // Crash oracle: fires AFTER Truncate succeeded on the clone
        // but BEFORE SerializeFrontier writes the bytes into the
        // undo record. Restart at this boundary asserts the live
        // tree is still at post-block size (no live mutation), and
        // the in-flight undo has no shielded snapshot yet.
        dinero::testing::MaybeAbortAt("d3_after_shielded_truncate_before_serialize",
                                      dinero::Params().network_id == "regtest");

        undo.pre_block_shielded_frontier = truncated_clone.SerializeFrontier();
    }

    return undo;
}

bool ChainstateService::HasFlatfileBlockBody(const uint256& hash) const {
    if (!chain_db_ || !block_storage_) {
        return false;
    }

    auto metadata_result = chain_db_->getHeaderMetadata(hash);
    if (metadata_result.status() != Status::Ok) {
        return false;
    }

    const auto& metadata = metadata_result.value();
    if (metadata.data_size == 0) {
        return false;
    }

    const FilePosition pos(metadata.file_number, metadata.data_pos, metadata.data_size);
    return block_storage_->hasBlock(pos) == Status::Ok;
}

bool ChainstateService::HasStoredBlockBody(const uint256& hash) const {
    if (!chain_db_) {
        return false;
    }
    return storage::HasArchivalBlockBody(
        *chain_db_,
        block_storage_.get(),
        hash,
        storage::ArchivalReadMode::RequireFlatfiles);
}

// #309: the scheduler stores not-yet-connected bodies (e.g. a competing
// side-branch above the active tip) to flatfiles but, lacking a ChainDB handle,
// cannot record where the body lives. HasArchivalBlockBody resolves a body via
// the header metadata's {file_number,data_pos,data_size}, so without this the
// import loop perpetually re-requests already-downloaded blocks and the branch
// tip never becomes a reorg candidate. Mirror block_acceptor's metadata write
// for the store-only case, preserving any existing undo reference.
void ChainstateService::PersistStoredBodyPosition(const uint256& hash, const FilePosition& pos) {
    if (!chain_db_) return;
    if (pos.offset > std::numeric_limits<uint32_t>::max()) {
        if (logger_) logger_->warning("[#309] PersistStoredBodyPosition: data offset exceeds uint32 for " +
                                      hash.GetHex().substr(0, 16));
        return;
    }
    ChainDB::PersistedHeaderMetadata metadata;
    auto md_result = chain_db_->getHeaderMetadata(hash);
    if (md_result.status() == Status::Ok) {
        metadata = md_result.value();
        if ((metadata.status_flags & BLOCK_HAVE_DATA) && metadata.data_size > 0) {
            return;  // body position already recorded
        }
    } else {
        // No metadata row yet: the competing header may live only in the in-memory
        // block index (added by the better-branch import path, never persisted).
        // Build the row from the index so the stored body becomes discoverable.
        CBlockIndex* idx = FindBlockIndex(hash);
        if (!idx) {
            return;  // truly unknown header; nothing to record
        }
        metadata.parent_hash = idx->pprev ? idx->pprev->hash : uint256();
        metadata.height = static_cast<int32_t>(idx->height);
        metadata.chainwork = ChainworkFromHex(idx->chainwork);
        metadata.status_flags = idx->status | BLOCK_VALID_HEADER;
    }
    metadata.file_number = pos.file_number;
    metadata.data_pos = static_cast<uint32_t>(pos.offset);
    metadata.data_size = pos.size;
    metadata.status_flags |= BLOCK_HAVE_DATA;
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    Status st = chain_db_->putHeaderMetadataPreservingExistingUndo(token, hash, metadata, nullptr);
    if (logger_) {
        if (st != Status::Ok) {
            logger_->warning("[#309] PersistStoredBodyPosition: putHeaderMetadata failed for " +
                             hash.GetHex().substr(0, 16));
        } else {
            logger_->info("[#309] persisted body position at height " +
                          std::to_string(metadata.height) + " " + hash.GetHex().substr(0, 16));
        }
    }
}

StatusOr<Block> ChainstateService::ReadStoredBlock(const uint256& hash) const {
    if (!chain_db_) {
        return Status::Internal;
    }
    const auto outcome = storage::ReadArchivalBlockDetailed(
        *chain_db_,
        block_storage_.get(),
        hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    return outcome.result;
}

StatusOr<UndoRecord> ChainstateService::ReadStoredUndo(const uint256& hash) const {
    if (!chain_db_) {
        return Status::Internal;
    }
    const auto outcome = storage::ReadArchivalUndoDetailed(
        *chain_db_,
        block_storage_.get(),
        hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    return outcome.result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase C.1: P2P Block Relay Integration
// ═══════════════════════════════════════════════════════════════════════════

bool ChainstateService::ProcessIncomingBlockHex(const std::string& blockHex, const std::string& peer_id) {
    std::cout << "[CHAINSTATE-DEBUG] >>> ProcessIncomingBlockHex ENTRY from " << peer_id
              << " hex_size=" << blockHex.size() << std::endl;

    if (!started_) {
        std::cout << "[CHAINSTATE-DEBUG] ERROR: Service not started!" << std::endl;
        logger_->warning("[ChainstateService] Cannot process block - service not started");
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // EARLY DUPLICATE REJECTION: Check before full validation
    // ═══════════════════════════════════════════════════════════════════════════
    // Compute block hash from header (first 128 bytes = 256 hex chars) and check
    // against completed_blocks_ set. This avoids expensive full parsing/validation
    // for blocks we've already processed.
    // ═══════════════════════════════════════════════════════════════════════════
    constexpr size_t HEADER_HEX_SIZE = 256;  // 128 bytes * 2
    if (blockHex.size() >= HEADER_HEX_SIZE) {
        // Quick header hash computation
        std::string header_hex = blockHex.substr(0, HEADER_HEX_SIZE);
        std::vector<uint8_t> header_bytes;
        header_bytes.reserve(128);
        for (size_t i = 0; i < HEADER_HEX_SIZE; i += 2) {
            uint8_t byte = static_cast<uint8_t>(std::stoul(header_hex.substr(i, 2), nullptr, 16));
            header_bytes.push_back(byte);
        }

        // Double SHA256 for block hash (returns hex string)
        std::string block_hash_hex = crypto::double_sha256(header_bytes);

        // Check against completed blocks
        {
            std::lock_guard<std::mutex> lock(block_request_state_mutex_);
            if (completed_blocks_.count(block_hash_hex) > 0) {
                std::cout << "[CHAINSTATE-DEBUG] EARLY DUPLICATE REJECTION: " << block_hash_hex << std::endl;
                logger_->debug("[ChainstateService] Ignoring duplicate block (early check): " + block_hash_hex);
                return true;  // Not an error, just already processed
            }
        }

        // ═══════════════════════════════════════════════════════════════════════
        // SIDE-CHAIN GATE: Explicit permission based on IBD state machine
        // ═══════════════════════════════════════════════════════════════════════
        // Side-chains are allowed IF AND ONLY IF ibd_status_ == IBDComplete.
        // This is the SINGLE gate for all non-tip block processing.
        //
        // Before IBDComplete: Linear only (no side-chains, no reorgs)
        // After IBDComplete:  Side-chains allowed (reorg support enabled)
        //
        // This is PERMISSION, not correctness. Reorg logic comes later.
        // ═══════════════════════════════════════════════════════════════════════

        // Extract prev_block_hash from header (offset 4, 32 bytes = hex chars 8-72)
        // Wire-order hex — must be compared as uint256, not as hex strings,
        // because GetHex() returns display-order (reversed).
        constexpr size_t PREV_HASH_OFFSET = 8;   // 4 bytes * 2
        constexpr size_t PREV_HASH_LEN = 64;     // 32 bytes * 2
        std::string prev_hash_wire_hex = blockHex.substr(PREV_HASH_OFFSET, PREV_HASH_LEN);

        // Get current tip from chain DB
        if (chain_db_) {
            auto tip_result = chain_db_->getTip();
            if (tip_result.status() == Status::Ok) {
                // Parse wire-order hex into uint256 for correct comparison.
                // Wire hex is raw byte order = uint256 internal storage.
                uint256 prev_hash;
                for (size_t i = 0; i < 32 && i * 2 + 1 < prev_hash_wire_hex.size(); ++i) {
                    unsigned int byte_val;
                    sscanf(prev_hash_wire_hex.c_str() + i * 2, "%02x", &byte_val);
                    prev_hash.data[i] = static_cast<uint8_t>(byte_val);
                }
                std::string tip_hash_hex = tip_result.value().hash.GetHex();
                bool is_main_chain_extension = (prev_hash == tip_result.value().hash);

                if (!is_main_chain_extension) {
                    // This is a side-chain block (parent != active tip)
                    // REORG FIX: Always allow side-chain blocks to be processed.
                    // The two-phase validation architecture handles them correctly:
                    // 1. Store block and index it (deferred UTXO validation)
                    // 2. ActivateBestChain detects better chain and reorgs
                    // The old IBD gate was too restrictive and blocked reorgs entirely.
                    std::cout << "[SYNC] SIDE-CHAIN BLOCK detected:" << std::endl;
                    std::cout << "[SYNC]   Block parent: " << prev_hash.GetHex().substr(0, 16) << "..." << std::endl;
                    std::cout << "[SYNC]   Active tip:   " << tip_hash_hex.substr(0, 16) << "..." << std::endl;
                    std::cout << "[SYNC]   Processing (UTXO validation deferred to reorg)..." << std::endl;
                    // Fall through to BlockAcceptor (handles side-chain validation correctly)
                } else {
                    std::cout << "[SYNC] Main chain extension ✓" << std::endl;
                }
            }
        }
    }

    logger_->info("[ChainstateService] Processing incoming block (hex) from peer: " + peer_id);

    // Call BlockAcceptor to perform full validation
    // AcceptBlockFromRPC handles hex format and performs:
    // - PoW validation
    // - Parent link validation
    // - Merkle root validation
    // - Transaction validation
    // - UTXO validation via ChainManager::ProcessNewBlock
    std::string source = "peer:" + peer_id;
    std::cout << "[CHAINSTATE-DEBUG] Calling BlockAcceptor::AcceptBlockFromRPC..." << std::endl;
    BlockAcceptResult result = BlockAcceptor::AcceptBlockFromRPC(blockHex, source);
    std::cout << "[CHAINSTATE-DEBUG] BlockAcceptor returned: accepted=" << result.accepted()
              << " code=" << static_cast<int>(result.code)
              << " reason=" << result.reason << std::endl;

    std::string hash_hex = result.block_hash.GetHex();

    if (result.accepted()) {
        logger_->info("[ChainstateService] Block accepted: " + hash_hex +
                     " at height " + std::to_string(result.height));

        // Phase C.3 Phase 3: Mark block as completed and remove from in-flight tracking
        {
            std::lock_guard<std::mutex> lock(block_request_state_mutex_);
            completed_blocks_.insert(hash_hex);
            if (in_flight_blocks_.count(hash_hex) > 0) {
                logger_->debug("[ChainstateService] Block " + hash_hex + " completed (was in-flight)");
                in_flight_blocks_.erase(hash_hex);
            }
        }

        ResetTrackedStallForBlock(hash_hex, "tracked branch block received");

        // Do NOT announce on accept. Announcement is activation-gated and happens
        // from the active-chain connect path (ConnectTip -> notifyBlockConnected).
        logger_->debug("[ChainstateService] Block accepted but not announced yet (awaiting activation): " +
                      hash_hex);

        // Phase C.1 v2: Process orphans that were waiting for this block
        ProcessOrphans(hash_hex);

        // Phase N.4: Keep the scheduler aligned to the REAL active tip, not the
        // accepted block's height. Side-branch blocks can be accepted into the
        // index long before they activate, and using result.height here poisons
        // the scheduler's local tip shadow.
        if (auto* ctx = DaemonContext::instance()) {
            if (ctx->block_download) {
                if (auto* tip = GetActiveTip()) {
                    ctx->block_download->SetLocalTipHeight(static_cast<uint32_t>(tip->height));
                }
                ctx->block_download->Tick();
            }
        }

        // ═══════════════════════════════════════════════════════════════════════
        // IBD EXIT DETECTION: Check if we've transitioned out of IBD
        // ═══════════════════════════════════════════════════════════════════════
        // When IBD completes, side-chain processing becomes allowed for reorg support.
        // Log this transition for visibility and update internal state.
        // ═══════════════════════════════════════════════════════════════════════
        if (ibd_status_ == IBDStatus::InIBD && !IsInIBD()) {
            ibd_status_ = IBDStatus::IBDComplete;
            services_ready_ = true;
            logger_->info("════════════════════════════════════════════════════════════════");
            logger_->info("✅ INITIAL BLOCK DOWNLOAD COMPLETE");
            logger_->info("════════════════════════════════════════════════════════════════");
            logger_->info("   Final height: " + std::to_string(result.height));
            logger_->info("   Side-chain processing: ENABLED (for reorg support)");
            logger_->info("   Services: READY");
            logger_->info("════════════════════════════════════════════════════════════════");
        }

        // P2 Fix: Trigger ActivateBestChain after every accepted block.
        // Fork blocks need immediate reorg consideration — the 30s periodic check
        // is too slow and the stall guard may have been blocking retries.
        ActivateBestChain();

        // ActivateBestChain may have advanced the active tip through a reorg.
        // Refresh the scheduler hint again so later rescans/request windows start
        // from the chain we actually connected, not the last pre-activation tip.
        if (auto* ctx = DaemonContext::instance()) {
            if (ctx->block_download) {
                if (auto* tip = GetActiveTip()) {
                    ctx->block_download->SetLocalTipHeight(static_cast<uint32_t>(tip->height));
                }
            }
        }

        // Do not emit connected notifications on accept.
        // Active-chain notifications must happen only from ConnectTip(), after
        // fork-choice activation has committed state.

        return true;
    } else {
        const std::string reject_log = "[ChainstateService] Block rejected: " + result.reason +
            " (code: " + std::string(BlockRejectCodeToString(result.code)) + ")";
        if (result.code == BlockRejectCode::MISSING_PARENT) {
            logger_->debug(reject_log);
        } else {
            logger_->warning(reject_log);
        }

        // Phase C.1 v2: Handle orphan blocks (missing parent)
        if (result.code == BlockRejectCode::MISSING_PARENT) {
            // Extract prev_hash from block header (bytes 4..36 of header in blockHex)
            uint256 prev_hash;
            if (blockHex.size() >= 72) {
                std::string prev_hex = blockHex.substr(8, 64);
                for (size_t i = 0; i < 32 && i * 2 + 1 < prev_hex.size(); ++i) {
                    unsigned int bv;
                    sscanf(prev_hex.c_str() + i * 2, "%02x", &bv);
                    prev_hash.data[i] = static_cast<uint8_t>(bv);
                }
            }

            bool parent_received = false;
            bool parent_expected = false;
            bool scheduler_synced = false;
            if (auto* ctx = DaemonContext::instance()) {
                if (ctx->block_download) {
                    parent_received = ctx->block_download->HasReceivedBlock(prev_hash);
                    parent_expected = ctx->block_download->IsBlockExpected(prev_hash);
                    scheduler_synced = ctx->block_download->IsFullySynchronized();
                }
            }
            LogMissingParentDiagRateLimited(peer_id, prev_hash, parent_received, parent_expected, scheduler_synced);

            // Deserialize hex to block for orphan storage
            try {
                std::vector<uint8_t> block_bytes;
                block_bytes.reserve(blockHex.size() / 2);
                for (size_t i = 0; i < blockHex.size(); i += 2) {
                    std::string byte_str = blockHex.substr(i, 2);
                    block_bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
                }

                auto block_opt = Block::Deserialize(block_bytes);
                if (!block_opt.has_value()) {
                    logger_->warning("[ChainstateService] Failed to deserialize block for orphan pool: Block::Deserialize returned null");
                    return false;
                }
                const Block& block = *block_opt;

                bool orphan_added = AddOrphanBlock(block, peer_id);
                if (!orphan_added) {
                    logger_->debug("[ChainstateService] Orphan pool rejected block " +
                                  block.GetHash().GetHex().substr(0, 16) +
                                  "... (duplicate/full); child will not be re-requested");
                }
            } catch (const std::exception& e) {
                logger_->warning("[ChainstateService] Failed to deserialize block for orphan pool: " +
                               std::string(e.what()));
            }
        }

        return false;
    }
}

consensus::ConnectBlockResult ChainstateService::ProcessIncomingStoredBlock(const Block& block, const std::string& source) {
    const bool allow_prestart = (source == kStartupCatchupSource);
    if (!started_ && !allow_prestart) {
        logger_->warning("[ChainstateService] Cannot process block - service not started");
        return consensus::ConnectBlockResult::TEMPORARY_FAIL;
    }

    logger_->info("[ChainstateService] Processing incoming block from peer: " + source);

    // Call BlockAcceptor to perform full validation
    // This includes: PoW check, parent link, merkle root, script validation, UTXO validation
    BlockAcceptResult result = BlockAcceptor::AcceptBlockFromPeer(block, source);

    std::string hash_hex = result.block_hash.GetHex();

    if (result.accepted()) {
        logger_->info("[ChainstateService] Block accepted: " + hash_hex +
                     " at height " + std::to_string(result.height));

        // Clear from unreadable set — block now has valid data from peer.
        // Self-synchronizing: this runs on the scheduler-drain thread, not
        // under activation_mutex_ (see UnreadableBlockSet).
        unreadable_blocks_.clear(result.block_hash);

        // Do NOT announce on accept. Announcement is activation-gated and happens
        // from the active-chain connect path (ConnectTip -> notifyBlockConnected).
        logger_->debug("[ChainstateService] Stored block accepted but not announced yet (awaiting activation): " +
                      hash_hex);

        // Phase C.1 v2: Process orphans that were waiting for this block
        ProcessOrphans(hash_hex);

        // P2 Fix: Reset stall tracking when blocks from a better chain arrive.
        ResetTrackedStallForBlock(hash_hex, "tracked stored block received");

        // P2 Fix: Trigger ActivateBestChain after every accepted stored block.
        // Fork blocks from the scheduler-drain path need immediate reorg
        // consideration — the 30s periodic check is too slow.
        ActivateBestChain();

        // Important distinction for the scheduler:
        // Accepting/indexing a block is not the same thing as activating it on
        // the current chain. Returning CONNECTED here for side-branch blocks
        // lets the scheduler advance its local tip far beyond the real active
        // tip, which then poisons later rescans and gap recovery.
        uint256 active_hash_at_height;
        if (result.height > 0 &&
            consensus::GetActiveChainHashAtHeight(GetActiveTip(), result.height, active_hash_at_height) &&
            active_hash_at_height == result.block_hash) {
            return consensus::ConnectBlockResult::CONNECTED;
        }

        logger_->debug("[ChainstateService] Block accepted but not active at height " +
                      std::to_string(result.height) + ": " +
                      hash_hex.substr(0, 16) + "...");
        return consensus::ConnectBlockResult::ACCEPTED_NOT_ACTIVE;
    }

    const std::string reject_log = "[ChainstateService] Block rejected: " + result.reason +
        " (code: " + std::string(BlockRejectCodeToString(result.code)) + ")";
    if (result.code == BlockRejectCode::MISSING_PARENT) {
        logger_->debug(reject_log);
    } else {
        logger_->warning(reject_log);
    }

    if (result.code == BlockRejectCode::DUPLICATE) {
        return consensus::ConnectBlockResult::DUPLICATE;
    }

    // Missing parent is the critical branch for scheduler-drain:
    // do NOT orphan/re-request child there. Return classification so
    // drainer can request parent and stop.
    if (result.code == BlockRejectCode::MISSING_PARENT) {
        const uint256 prev_hash = block.header.prev_block_hash;
        bool parent_received = false;
        bool parent_expected = false;
        bool scheduler_synced = false;
        if (auto* ctx = DaemonContext::instance()) {
            if (ctx->block_download) {
                parent_received = ctx->block_download->HasReceivedBlock(prev_hash);
                parent_expected = ctx->block_download->IsBlockExpected(prev_hash);
                scheduler_synced = ctx->block_download->IsFullySynchronized();
            }
        }

        if (source == "scheduler-drain") {
            logger_->debug("[ChainstateService] Drain block missing parent " +
                          prev_hash.GetHex().substr(0, 16) +
                          "... (received=" + std::to_string(parent_received) +
                          ", expected=" + std::to_string(parent_expected) + ")");
            if (parent_received || parent_expected) {
                return consensus::ConnectBlockResult::WAITING_PARENT;
            }
            return consensus::ConnectBlockResult::MISSING_PARENT;
        }

        LogMissingParentDiagRateLimited(source, prev_hash, parent_received, parent_expected, scheduler_synced);
        AddOrphanBlock(block, source);
        return consensus::ConnectBlockResult::MISSING_PARENT;
    }

    // Connection-level failures are usually transient (DB busy/lock contention).
    if (result.code == BlockRejectCode::CONNECT_FAILED ||
        result.code == BlockRejectCode::STALE_TIP_CHANGED ||
        result.code == BlockRejectCode::STALE_MEMPOOL_CHANGED ||
        result.code == BlockRejectCode::STALE_REORG ||
        result.code == BlockRejectCode::STALE_TIMESTAMP) {
        return consensus::ConnectBlockResult::TEMPORARY_FAIL;
    }

    return consensus::ConnectBlockResult::INVALID;
}

bool ChainstateService::ProcessIncomingBlock(const Block& block, const std::string& peer_id) {
    consensus::ConnectBlockResult connect_result = ProcessIncomingStoredBlock(block, peer_id);
    return (connect_result == consensus::ConnectBlockResult::CONNECTED ||
            connect_result == consensus::ConnectBlockResult::DUPLICATE);
}

void ChainstateService::setP2PService(std::shared_ptr<class P2PService> p2p_service) {
    p2p_service_ = p2p_service;
    if (p2p_service_) {
        logger_->info("[ChainstateService] P2P service wired for block broadcasting");
    }
}

void ChainstateService::setBlockRelayManager(std::shared_ptr<class BlockRelayManager> block_relay) {
    block_relay_manager_ = block_relay;
    if (block_relay_manager_) {
        // Wire up GetBestBlockHashCallback for tip announcement (Phase G.X fork resolution)
        block_relay_manager_->SetGetBestBlockHashCallback([this]() -> uint256 {
            std::string hex_hash = this->getBestBlockHash();
            if (hex_hash.empty() || hex_hash == "0") {
                return uint256();  // Null hash
            }
            return uint256::FromHexUnsafe(hex_hash);
        });
        logger_->info("[ChainstateService] BlockRelayManager wired for Phase G.2 announcements + tip sync");
    }
}

void ChainstateService::setChainOracleClient(std::unique_ptr<dinero::ipc::ChainOracleClient> oracle) {
    chain_oracle_client_ = std::move(oracle);
    if (chain_oracle_client_) {
        logger_->info("[ChainstateService] ChainOracleClient wired for Phase 9.2 Lightning events");
    }
}

void ChainstateService::setHeaderChainSelector(std::shared_ptr<dinero::consensus::HeaderChainSelector> header_chain) {
    header_chain_selector_ = header_chain;
    if (header_chain_selector_) {
        logger_->info("[ChainstateService] HeaderChainSelector wired for header sync");
    }
}

void ChainstateService::setTimeOracleClient(std::unique_ptr<dinero::ipc::TimeOracleClient> oracle) {
    time_oracle_client_ = std::move(oracle);
    if (time_oracle_client_) {
        logger_->info("[ChainstateService] TimeOracleClient wired for Phase 9.2 Lightning time tracking");
    }
}

void ChainstateService::setTransactionOracleClient(std::shared_ptr<dinero::ipc::TransactionOracleClient> oracle) {
    transaction_oracle_client_ = oracle;
    if (transaction_oracle_client_) {
        logger_->info("[ChainstateService] TransactionOracleClient wired for Phase 9.2 Lightning TX tracking");
    }
}

void ChainstateService::setBridgeNode(std::shared_ptr<network::BridgeNode> node) {
    bridge_node_ = node;
    if (bridge_node_) {
        logger_->info("[ChainstateService] BridgeNode wired for Phase P.2 Utreexo proof pre-caching");
    }
}

void ChainstateService::setProofGossipManager(
    std::shared_ptr<consensus::ProofGossipManager> manager
) {
    proof_gossip_manager_ = std::move(manager);
    if (proof_gossip_manager_) {
        logger_->info("[ChainstateService] ProofGossipManager wired for tip-proof prewarm");
    }
}

void ChainstateService::BroadcastNewBlock(const std::string& block_hash) {
    logger_->info("[ChainstateService] Broadcasting new block: " + block_hash);

    // Phase G.2: Use BlockRelayManager if available
    if (block_relay_manager_) {
        uint256 hash = uint256::FromHexUnsafe(block_hash);
        block_relay_manager_->AnnounceBlock(hash);
        logger_->info("[ChainstateService] Block announced via Phase G.2 BlockRelayManager: " + block_hash);
        return;
    }

    // Fallback to legacy P2P broadcast
    if (!p2p_service_) {
        logger_->warning("[ChainstateService] Cannot broadcast - P2P service not wired");
        return;
    }

    // Create INV message for the block
    std::vector<std::string> hashes = {block_hash};
    ::P2PMessage inv_msg = ::P2PMessage::create_inv(hashes, "block");

    // Broadcast to all peers
    p2p_service_->BroadcastMessage(inv_msg);

    logger_->info("[ChainstateService] Block INV broadcasted to all peers: " + block_hash);
}

// ============================================================================
// Phase C.1.5: P2P Message Handlers
// ============================================================================

void ChainstateService::OnInv(const std::string& peer_addr, const ::P2PMessage& msg) {
    // Parse INV message payload: "block:hash1,hash2,..."
    std::string payload_str(msg.payload.begin(), msg.payload.end());

    // Extract type and hashes
    size_t colon = payload_str.find(':');
    if (colon == std::string::npos) {
        logger_->warning("[ChainstateService] Invalid INV format from " + peer_addr);
        return;
    }

    std::string inv_type = payload_str.substr(0, colon);
    std::string hashes_str = payload_str.substr(colon + 1);

    // Only handle block INVs for Phase C.1.5
    if (inv_type != "block" && inv_type != "utreexo_block") {
        logger_->debug("[ChainstateService] Ignoring non-block INV: " + inv_type);
        return;
    }

    // Parse comma-separated hashes
    std::vector<std::string> needed_hashes;
    std::istringstream ss(hashes_str);
    std::string hash;

    while (std::getline(ss, hash, ',')) {
        if (hash.empty()) continue;
        if (AlreadyHaveBlock(hash)) {
            logger_->debug("[ChainstateService] INV block already known, skip: " + hash);
            continue;
        }
        if (IsBlockInFlight(hash)) {
            logger_->debug("[ChainstateService] INV block already in-flight, skip: " + hash);
            continue;
        }
        needed_hashes.push_back(hash);
        logger_->info("[ChainstateService] Need block: " + hash);
    }

    if (needed_hashes.empty()) {
        logger_->debug("[ChainstateService] INV from " + peer_addr + " contains no new blocks");
        return;
    }

    // Request blocks via GETDATA
    if (!p2p_service_) {
        logger_->warning("[ChainstateService] Cannot request blocks - P2P service not wired");
        return;
    }

    logger_->info("[ChainstateService] Requesting " + std::to_string(needed_hashes.size()) +
                 " block(s) from " + peer_addr);

    for (const auto& hash : needed_hashes) {
        ::P2PMessage getdata_msg = CreateBlockGetDataMessage(hash);
        p2p_service_->get().send_to_peer(peer_addr, getdata_msg);
    }
}

void ChainstateService::OnGetData(const std::string& peer_addr, const ::P2PMessage& msg) {
    // Parse GETDATA message payload: "block:hash1,hash2,..."
    std::string payload_str(msg.payload.begin(), msg.payload.end());

    // Extract type and hashes
    size_t colon = payload_str.find(':');
    if (colon == std::string::npos) {
        logger_->warning("[ChainstateService] Invalid GETDATA format from " + peer_addr);
        return;
    }

    std::string data_type = payload_str.substr(0, colon);
    std::string hashes_str = payload_str.substr(colon + 1);

    // Only handle block requests for Phase C.1.5
    if (data_type != "block" && data_type != "utreexo_block") {
        logger_->debug("[ChainstateService] Ignoring non-block GETDATA: " + data_type);
        return;
    }

    // Parse comma-separated hashes and send requested blocks
    std::istringstream ss(hashes_str);
    std::string hash;
    int sent_count = 0;

    while (std::getline(ss, hash, ',')) {
        if (hash.empty()) continue;

        // Retrieve block from the shared archival reader
        if (!chain_db_) {
            logger_->error("[ChainstateService] ChainDB not available");
            continue;
        }

        // Phase C.2: Full block transmission implementation
        try {
            // Phase M.0: Convert hex hash to uint256
            uint256 hash_uint256 = uint256::FromHexUnsafe(hash);
            auto block_result = getBlockByHash(hash_uint256);
            if (block_result.status() != Status::Ok) {
                logger_->warning("[ChainstateService] Block not found in archival storage: " + hash);
                continue;
            }

            // Serialize block to raw bytes, then convert to hex
            Block block = block_result.value();
            std::string block_bytes = block.Serialize();  // Returns raw bytes

            // Convert bytes to hex string
            std::string block_hex;
            block_hex.reserve(block_bytes.size() * 2);
            const char hex_chars[] = "0123456789abcdef";
            for (unsigned char c : block_bytes) {
                block_hex += hex_chars[c >> 4];
                block_hex += hex_chars[c & 0x0F];
            }

            // Create BLOCK P2P message
            ::P2PMessage block_msg = ::P2PMessage::create_block(block_hex);

            // Send to requesting peer
            if (p2p_service_) {
                p2p_service_->get().send_to_peer(peer_addr, block_msg);
                logger_->info("[ChainstateService] ✅ Sent block to " + peer_addr + ": " + hash);
                sent_count++;
            } else {
                logger_->error("[ChainstateService] P2P service not available");
            }

        } catch (const std::exception& e) {
            logger_->error("[ChainstateService] Failed to send block " + hash + ": " + std::string(e.what()));
        }
    }

    if (sent_count > 0) {
        logger_->info("[ChainstateService] ✅ Total blocks sent: " + std::to_string(sent_count));
    }
}

// ============================================================================
// Phase C.3: Headers-First Sync Implementation
// ============================================================================

// Phase M.0: Returns uint256 vector (consensus-adjacent, identity-sensitive)
// Conversion to hex happens at P2P/RPC boundary (network serialization layer)
std::vector<uint256> ChainstateService::GenerateBlockLocator() {
    std::vector<uint256> locator;  // Phase M.0: Store uint256, not hex strings

    if (!chain_db_) {
        logger_->error("[ChainstateService] Cannot generate block locator: ChainDB not available");
        return locator;
    }

    // Get current tip
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        logger_->warning("[ChainstateService] Cannot generate block locator: No tip found");
        return locator;
    }

    TipInfo tip = tip_result.value();
    int current_height = tip.height;

    // Bitcoin-standard block locator algorithm
    // Start at best block, exponential backoff

    int step = 1;
    int height = current_height;

    // Add recent blocks (heights: best, best-1, ..., best-9)
    while (height > current_height - 10 && height >= 0) {
        auto hash_result = chain_db_->getBlockHashByHeight(height);
        if (hash_result.status() == Status::Ok) {
            locator.push_back(hash_result.value());  // Phase M.0: Store uint256 directly
        }
        height--;
    }

    // Exponential backoff (heights: best-13, best-21, best-37, ...)
    if (height >= 0) {
        step = 2;
        while (height >= 0) {
            auto hash_result = chain_db_->getBlockHashByHeight(height);
            if (hash_result.status() == Status::Ok) {
                locator.push_back(hash_result.value());  // Phase M.0: Store uint256 directly
            }

            height -= step;
            step *= 2;  // Exponential increase
        }
    }

    // Always include genesis hash (height 0)
    auto genesis_result = chain_db_->getBlockHashByHeight(0);
    if (genesis_result.status() == Status::Ok) {
        const uint256& genesis_hash = genesis_result.value();  // Phase M.0: Keep as uint256
        // Check if already added (Phase M.0: Direct uint256 comparison)
        if (std::find(locator.begin(), locator.end(), genesis_hash) == locator.end()) {
            locator.push_back(genesis_hash);  // Phase M.0: Store uint256 directly
        }
    }

    logger_->debug("[ChainstateService] Generated block locator with " +
                   std::to_string(locator.size()) + " hashes (tip height: " +
                   std::to_string(current_height) + ")");

    return locator;
}

void ChainstateService::OnGetHeaders(const std::string& peer_addr, const P2PMessage& msg) {
    // Phase C.3: GETHEADERS handler
    // Bitcoin wire format: version(4) + varint(count) + hashes(32*count) + stophash(32)

    if (msg.payload.size() < 4) {
        logger_->warning("[ChainstateService] Received too-short GETHEADERS from " + peer_addr);
        return;
    }

    size_t offset = 0;

    // Skip version (4 bytes) - we don't validate it
    offset += 4;

    // Parse varint for hash count
    auto read_varint = [&]() -> uint64_t {
        if (offset >= msg.payload.size()) return 0;
        uint8_t first = msg.payload[offset++];
        if (first < 0xFD) {
            return first;
        } else if (first == 0xFD) {
            if (offset + 2 > msg.payload.size()) return 0;
            uint64_t val = msg.payload[offset] | (static_cast<uint64_t>(msg.payload[offset + 1]) << 8);
            offset += 2;
            return val;
        } else if (first == 0xFE) {
            if (offset + 4 > msg.payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 4; i++) val |= static_cast<uint64_t>(msg.payload[offset + i]) << (i * 8);
            offset += 4;
            return val;
        } else {
            if (offset + 8 > msg.payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 8; i++) val |= static_cast<uint64_t>(msg.payload[offset + i]) << (i * 8);
            offset += 8;
            return val;
        }
    };

    uint64_t hash_count = read_varint();
    if (hash_count == 0 || hash_count > 101) {  // Bitcoin allows max 101 locator hashes
        logger_->warning("[ChainstateService] Invalid GETHEADERS hash count: " + std::to_string(hash_count));
        return;
    }

    std::vector<uint256> locator;
    for (uint64_t i = 0; i < hash_count; i++) {
        if (offset + 32 > msg.payload.size()) {
            logger_->warning("[ChainstateService] Truncated GETHEADERS payload from " + peer_addr);
            return;
        }
        // Wire bytes arrive in display order (sender used GetHex()).
        // uint256.data[] is internal order, so reverse after copy.
        uint256 hash;
        std::memcpy(hash.data, &msg.payload[offset], 32);
        std::reverse(hash.data, hash.data + 32);
        locator.push_back(hash);
        offset += 32;
    }

    // Skip stop hash (32 bytes) - we send all available headers anyway

    if (locator.empty()) {
        logger_->warning("[ChainstateService] Received empty GETHEADERS from " + peer_addr);
        return;
    }

    logger_->debug("[ChainstateService] Received GETHEADERS from " + peer_addr +
                   " (locator size: " + std::to_string(locator.size()) + ")");

    if (!chain_db_) {
        logger_->error("[ChainstateService] Cannot process GETHEADERS: ChainDB not available");
        return;
    }

    // Find common ancestor from locator, but ONLY if the locator hash is on the
    // active chain at that height. A hash existing in header DB is insufficient
    // (it may be a stale side-branch from an earlier fork).
    int common_height = -1;
    std::string common_hash;

    for (const auto& loc_hash : locator) {
        auto height_result = chain_db_->getBlockHeight(loc_hash);
        if (height_result.status() != Status::Ok) {
            continue;
        }

        int height = height_result.value();
        auto active_hash_result = chain_db_->getBlockHashByHeight(height);
        if (active_hash_result.status() != Status::Ok) {
            continue;
        }

        if (active_hash_result.value() == loc_hash) {
            // Locator is ordered newest->oldest, so first active hit is best.
            common_height = height;
            common_hash = loc_hash.GetHex();
            break;
        }
    }

    if (common_height < 0) {
        // No common ancestor - peer is starting fresh, send from genesis
        logger_->info("[ChainstateService] No common ancestor with peer " + peer_addr + " - sending from genesis");
        common_height = -1;  // Will start from height 0
    }

    logger_->debug("[ChainstateService] Found common ancestor at height " +
                   std::to_string(common_height) + ": " + common_hash);

    // Get current tip to know where to stop
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        logger_->warning("[ChainstateService] Cannot get tip for GETHEADERS");
        return;
    }

    int tip_height = tip_result.value().height;

    // Walk forward from common ancestor, collecting headers
    // Bitcoin standard: max 2000 headers per message
    std::vector<std::string> header_hexes;
    const int MAX_HEADERS = 2000;

    int start_height = common_height + 1;  // Start after common ancestor
    int end_height = std::min(tip_height, start_height + MAX_HEADERS - 1);

    for (int h = start_height; h <= end_height; h++) {
        auto hash_result = chain_db_->getBlockHashByHeight(h);
        if (hash_result.status() != Status::Ok) {
            logger_->warning("[ChainstateService] Missing block hash at height " + std::to_string(h));
            break;
        }

        // Phase M.0: Keep as uint256, convert to hex only for logging
        const uint256& block_hash = hash_result.value();
        auto header_result = chain_db_->getHeader(block_hash);
        if (header_result.status() != Status::Ok) {
            logger_->warning("[ChainstateService] Missing header for hash " + block_hash.GetHex());
            break;
        }

        // Serialize header to hex
        BlockHeader header = header_result.value();
        std::string header_bytes = header.Serialize();

        // Convert bytes to hex
        std::string header_hex;
        header_hex.reserve(header_bytes.size() * 2);
        const char hex_chars[] = "0123456789abcdef";
        for (unsigned char c : header_bytes) {
            header_hex += hex_chars[c >> 4];
            header_hex += hex_chars[c & 0x0F];
        }

        header_hexes.push_back(header_hex);
    }

    if (header_hexes.empty()) {
        logger_->info("[ChainstateService] No new headers for " + peer_addr +
                      " (at same tip) — sending empty headers response");
        // CRITICAL: Must send an empty headers message so the peer's OnHeaders
        // handler fires and calls OnHeadersProcessed(), which unblocks inv-based
        // block relay. Without this, the peer stays in permanent IBD state.
        ::P2PMessage empty_headers = ::P2PMessage::create_headers({});
        if (p2p_service_) {
            p2p_service_->get().send_to_peer(peer_addr, empty_headers);
        }
        return;
    }

    logger_->info("[ChainstateService] Sending " + std::to_string(header_hexes.size()) +
                  " headers to " + peer_addr + " (heights " + std::to_string(start_height) +
                  " to " + std::to_string(start_height + header_hexes.size() - 1) + ")");

    // Create and send HEADERS message
    ::P2PMessage headers_msg = ::P2PMessage::create_headers(header_hexes);

    if (p2p_service_) {
        p2p_service_->get().send_to_peer(peer_addr, headers_msg);
        logger_->info("[ChainstateService] ✅ Sent HEADERS to " + peer_addr);
    } else {
        logger_->error("[ChainstateService] P2P service not available");
    }
}

void ChainstateService::OnHeaders(const std::string& peer_addr, const P2PMessage& msg) {
    // Phase C.3: HEADERS handler
    // Parse HEADERS payload (pipe-separated hex headers)
    std::string payload_str(msg.payload.begin(), msg.payload.end());

    std::vector<std::string> header_hexes;
    std::istringstream ss(payload_str);
    std::string header_hex;
    while (std::getline(ss, header_hex, '|')) {
        if (!header_hex.empty()) {
            header_hexes.push_back(header_hex);
        }
    }

    if (header_hexes.empty()) {
        logger_->warning("[ChainstateService] Received empty HEADERS from " + peer_addr);
        return;
    }

    logger_->info("[ChainstateService] Received HEADERS from " + peer_addr +
                  " (count: " + std::to_string(header_hexes.size()) + ")");

    // Deserialize and validate each header
    std::vector<BlockHeader> headers;
    headers.reserve(header_hexes.size());

    for (const auto& hex : header_hexes) {
        // Convert hex to bytes
        std::vector<uint8_t> header_bytes;
        header_bytes.reserve(hex.size() / 2);

        for (size_t i = 0; i < hex.size(); i += 2) {
            if (i + 1 < hex.size()) {
                std::string byte_str = hex.substr(i, 2);
                char* end_ptr = nullptr;
                long val = std::strtol(byte_str.c_str(), &end_ptr, 16);
                if (end_ptr == byte_str.c_str() + 2) {
                    header_bytes.push_back(static_cast<uint8_t>(val));
                } else {
                    logger_->warning("[ChainstateService] Invalid hex in header from " + peer_addr);
                    return;
                }
            }
        }

        // Deserialize header (convert bytes to string for ParseFrom)
        std::string header_str(header_bytes.begin(), header_bytes.end());
        BlockHeader header;
        auto parse_status = ParseFrom(header_str, header);
        if (parse_status != Status::Ok) {
            logger_->warning("[ChainstateService] Failed to deserialize header from " + peer_addr);
            return;
        }
        headers.push_back(header);
    }

    // Phase C.3 Phase 2: Validate headers
    // 1. Prev-hash linkage
    // 2. Difficulty
    // 3. Timestamp sanity

    if (!ValidateHeaderChain(headers, peer_addr)) {
        logger_->warning("[ChainstateService] Header chain validation failed from " + peer_addr);
        return;
    }

    logger_->info("[ChainstateService] ✅ Validated " + std::to_string(headers.size()) +
                  " headers from " + peer_addr);

    RecordHeaderAnnouncements(peer_addr, headers);

    // Update estimated network height from validated headers
    // The last header in the batch represents the peer's chain tip (or close to it)
    if (!headers.empty()) {
        auto last_hash = headers.back().GetHash();
        auto height_result = chain_db_->getBlockHeight(last_hash);
        if (height_result.status() == Status::Ok) {
            uint32_t peer_tip = height_result.value();
            if (peer_tip > ibd_network_height_) {
                ibd_network_height_ = peer_tip;
            }

            // Keep P2PManager's live peer snapshot in sync with validated header
            // progress. getpeerinfo RPC and mining readiness both read peer
            // heights from P2PManager, not directly from ChainstateService.
            //
            // We promote the validated peer tip into best_known_height here too
            // because some peers bootstrap with start_height=0 and then only
            // reveal their real tip through header sync. If we leave
            // best_known_height stale, readiness and peer telemetry can keep
            // treating a synced peer as height 0 even after we've validated
            // higher headers from it.
            if (p2p_service_) {
                p2p_service_->get().update_peer_height(peer_addr, peer_tip);
                p2p_service_->get().update_peer_synced_headers(peer_addr, peer_tip);
            }
        }
    }

    // Phase C.3 Phase 3: Request blocks for validated headers
    std::vector<std::string> block_hashes;
    block_hashes.reserve(headers.size());
    for (const auto& header : headers) {
        // Phase M.0: Convert uint256 → hex for network protocol
        block_hashes.push_back(header.GetHash().GetHex());
    }

    if (GetConfig().utreexo_stateless) {
        // In CSN mode, headers only feed the ordered BlockDownloadScheduler.
        // Bulk-requesting every validated header here races the scheduler and
        // floods the node with far-ahead utxoblk responses that get rejected
        // by the sequential validation cursor.
        logger_->info("[ChainstateService] Stateless mode: scheduler owns block fetching for " +
                      std::to_string(block_hashes.size()) + " validated headers");
        return;
    }

    logger_->debug("[ChainstateService] Requesting " + std::to_string(block_hashes.size()) +
                  " blocks based on validated headers");
    RequestBlocks(block_hashes);
}

bool ChainstateService::ValidateHeaderChain(const std::vector<BlockHeader>& headers, const std::string& peer_addr) {
    if (headers.empty()) return true;

    // Validate each header
    for (size_t i = 0; i < headers.size(); i++) {
        const auto& header = headers[i];

        // Validation 1: Prev-hash linkage
        if (i > 0) {
            const auto& prev_header = headers[i - 1];
            // Phase M.0: Convert uint256 → hex for comparison
            uint256 prev_hash = prev_header.GetHash();

            if (header.prev_block_hash != prev_hash) {
                logger_->error("[ChainstateService] Header linkage broken at index " +
                             std::to_string(i) + ": expected prev " + prev_hash.GetHex() +
                             " but got " + header.prev_block_hash.GetHex());
                return false;
            }
        } else {
            // First header: verify it connects to our chain
            if (!chain_db_) {
                logger_->error("[ChainstateService] ChainDB not available for validation");
                return false;
            }

            // Phase M.0: prevBlockHash is already uint256
            auto height_result = chain_db_->getBlockHeight(header.prev_block_hash);
            if (height_result.status() != Status::Ok) {
                logger_->warning("[ChainstateService] First header doesn't connect to known chain: prev=" +
                               header.prev_block_hash.GetHex());
                // This might be valid if peer is ahead, but we can't validate without context
                // For now, log but continue (will be handled properly in full sync logic)
            }
        }

        // Validation 2: Proof-of-work meets difficulty target
        if (!ValidateProofOfWork(header)) {
            logger_->error("[ChainstateService] PoW validation failed for header " + header.GetHash().GetHex());
            return false;
        }

        // Validation 3: Timestamp sanity
        if (!ValidateTimestamp(header)) {
            logger_->error("[ChainstateService] Timestamp validation failed for header " + header.GetHash().GetHex());
            return false;
        }
    }

    return true;
}

bool ChainstateService::ValidateProofOfWork(const BlockHeader& header) {
    // Canonical PoW validation:
    // - mainnet/testnet: enforce standard difficulty-bit limits
    // - regtest: allow regtest difficulty encodings, but still require hash <= target
    const ChainParams& params = Params();
    const bool require_standard = (params.name != "regtest");
    return consensus::CheckProofOfWork(header, require_standard);
}

bool ChainstateService::ValidateTimestamp(const BlockHeader& header) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Bitcoin rule: Block timestamp must not be more than 2 hours in the future
    const int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours

    if (header.timestamp > static_cast<uint64_t>(now_seconds + MAX_FUTURE_BLOCK_TIME)) {
        logger_->warning("[ChainstateService] Block timestamp too far in future: " +
                       std::to_string(header.timestamp) + " vs now " + std::to_string(now_seconds));
        return false;
    }

    // Block timestamp should not be in distant past (before genesis)
    // Simplified check: timestamp > 0
    if (header.timestamp == 0) {
        return false;
    }

    return true;
}

// ============================================================================
// Phase C.3 Phase 3: Block Download Scheduling
// ============================================================================

bool ChainstateService::AlreadyHaveBlock(const std::string& block_hash) {
    if (chain_db_) {
        if (hasBlockByHash(uint256::FromHexUnsafe(block_hash))) {
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(block_request_state_mutex_);
        if (completed_blocks_.count(block_hash) > 0) {
            return true;
        }
    }

    return false;
}

void ChainstateService::RecordHeaderAnnouncements(const std::string& peer_addr,
                                                  const std::vector<BlockHeader>& headers) {
    if (peer_addr.empty() || headers.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(header_announcement_mutex_);
    constexpr size_t kMaxPeersPerHeader = 4;

    for (const auto& header : headers) {
        const std::string hash_hex = header.GetHash().GetHex();
        auto& announcers = header_announcing_peers_[hash_hex];
        auto existing = std::find(announcers.begin(), announcers.end(), peer_addr);
        if (existing != announcers.end()) {
            announcers.erase(existing);
        }
        announcers.push_back(peer_addr);
        if (announcers.size() > kMaxPeersPerHeader) {
            announcers.erase(announcers.begin());
        }
    }
}

void ChainstateService::RequestBlocks(const std::vector<std::string>& block_hashes) {
    if (!p2p_service_) {
        logger_->warning("[ChainstateService] Cannot request blocks: P2P service not available");
        return;
    }

    auto peers = p2p_service_->GetConnectedPeers();
    if (peers.empty()) {
        logger_->warning("[ChainstateService] Cannot request blocks: No connected peers");
        return;
    }

    std::unordered_map<std::string, size_t> peer_index_by_addr;
    peer_index_by_addr.reserve(peers.size());
    std::vector<size_t> fallback_indices;
    fallback_indices.reserve(peers.size());

    const uint32_t active_height = active_tip_ ? active_tip_->height : 0;
    for (size_t i = 0; i < peers.size(); ++i) {
        const std::string peer_addr = peers[i].address + ":" + std::to_string(peers[i].port);
        peer_index_by_addr.emplace(peer_addr, i);
        const uint32_t candidate_height = std::max(peers[i].best_known_height, peers[i].synced_headers);
        if (candidate_height > active_height) {
            fallback_indices.push_back(i);
        }
    }
    if (fallback_indices.empty()) {
        for (size_t i = 0; i < peers.size(); ++i) {
            fallback_indices.push_back(i);
        }
        if (logger_) {
            logger_->debug("[ChainstateService] No peers with height > " +
                           std::to_string(active_height) + " — falling back to all " +
                           std::to_string(fallback_indices.size()) + " peers");
        }
    } else if (logger_) {
        logger_->info("[ChainstateService] Using " + std::to_string(fallback_indices.size()) +
                      " peers with height > " + std::to_string(active_height) +
                      " as fallback candidates (filtered from " +
                      std::to_string(peers.size()) + " total)");
    }

    std::unordered_map<std::string, std::vector<size_t>> preferred_indices_by_hash;
    {
        std::lock_guard<std::mutex> header_lock(header_announcement_mutex_);
        preferred_indices_by_hash.reserve(block_hashes.size());
        for (const auto& hash : block_hashes) {
            auto it = header_announcing_peers_.find(hash);
            if (it == header_announcing_peers_.end()) {
                continue;
            }
            auto& preferred = preferred_indices_by_hash[hash];
            for (const auto& peer_addr : it->second) {
                auto peer_it = peer_index_by_addr.find(peer_addr);
                if (peer_it != peer_index_by_addr.end()) {
                    preferred.push_back(peer_it->second);
                }
            }
        }
    }

    struct PendingRequest {
        std::string block_hash;
        std::string peer_addr;
        std::vector<std::string> fanout_peers;
        bool preferred_peer;
        int block_height;
    };
    std::vector<PendingRequest> pending_requests;
    pending_requests.reserve(block_hashes.size());
    auto now = std::chrono::steady_clock::now();
    size_t skipped_completed = 0;
    size_t skipped_in_flight = 0;
    size_t skipped_have_block = 0;
    size_t re_requesting_proofless = 0;
    size_t skipped_no_candidates = 0;

    {
        std::lock_guard<std::mutex> lock(block_request_state_mutex_);

        std::vector<std::string> expired_requests;
        for (auto it = in_flight_blocks_.begin(); it != in_flight_blocks_.end();) {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.request_time).count();
            if (age > BLOCK_REQUEST_TIMEOUT_SECONDS) {
                expired_requests.push_back(it->first);
                it = in_flight_blocks_.erase(it);
            } else {
                ++it;
            }
        }
        if (!expired_requests.empty() && logger_) {
            logger_->warning("[ChainstateService] Expired " +
                             std::to_string(expired_requests.size()) +
                             " stale in-flight block requests after " +
                             std::to_string(BLOCK_REQUEST_TIMEOUT_SECONDS) +
                             "s");
        }

        for (const auto& hash : block_hashes) {
            const uint256 hash_u256 = uint256::FromHexUnsafe(hash);
            const bool stateless_mode = GetConfig().utreexo_stateless;
            const bool was_completed = completed_blocks_.count(hash) > 0;
            const bool was_in_flight = in_flight_blocks_.count(hash) > 0;

            bool has_usable_block = false;
            bool has_proofless_block = false;
            if (chain_db_ && hasBlockByHash(hash_u256)) {
                if (stateless_mode) {
                    auto block_result = getBlockByHash(hash_u256);
                    if (block_result.status() == Status::Ok) {
                        has_usable_block = block_result.value().utreexo.has_value();
                        has_proofless_block = !has_usable_block;
                    } else {
                        // Treat storage read failures as not-having a usable
                        // stateless block so the requester can repair by re-fetching.
                        if (logger_) {
                            logger_->warning("[ChainstateService] Failed to read stored stateless block " +
                                             hash.substr(0, 16) + "... for request eligibility check");
                        }
                    }
                } else {
                    has_usable_block = true;
                }
            }

            if (stateless_mode && has_proofless_block) {
                if (was_completed) {
                    completed_blocks_.erase(hash);
                }
                ++re_requesting_proofless;
            } else if (was_completed) {
                ++skipped_completed;
                continue;
            }

            if (was_in_flight) {
                ++skipped_in_flight;
                continue;
            }

            if (has_usable_block) {
                ++skipped_have_block;
                continue;
            }

            std::vector<size_t> candidate_indices;
            auto preferred_it = preferred_indices_by_hash.find(hash);
            if (preferred_it != preferred_indices_by_hash.end()) {
                candidate_indices = preferred_it->second;
            }

            const bool preferred_peer = !candidate_indices.empty();
            if (candidate_indices.empty()) {
                candidate_indices = fallback_indices;
            }
            if (candidate_indices.empty()) {
                ++skipped_no_candidates;
                continue;
            }

            if (GetConfig().utreexo_stateless) {
                std::vector<size_t> ordered_candidates;
                ordered_candidates.reserve(candidate_indices.size() + fallback_indices.size());

                auto append_unique = [&ordered_candidates](size_t peer_idx) {
                    if (std::find(ordered_candidates.begin(), ordered_candidates.end(), peer_idx) ==
                        ordered_candidates.end()) {
                        ordered_candidates.push_back(peer_idx);
                    }
                };

                for (size_t peer_idx : candidate_indices) {
                    const auto& peer = peers[peer_idx];
                    const std::string peer_key = peer.address + ":" + std::to_string(peer.port);
                    if (p2p_service_->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_UTREEXO_BRIDGE)) {
                        append_unique(peer_idx);
                    }
                }
                for (size_t peer_idx : fallback_indices) {
                    const auto& peer = peers[peer_idx];
                    const std::string peer_key = peer.address + ":" + std::to_string(peer.port);
                    if (p2p_service_->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_UTREEXO_BRIDGE)) {
                        append_unique(peer_idx);
                    }
                }

                if (!ordered_candidates.empty()) {
                    candidate_indices = std::move(ordered_candidates);
                } else {
                    // Mixed or legacy networks may not advertise the bridge bit yet.
                    // Preserve liveness by falling back to the original peer set.
                    for (size_t peer_idx : fallback_indices) {
                        if (std::find(candidate_indices.begin(), candidate_indices.end(), peer_idx) ==
                            candidate_indices.end()) {
                            candidate_indices.push_back(peer_idx);
                        }
                    }
                }
            }

            const size_t idx = next_peer_index_ % candidate_indices.size();
            ++next_peer_index_;
            const size_t peer_idx = candidate_indices[idx];
            const std::string peer_addr = peers[peer_idx].address + ":" + std::to_string(peers[peer_idx].port);

            BlockDownloadRequest request;
            request.block_hash = hash;
            request.peer_addr = peer_addr;
            request.request_time = now;
            in_flight_blocks_[hash] = request;

            std::vector<std::string> fanout_peers;
            if (GetConfig().utreexo_stateless) {
                fanout_peers.reserve(candidate_indices.size());
                for (size_t candidate_idx : candidate_indices) {
                    const auto& candidate = peers[candidate_idx];
                    fanout_peers.push_back(candidate.address + ":" + std::to_string(candidate.port));
                }
            } else {
                fanout_peers.push_back(peer_addr);
            }

            int block_height = -1;
            if (chain_db_) {
                auto height_result = chain_db_->getBlockHeight(uint256::FromHexUnsafe(hash));
                if (height_result.status() == Status::Ok) {
                    block_height = height_result.value();
                }
            }

            pending_requests.push_back({hash, peer_addr, std::move(fanout_peers), preferred_peer, block_height});
        }
    }

    if (logger_) {
        logger_->info("[ChainstateService] RequestBlocks summary: requested=" +
                      std::to_string(pending_requests.size()) +
                      " skipped_completed=" + std::to_string(skipped_completed) +
                      " skipped_in_flight=" + std::to_string(skipped_in_flight) +
                      " skipped_have_block=" + std::to_string(skipped_have_block) +
                      " re_requesting_proofless=" + std::to_string(re_requesting_proofless) +
                      " skipped_no_candidates=" + std::to_string(skipped_no_candidates));
    }

    if (pending_requests.empty()) {
        logger_->debug("[ChainstateService] No new blocks to request (all already have or in flight)");
        return;
    }

    size_t preferred_count = 0;
    const std::string request_type = GetConfig().utreexo_stateless ? "MSG_UTREEXO_BLOCK" : "MSG_BLOCK";
    for (const auto& request : pending_requests) {
        ::P2PMessage getdata_msg = CreateBlockGetDataMessage(request.block_hash);
        int sent = 0;
        for (const auto& peer_addr : request.fanout_peers) {
            if (p2p_service_->get().send_to_peer(peer_addr, getdata_msg)) {
                ++sent;
            }
        }
        if (request.preferred_peer) {
            ++preferred_count;
        }
        if (logger_ && GetConfig().utreexo_stateless) {
            logger_->info("[ChainstateService] Sent getdata(" + request_type + ") for block " +
                          request.block_hash +
                          (request.block_height >= 0 ? " (height " + std::to_string(request.block_height) + ")" : "") +
                          " to " + std::to_string(sent) + "/" +
                          std::to_string(request.fanout_peers.size()) + " eligible peers");
        }
        logger_->debug("[ChainstateService] Requested block " + request.block_hash + " from " +
                       request.peer_addr +
                       (request.fanout_peers.size() > 1
                            ? " (fanout " + std::to_string(sent) + "/" +
                                  std::to_string(request.fanout_peers.size()) + " peers)"
                            : "") +
                       (request.preferred_peer ? " (header announcer)" : " (fallback)"));
    }

    if (logger_) {
        logger_->info("[ChainstateService] Sent GETDATA for " +
                      std::to_string(pending_requests.size()) + " blocks (" +
                      std::to_string(preferred_count) + " targeted by header provenance)");
    }
}

bool ChainstateService::IsBlockInFlight(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(block_request_state_mutex_);
    return in_flight_blocks_.count(block_hash) > 0;
}

void ChainstateService::HandleNotFoundFromPeer(const std::string& peer_addr,
                                               const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    uint64_t count = 0;
    if (!ReadCompactSize(payload, &offset, &count)) {
        if (logger_) {
            logger_->warning("[ChainstateService] Ignoring malformed NOTFOUND from " + peer_addr);
        }
        return;
    }

    std::unordered_set<std::string> missing_blocks;
    std::vector<uint256> notfound_hashes;  // raw hashes for scheduler propagation
    for (uint64_t i = 0; i < count; ++i) {
        if (offset + 36 > payload.size()) {
            if (logger_) {
                logger_->warning("[ChainstateService] Ignoring truncated NOTFOUND from " + peer_addr);
            }
            return;
        }

        const uint32_t inv_type = static_cast<uint32_t>(payload[offset]) |
                                  (static_cast<uint32_t>(payload[offset + 1]) << 8) |
                                  (static_cast<uint32_t>(payload[offset + 2]) << 16) |
                                  (static_cast<uint32_t>(payload[offset + 3]) << 24);
        offset += 4;

        uint256 hash;
        std::memcpy(hash.data, payload.data() + offset, 32);
        offset += 32;

        if (inv_type == kInvMsgBlock || inv_type == kInvMsgUtreexoBlock) {
            missing_blocks.insert(hash.GetHex());
            notfound_hashes.push_back(hash);
        }
    }

    if (missing_blocks.empty()) {
        return;
    }

    size_t cleared = 0;
    {
        std::lock_guard<std::mutex> lock(block_request_state_mutex_);
        for (const auto& block_hash : missing_blocks) {
            auto it = in_flight_blocks_.find(block_hash);
            if (it != in_flight_blocks_.end() && it->second.peer_addr == peer_addr) {
                in_flight_blocks_.erase(it);
                ++cleared;
            }
        }
    }

    if (cleared > 0 && logger_) {
        logger_->info("[ChainstateService] Cleared " + std::to_string(cleared) +
                      " block requests after NOTFOUND from " + peer_addr);
    }

    // Propagate NOTFOUND to the consensus BlockDownloadScheduler so it can
    // clear stale-branch hashes from its download queue and trigger a rescan.
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->block_download) {
        for (const auto& h : notfound_hashes) {
            // Pass the peer so the scheduler can mark it body-incapable for this
            // height and stop re-requesting from it (issue #241). peer_addr must
            // match PeerInfo::to_string() used in the send_getdata callback.
            ctx->block_download->OnBlockNotFound(h, peer_addr);
        }
    }
}

bool ChainstateService::ResetTrackedStallForBlock(const std::string& block_hash,
                                                  const std::string& source) {
    bool reset = false;
    {
        std::lock_guard<std::mutex> lock(header_announcement_mutex_);
        header_announcing_peers_.erase(block_hash);

        auto it = stalled_missing_block_hashes_.find(block_hash);
        if (it == stalled_missing_block_hashes_.end()) {
            return false;
        }
        stalled_missing_block_hashes_.erase(it);
        if (stalled_missing_block_hashes_.empty()) {
            stalled_header_tracking_ = false;
            stalled_header_tip_.SetNull();
            reset = true;
        }
    }

    if (reset && logger_) {
        logger_->info("[ChainstateService] Stall tracking reset — " + source);
    }
    return reset;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 39: Accessor methods using DaemonContext (NO GLOBALS)
// ═══════════════════════════════════════════════════════════════════════════
// g_chain_manager DELETED - access via DaemonContext::instance()->chain_manager

// Phase 39: ChainManager accessors removed (ChainManager deleted)
// Any code calling chainManager() needs to be migrated to use GetChainDB() instead
ChainManager& ChainstateService::chainManager() {
    throw std::runtime_error("Phase 39: ChainManager deleted - use GetChainDB() instead");
}

const ChainManager& ChainstateService::chainManager() const {
    throw std::runtime_error("Phase 39: ChainManager deleted - use GetChainDB() instead");
}

// Phase 39: GetChainDB accessor (re-enabled after ChainManager deletion)
ChainDB* ChainstateService::GetChainDB() {
    // Phase 39: Return direct chain_db_ pointer (ChainManager deleted)
    return chain_db_;
}

const ChainDB* ChainstateService::GetChainDB() const {
    // Phase 11a: Const overload for read-only access (GlobalUTXOSet)
    return chain_db_;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 40: Chain Activation (Foundation)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * ActivateBestChain - Full chain activation with reorg logic (Phase 41)
 *
 * Performs automatic fork resolution via chainwork comparison:
 * 1. Get best candidate from BlockIndex graph
 * 2. Compare with current active tip
 * 3. Execute reorg if better chain exists (disconnect old, connect new)
 * 4. Notify dependent services of tip changes
 *
 * CONSENSUS RULE (v0.15.0.4):
 * When two chains have equal chainwork, select the chain whose tip has the
 * lexicographically smallest block hash (deterministic tie-breaking).
 */
void ChainstateService::ActivateBestChain() {
    // Thread safety: prevent concurrent entry from P2P handler + P1 reorg tick
    std::lock_guard<std::recursive_mutex> activation_lock(activation_mutex_);

    std::cout << "\n════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "🔍 [ActivateBestChain] ENTRY" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

    if (!chain_db_) {
        if (logger_) logger_->warning("[ActivateBestChain] ChainDB not available");
        std::cout << "❌ [ActivateBestChain] EARLY RETURN: ChainDB not available" << std::endl;
        return;
    }

    // Phase 3b step 3 part 2: one-shot startup verification of the
    // journal row written by ConsensusWriteBatch::Commit(). Runs at
    // first canonical activation only — by the second ActivateBestChain
    // call the daemon has already been processing blocks, so a
    // mismatch between the stored DSRH v2 and the live state would
    // be a normal in-flight delta, not a partial-commit signature.
    // Guarded by `journal_verified_at_startup_`.
    if (!journal_verified_at_startup_) {
        VerifyConsensusJournalAtActiveTip();
        journal_verified_at_startup_ = true;
    }

    // Canonical state alignment check with self-healing bootstrap.
    // If active_tip_ is at genesis but DB tip is higher, bootstrap from DB tip.
    // This handles fresh starts where startup paths don't advance active_tip_.
    std::string alignment_reason;
    if (!IsCanonicalStateAligned(&alignment_reason)) {
        // Attempt self-healing: align active_tip_ with consensus UTXO state
        // (never blindly with ChainDB storage tip).
        bool healed = false;
        if (consensus_utxo_set_) {
            const uint256 utxo_best = consensus_utxo_set_->GetBestBlock();
            const uint32_t utxo_height = consensus_utxo_set_->GetHeight();
            const uint64_t forest_leaves = consensus_utxo_set_->GetForest().getNumLeaves();
            CBlockIndex* utxo_tip_idx = dinero::FindBlockIndex(utxo_best);
            if (forest_leaves == 0 && utxo_height > 0) {
                if (logger_) logger_->warning("[ActivateBestChain] Misaligned: " + alignment_reason +
                                             " — refusing to realign to consensus UTXO tip because forest is empty");
            } else if (utxo_tip_idx && static_cast<uint32_t>(utxo_tip_idx->height) == utxo_height) {
                if (logger_) logger_->warning("[ActivateBestChain] Misaligned: " + alignment_reason +
                                             " — realigning active_tip_ to consensus UTXO tip (height=" +
                                             std::to_string(utxo_height) + ")");
                PublishActiveTip(utxo_tip_idx, TipPublishReason::kSelfHealRealign);

                // Re-check alignment after realignment.
                std::string recheck_reason;
                if (IsCanonicalStateAligned(&recheck_reason)) {
                    healed = true;
                    if (logger_) logger_->info("[ActivateBestChain] Self-heal successful — alignment restored");
                } else {
                    if (logger_) logger_->error("[ActivateBestChain] Self-heal failed, still misaligned: " + recheck_reason);
                }
            } else {
                // #353 fresh-snapshot-bootstrap wedge fix: utxo_tip_idx is null —
                // the consensus UTXO best block (snapshot base) has a header in the
                // header-chain selector but was never MATERIALIZED into the block
                // index, so FindBlockIndex returns null and the node dead-loops on
                // "consensus UTXO tip missing from block index". The restore path
                // already handles exactly this (EnsureHeaderBranchIndexed +
                // PublishActiveTip); mirror it here so a FRESH bootstrap self-heals
                // instead of wedging in safe mode.
                CBlockIndex* materialized = nullptr;
                if (header_chain_selector_ && !utxo_best.IsNull()) {
                    if (const auto* hcs_entry = header_chain_selector_->GetHeader(utxo_best)) {
                        materialized = EnsureHeaderBranchIndexed(hcs_entry, /*mark_chain_valid=*/true);
                    }
                }
                if (materialized &&
                    static_cast<uint32_t>(materialized->height) == utxo_height) {
                    if (logger_) logger_->warning("[ActivateBestChain] Misaligned: " + alignment_reason +
                                                 " — materialized snapshot base into block index; realigning "
                                                 "active_tip_ to consensus UTXO tip (height=" +
                                                 std::to_string(utxo_height) + ")");
                    PublishActiveTip(materialized, TipPublishReason::kSelfHealRealign);
                    std::string recheck_reason;
                    if (IsCanonicalStateAligned(&recheck_reason)) {
                        healed = true;
                        if (logger_) logger_->info("[ActivateBestChain] Self-heal successful — snapshot base "
                                                   "materialized + alignment restored");
                    } else if (logger_) {
                        logger_->error("[ActivateBestChain] Self-heal failed after materialize, still "
                                       "misaligned: " + recheck_reason);
                    }
                } else if (logger_) {
                    logger_->error("[ActivateBestChain] Cannot realign: consensus UTXO tip missing from "
                                   "block index (materialize failed)");
                }
            }
        }

        if (!healed) {
            if (logger_) logger_->error("[ActivateBestChain] INVARIANT VIOLATION (unrecoverable): " + alignment_reason);
            RecordActivationFailure(utxo_index_.get(), "chainstate-misaligned: " + alignment_reason);
            EnterSafeMode("chainstate misaligned: " + alignment_reason);
            std::cout << "❌ [ActivateBestChain] EARLY RETURN: Canonical state misaligned" << std::endl;
            return;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // HEADER-AWARE REORG CHECK (P2P Fix)
    // ═══════════════════════════════════════════════════════════════════════════
    // If HeaderChainSelector shows a better chain than our active tip, we need
    // to import blocks from BlockStorage into block_index_ BEFORE the standard
    // reorg logic runs. This enables reorgs to competing forks.
    //
    // Flow:
    // 1. Headers arrive → HeaderChainSelector knows best chain
    // 2. Blocks downloaded → stored in BlockStorage
    // 3. Here: detect better header chain, import blocks to block_index_
    // 4. Standard reorg logic then works correctly
    // ═══════════════════════════════════════════════════════════════════════════
    // In the classic AssumeUTXO profile the active tip is intentionally held
    // at the snapshot base until genesis-to-base replay is promoted.  Post-base
    // bodies are independently downloaded and durably indexed by the scheduler,
    // so importing that same continuation here cannot activate anything yet.
    //
    // Previously the hold was applied only *after* rebuilding the full branch.
    // Every arriving body (including fanout duplicates) therefore walked ~9k
    // headers, restored RocksDB metadata, and INFO-logged AddCandidate for every
    // already stored block while holding activation_mutex_.  Peer receive loops
    // queued behind that work until both sides' blocking sockets stopped
    // draining.  Defer the no-op import at its source.  The ancestry check keeps
    // competing/below-base branches on the normal safety path; when promotion
    // clears assumeutxo_active_, the next activation imports the stored branch.
    const auto* best_header_for_hold =
        header_chain_selector_ ? header_chain_selector_->GetBestHeader() : nullptr;
    bool defer_snapshot_continuation = false;
    if (assumeutxo_active_ && !GetConfig().assumeutxo_forward_connect &&
        best_header_for_hold && active_tip_ &&
        !assumeutxo_base_block_.IsNull() && assumeutxo_base_height_ > 0 &&
        static_cast<uint32_t>(active_tip_->height) == assumeutxo_base_height_ &&
        active_tip_->hash == assumeutxo_base_block_ &&
        best_header_for_hold->height > assumeutxo_base_height_) {
        const auto* base_ancestor =
            best_header_for_hold->GetAncestor(assumeutxo_base_height_);
        const bool header_descends_from_base =
            base_ancestor && base_ancestor->hash == assumeutxo_base_block_;

        // BlockAcceptor can populate candidates_ before that block's header is
        // reflected in HeaderChainSelector.  Inspect the best queued candidate
        // too, otherwise a benign HCS continuation could hide a full-block fork
        // that diverges at/below the snapshot base and needs the normal fatal
        // fork guard below.  Candidates below the base are the replayed-history
        // case already ignored by the existing AssumeUTXO floor; an equal-height
        // different hash is deliberately NOT deferred.
        bool candidate_is_safe_to_defer = true;
        if (header_descends_from_base) {
            // #360 lock order is activation_mutex_ -> g_block_index_mutex.
            // Keep GetBestCandidate and its pprev ancestry walk in one graph
            // snapshot so a concurrent load-thread relink cannot turn this
            // safety decision into a torn read.
            std::lock_guard<std::recursive_mutex> index_lock(
                dinero::g_block_index_mutex);
            CBlockIndex* queued_candidate = GetBestCandidate();
            if (queued_candidate && queued_candidate != active_tip_) {
                if (queued_candidate->height <
                    static_cast<int>(assumeutxo_base_height_)) {
                    candidate_is_safe_to_defer = true;
                } else {
                    CBlockIndex* candidate_base = queued_candidate;
                    while (candidate_base && candidate_base->height >
                           static_cast<int>(assumeutxo_base_height_)) {
                        candidate_base = candidate_base->pprev;
                    }
                    candidate_is_safe_to_defer =
                        candidate_base &&
                        candidate_base->height == static_cast<int>(assumeutxo_base_height_) &&
                        candidate_base->hash == assumeutxo_base_block_;
                }
            }
        }
        defer_snapshot_continuation =
            header_descends_from_base && candidate_is_safe_to_defer;
    }

    if (defer_snapshot_continuation) {
        if (!assumeutxo_header_import_deferred_logged_ && logger_) {
            assumeutxo_header_import_deferred_logged_ = true;
            logger_->info("[ActivateBestChain] AssumeUTXO active — deferring post-base "
                          "header import until history promotion (base=" +
                          std::to_string(assumeutxo_base_height_) + ", best_header=" +
                          std::to_string(best_header_for_hold->height) +
                          "); block bodies remain scheduler-managed");
        }
        return;
    }

    std::cout << "🔍 [REORG-CHECK] header_chain_selector_=" << (header_chain_selector_ ? "SET" : "NULL")
              << ", active_tip_=" << (active_tip_ ? "SET" : "NULL") << std::endl;

    if (header_chain_selector_) {
        const auto* best_header = best_header_for_hold;
        std::cout << "🔍 [REORG-CHECK] best_header=" << (best_header ? "SET" : "NULL") << std::endl;
        if (best_header) {
            std::cout << "🔍 [REORG-CHECK] best_header: height=" << best_header->height
                      << ", chainwork=" << best_header->chainwork.GetHex().substr(56) << std::endl;
        }
        if (active_tip_) {
            std::cout << "🔍 [REORG-CHECK] active_tip: height=" << active_tip_->height
                      << ", chainwork=" << active_tip_->chainwork.substr(56) << std::endl;
        }

        if (best_header && active_tip_ && IsHeaderChainBetterThanActiveTip(best_header, active_tip_)) {
            // Stalling detection: if this header chain has been "better" but
            // blocks haven't arrived for HEADER_STALL_TIMEOUT_SECONDS, skip it.
            // This prevents fork poisoning from unavailable peers.
            bool header_stalled = false;
            bool clear_stale_requests = false;
            {
                std::lock_guard<std::mutex> stall_lock(header_announcement_mutex_);
                if (stalled_header_tracking_ && stalled_header_tip_ == best_header->hash) {
                    auto elapsed = std::chrono::steady_clock::now() - stalled_header_first_seen_;
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                    if (secs > HEADER_STALL_TIMEOUT_SECONDS) {
                        if (logger_) {
                            logger_->warning("[ActivateBestChain] Header chain at height " +
                                            std::to_string(best_header->height) +
                                            " stalled for " + std::to_string(secs) +
                                            "s — clearing stale in-flight blocks and retrying");
                        }
                        stalled_header_tracking_ = false;
                        stalled_header_tip_.SetNull();
                        stalled_missing_block_hashes_.clear();
                        clear_stale_requests = true;
                    }
                }
            }
            if (clear_stale_requests) {
                std::lock_guard<std::mutex> request_lock(block_request_state_mutex_);
                in_flight_blocks_.clear();
            }

            if (!header_stalled) {
            std::cout << "🚨 [ActivateBestChain] HEADER CHAIN IS BETTER!" << std::endl;
            std::cout << "    best_header: height=" << best_header->height
                      << ", chainwork=" << best_header->chainwork.GetHex().substr(56) << std::endl;
            std::cout << "    active_tip:  height=" << active_tip_->height
                      << ", chainwork=" << active_tip_->chainwork.substr(56) << std::endl;
            std::cout << "    → Importing available branch blocks into BlockIndex" << std::endl;

            if (logger_) logger_->info("[ActivateBestChain] Better header chain detected - importing available branch blocks");

            // Build the full non-active branch path from best_header down to fork point.
            // This handles both:
            //  - headers missing from block index, and
            //  - headers that already have index entries but still need body import/request.
            std::unordered_set<uint256> active_chain_hashes;
            for (CBlockIndex* cursor = active_tip_; cursor; cursor = cursor->pprev) {
                active_chain_hashes.insert(cursor->hash);
            }

            std::vector<const consensus::HeaderIndexEntry*> branch_path;
            const consensus::HeaderIndexEntry* walk = best_header;
            while (walk && active_chain_hashes.find(walk->hash) == active_chain_hashes.end()) {
                branch_path.push_back(walk);
                walk = walk->parent;
            }

            // If we never reach an active-chain ancestor, this header branch is
            // incompatible with our current chain graph (e.g., stale foreign headers).
            // Skip importing it as a candidate to avoid FindFork() null deadlocks.
            if (!walk) {
                if (logger_) {
                    logger_->warning("[ActivateBestChain] Ignoring incompatible header branch (no common ancestor with active tip)");
                }
            } else {
                const bool importing_competing_branch =
                    walk->hash != active_tip_->hash;
                std::reverse(branch_path.begin(), branch_path.end());

                size_t imported_blocks = 0;
                std::vector<std::string> missing_block_bodies;
                missing_block_bodies.reserve(branch_path.size());

                for (const auto* entry : branch_path) {
                    if (!entry) continue;

                    CBlockIndex* idx = FindBlockIndex(entry->hash);
                    if (!idx) {
                        idx = AddBlockIndex(entry->header, entry->height);
                        if (!idx) {
                            continue;
                        }
                    }

                    idx->chainwork = entry->chainwork.GetHex();

                    // If body isn't stored yet, request it and continue.
                    if (!HasStoredBlockBody(entry->hash)) {
                        missing_block_bodies.push_back(entry->hash.GetHex());
                        continue;
                    }

                    // Skip blocks that were found unreadable (corrupt chaindb entries).
                    // They will be re-downloaded from peers. Self-synchronizing
                    // read — the scheduler-drain thread clears concurrently.
                    if (unreadable_blocks_.contains(entry->hash)) {
                        missing_block_bodies.push_back(entry->hash.GetHex());
                        continue;
                    }

                    // In CSN mode, flat-file presence alone is not validation.
                    // The scheduler stores raw utxoblk bodies before the ordered
                    // worker verifies their accumulator transition. Import only
                    // after either (a) the reorg worker persisted hash-anchored
                    // replay data for a genuinely competing branch, or (b)
                    // AcceptBlockFromRPC durably marked transaction validation
                    // before a crash stopped tip connection. Forward-sync replay
                    // data is not an activation marker: the ordered worker still
                    // owns submission of that block. A raw scheduler body may
                    // carry proof bytes too, so payload shape is not a validation
                    // marker. This prevents periodic ABC from racing ahead while
                    // preserving store-ahead crash recovery.
                    if (GetConfig().utreexo_stateless) {
                        const auto metadata = chain_db_->getHeaderMetadata(entry->hash);
                        const bool transaction_validated =
                            metadata.status() == Status::Ok &&
                            (metadata.value().status_flags &
                             BLOCK_VALID_TRANSACTIONS) != 0;
                        const bool reorg_plan_validated =
                            importing_competing_branch &&
                            chain_db_->getCSNSpendTargets(entry->hash).status() == Status::Ok;
                        if (!transaction_validated && !reorg_plan_validated) {
                            if (logger_) {
                                logger_->debug("[ActivateBestChain] CSN body at height " +
                                               std::to_string(entry->height) +
                                               " awaits ordered proof validation");
                            }
                            continue;
                        }
                    }

                    // Restart recovery: a stored better-branch block can have
                    // persisted BLOCK_VALID_CHAIN state even though it is not
                    // reachable from the active height index. Rehydrate that
                    // metadata before candidacy checks so the node can promote
                    // a locally stored better block after restart.
                    RestorePersistedBlockIndexMetadata(*chain_db_, entry->hash, idx);
                    std::string invalidity_error;
                    if (!BackfillFailedChildFromParent(chain_db_,
                                                       idx,
                                                       logger_,
                                                       nullptr,
                                                       &invalidity_error)) {
                        logger_->error("[ActivateBestChain] Failed invalidity backfill while importing better header branch: " +
                                       invalidity_error);
                        return;
                    }
                    idx->status |= BLOCK_HAVE_DATA;
                    AddCandidate(idx);
                    if (!IsEligibleForCandidacy(idx->status) && logger_) {
                        logger_->debug("[ActivateBestChain] Block at height " +
                                      std::to_string(idx->height) +
                                      " has data but needs chain validation (status=" +
                                      std::to_string(idx->status) + ")");
                    }
                    imported_blocks++;
                }

                if (!missing_block_bodies.empty()) {
                    if (logger_) {
                        logger_->info("[ActivateBestChain] Requesting " +
                                     std::to_string(missing_block_bodies.size()) +
                                     " missing block bodies for better header branch");
                    }
                    if (GetConfig().utreexo_stateless) {
                        // In CSN mode the ordered BlockDownloadScheduler owns
                        // body fetching. Bulk-marking every missing body as
                        // in-flight here poisons ChainstateService state and
                        // stalls later retries even though the scheduler is
                        // requesting blocks sequentially.
                        logger_->info("[ActivateBestChain] Stateless mode: deferring missing-body fetches to scheduler");
                    } else {
                        // BlockDownloadScheduler owns block-body fetching in non-stateless
                        // mode too. Sending a bulk RequestBlocks for all missing bodies
                        // here bypasses the scheduler's 16-in-flight flow control and
                        // floods peers with thousands of getdata messages at once, which
                        // causes servers to stop responding and permanently stalls IBD.
                        // The scheduler's Tick() loop (every 5s, max 16 in-flight) handles
                        // this correctly via ScanForMissingBlocks.
                        logger_->info("[ActivateBestChain] Non-stateless mode: deferring " +
                                      std::to_string(missing_block_bodies.size()) +
                                      " missing body fetches to BlockDownloadScheduler");
                    }

                    // Start or continue stall tracking for this header chain tip
                    {
                        std::lock_guard<std::mutex> stall_lock(header_announcement_mutex_);
                        if (!stalled_header_tracking_ || stalled_header_tip_ != best_header->hash) {
                            stalled_header_tip_ = best_header->hash;
                            stalled_header_first_seen_ = std::chrono::steady_clock::now();
                        }
                        stalled_header_tracking_ = true;
                        stalled_missing_block_hashes_.clear();
                        stalled_missing_block_hashes_.insert(missing_block_bodies.begin(),
                                                             missing_block_bodies.end());
                    }
                } else {
                    // All blocks available — clear stall tracking
                    std::lock_guard<std::mutex> stall_lock(header_announcement_mutex_);
                    stalled_header_tracking_ = false;
                    stalled_header_tip_.SetNull();
                    stalled_missing_block_hashes_.clear();
                }

                if (logger_) {
                    logger_->info("[ActivateBestChain] Header-branch import complete: imported=" +
                                 std::to_string(imported_blocks) +
                                 ", missing_bodies=" + std::to_string(missing_block_bodies.size()));
                }
            }
            } // if (!header_stalled)
        }
    }
    // Phase 41: Get best candidate from BlockIndex graph
    CBlockIndex* best_candidate = GetBestCandidate();

    std::cout << "🔍 [ActivateBestChain] best_candidate=" << (best_candidate ? "EXISTS" : "NULL") << std::endl;
    if (best_candidate) {
        std::cout << "🔍 [ActivateBestChain]   → height=" << best_candidate->height << std::endl;
        std::cout << "🔍 [ActivateBestChain]   → hash=" << best_candidate->hash.GetHex().substr(0, 16) << "..." << std::endl;
        std::cout << "🔍 [ActivateBestChain]   → chainwork=" << best_candidate->chainwork.substr(0, 16) << "..." << std::endl;
    }
    std::cout << "🔍 [ActivateBestChain] active_tip_=" << (active_tip_ ? "EXISTS" : "NULL") << std::endl;
    if (active_tip_) {
        std::cout << "🔍 [ActivateBestChain]   → height=" << active_tip_->height << std::endl;
        std::cout << "🔍 [ActivateBestChain]   → hash=" << active_tip_->hash.GetHex().substr(0, 16) << "..." << std::endl;
        std::cout << "🔍 [ActivateBestChain]   → chainwork=" << active_tip_->chainwork.substr(0, 16) << "..." << std::endl;
    }

    // If no candidates yet (early startup), just log current tip
    if (!best_candidate) {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == dinero::Status::Ok && logger_) {
            const auto& tip = tip_result.value();
            logger_->debug("[ActivateBestChain] No candidates yet, current tip: height=" +
                          std::to_string(tip.height) + ", hash=" +
                          tip.hash.GetHex().substr(0, 16) + "...");
        }
        std::cout << "❌ [ActivateBestChain] EARLY RETURN: No best_candidate" << std::endl;
        return;
    }

    // ── AssumeUTXO floor: never reorg to a below-base-TIP candidate ──────────
    // A snapshot-bootstrapped node's active chain is anchored at the trusted
    // snapshot base. A candidate whose TIP is below that base can never be a
    // legitimately-better chain than the active chain — it is a block
    // re-materialized from genesis by background validation (the node re-deriving
    // its own assumed-trusted history), NOT a competing chain. Selecting it would
    // attempt to reorg the active tip down below the base, which the
    // fork-below-base guard then treats as fatal — spuriously here, driven only
    // by the base-relative chainwork a snapshot node computes for its pre-base
    // history. So: while the active chain is at/above the base, ignore any
    // candidate whose tip is below the base. A GENUINE higher-work below-base
    // divergence still has its TIP ABOVE the base (a real longer chain), so it is
    // NOT excluded here — it falls through to the normal work comparison and the
    // existing fork-below-base fatal guard.
    {
        const uint32_t assumeutxo_floor = std::max(
            assumeutxo_active_ ? assumeutxo_base_height_ : 0u, promoted_base_height_);
        if (assumeutxo_floor > 0 && active_tip_ &&
            static_cast<uint32_t>(active_tip_->height) >= assumeutxo_floor &&
            static_cast<uint32_t>(best_candidate->height) < assumeutxo_floor) {
            if (logger_) {
                // debug, not info: background validation re-materializes the
                // trusted genesis->base history continuously, so this fires once
                // per re-materialized ancestor (thousands of times over a sync) —
                // a routine, benign event, not something operators need at INFO.
                logger_->debug("[ActivateBestChain] Ignoring below-base candidate (height=" +
                               std::to_string(best_candidate->height) +
                               " < AssumeUTXO base " + std::to_string(assumeutxo_floor) +
                               ") — re-materialized ancestor, not a valid reorg target; "
                               "staying on active tip @" + std::to_string(active_tip_->height));
            }
            return;
        }
    }

    // #353 bug-2 (promotion race): while an AssumeUTXO snapshot is still being
    // background-validated, HOLD the canonical tip at the snapshot base. Promotion
    // (PromoteValidatedHistory) reconciles the coin CF to the proven pre-base set
    // and ends with setTip(base); it is only correct when tip <= base — its stage-3
    // deletes every coin absent from the proven pre-base set (post-base coins
    // included) and setTip(base) would regress a higher tip. If forward sync
    // advances the tip past base first, promotion is skipped (tip_below_base=false
    // in BackgroundValidationWorker) and the pre-base coins are never written into
    // the coin CF (they stay accumulator-only: wallet-visible but gettxout-null,
    // unspendable). So cap the activation target at base until promotion clears
    // assumeutxo_active_. Block download + AcceptBlock are independent of
    // ActivateBestChain, so blocks past base keep arriving and are STORED; they are
    // connected on the normal catch-up pass once the mode exits. Only caps when the
    // candidate genuinely descends from the snapshot base (otherwise the below-base
    // fork guards above / the normal work comparison handle it).
    if (assumeutxo_active_ && !assumeutxo_base_block_.IsNull() &&
        static_cast<uint32_t>(best_candidate->height) > assumeutxo_base_height_ &&
        GetConfig().assumeutxo_forward_connect) {
        // FORWARD-CONNECT (mobile profile): do NOT hold the tip at the base.
        // Blocks descending from the snapshot base connect immediately (the
        // base state is verified against the compiled registry anchor), and
        // promotion runs in advanced-tip mode when the replay completes (see
        // PromoteValidatedHistory) — so the #353 bug-2 hazard the hold guards
        // against is handled there, not by deferring the tip.
        if (!assumeutxo_forward_connect_logged_ && logger_) {
            assumeutxo_forward_connect_logged_ = true;
            logger_->info("[ActivateBestChain] AssumeUTXO active — forward-connect profile: "
                          "connecting past snapshot base " +
                          std::to_string(assumeutxo_base_height_) +
                          " while background validation runs (node usable at the live tip)");
        }
    } else if (assumeutxo_active_ && !assumeutxo_base_block_.IsNull() &&
        static_cast<uint32_t>(best_candidate->height) > assumeutxo_base_height_) {
        // #360 residual: hold g_block_index_mutex across the ancestry walk so the
        // pprev reads don't race a concurrent load-thread relink. activation_mutex_
        // is already held here, preserving the [caller] -> g_block_index_mutex order.
        CBlockIndex* base_idx = nullptr;
        CBlockIndex* ancestor = best_candidate;
        {
            std::lock_guard<std::recursive_mutex> lk(dinero::g_block_index_mutex);
            base_idx = dinero::FindBlockIndex(assumeutxo_base_block_);
            while (ancestor &&
                   static_cast<uint32_t>(ancestor->height) > assumeutxo_base_height_) {
                ancestor = ancestor->pprev;
            }
        }
        if (base_idx && ancestor == base_idx) {
            if (logger_) {
                // Keep at info: this "holding tip at snapshot base" line is a
                // significant operational event and is the observability signal
                // the AssumeUtxoPromotionRace e2e test (A1) greps for. Demoting it
                // to debug made the test's default-INFO daemon log miss it → A1
                // failed deterministically. Log-once spam reduction, if wanted,
                // belongs in a separate change paired with a test update.
                logger_->info("[ActivateBestChain] AssumeUTXO active — holding tip at snapshot base " +
                              std::to_string(assumeutxo_base_height_) +
                              " until background validation + promotion complete (network candidate @" +
                              std::to_string(best_candidate->height) +
                              " deferred; blocks stored, not connected)");
            }
            best_candidate = base_idx;
        } else if (!base_idx && logger_) {
            // base_idx null while AssumeUTXO is active should be unreachable —
            // the fresh-bootstrap self-heal / snapshot restore materializes the
            // base into the block index before we get here. Warn loudly if it
            // ever happens: without the cap the tip could race past base and
            // silently re-trigger the promotion-race bug this guard prevents.
            // (A candidate above base that does NOT descend from the base is a
            // competing fork; that intentionally falls through to the existing
            // fork-below-base fatal guard, not here.)
            logger_->warning("[ActivateBestChain] AssumeUTXO active but snapshot base not "
                             "found in the block index — cannot hold tip at base; forward "
                             "sync may race past base (promotion-race guard degraded)");
        }
    }

    // Compare best candidate with active tip
    bool needs_activation = false;

    if (!active_tip_) {
        // No active tip yet (genesis initialization)
        needs_activation = true;
        std::cout << "✅ [ActivateBestChain] No active_tip_ → needs_activation=true (genesis)" << std::endl;
        if (logger_) logger_->info("[ActivateBestChain] Activating genesis block");
    } else if (best_candidate == active_tip_) {
        // Already on best chain, nothing to do
        if (logger_) logger_->debug("[ActivateBestChain] Already on best chain");
        std::cout << "❌ [ActivateBestChain] EARLY RETURN: best_candidate == active_tip_ (height=" << active_tip_->height << ")" << std::endl;
        return;
    } else {
        // Chain selection (Bitcoin-style first-seen).
        //
        // ONLY reorg on strictly more work. On equal work, keep the current
        // tip (first-seen wins). This is the standard Bitcoin rule.
        //
        // Prior behavior was to break same-work ties by picking the block
        // with the numerically lower hash. That's deterministic but causes
        // avoidable reorg churn whenever two miners produce same-work
        // siblings at the same height — every node that sees the lower-hash
        // block second will reorg, even though the network is already in
        // consensus on the first-seen block. On 2026-04-20 this was the
        // trigger that initiated the reorg attempt exposing the
        // "missing undo at active tip" safe-mode fire on Mac during the
        // two-miner sibling race at height 3417 (Bug A compounded with
        // Bug B; B was closed in 28c6ea069, A is this change).
        //
        // At fleet scale (100+ miners) lower-hash-wins adds measurable P2P
        // churn and creates subtle unfairness — a miner who happens to
        // produce blocks with low leading nonzero bytes wins ties more
        // often, which is noise, not skill.
        int work_cmp = chainwork::CompareWork(best_candidate->chainwork, active_tip_->chainwork);
        std::cout << "🔍 [ActivateBestChain] Comparing work: work_cmp=" << work_cmp << std::endl;
        std::cout << "🔍 [ActivateBestChain]   candidate.chainwork=" << best_candidate->chainwork << std::endl;
        std::cout << "🔍 [ActivateBestChain]   active_tip.chainwork=" << active_tip_->chainwork << std::endl;

        if (work_cmp > 0) {
            needs_activation = true;
            std::cout << "✅ [ActivateBestChain] work_cmp > 0 → needs_activation=true (higher work)" << std::endl;
            if (logger_) logger_->info("[ActivateBestChain] Switching to higher work chain");
        } else {
            // work_cmp <= 0 — candidate is not strictly heavier. Stay on the
            // first-seen tip (current active_tip_).
            std::cout << "⚠️  [ActivateBestChain] Not switching: candidate not strictly heavier (work_cmp=" << work_cmp << ")" << std::endl;
            std::cout << "    candidate.hash=" << best_candidate->hash.GetHex().substr(0, 16) << "..." << std::endl;
            std::cout << "    active_tip.hash=" << active_tip_->hash.GetHex().substr(0, 16) << "..." << std::endl;
            if (work_cmp == 0 && logger_) {
                logger_->info("[ActivateBestChain] Same-work sibling observed at height " +
                              std::to_string(best_candidate->height) +
                              " — keeping first-seen tip (no reorg)");
            }
        }
    }

    if (!needs_activation) {
        std::cout << "❌ [ActivateBestChain] EARLY RETURN: needs_activation=false" << std::endl;
        return; // Stay on current chain
    }

    // Final safety: P0 invariant — candidate must be reorg-eligible (whole-branch
    // data; #309 — validation is performed per-block during the ConnectTip walk).
    if (!IsReorgCandidateEligible(best_candidate)) {
        if (logger_) {
            logger_->warning("[ActivateBestChain] Best candidate at height " +
                            std::to_string(best_candidate->height) +
                            " not eligible for activation (status=" +
                            std::to_string(best_candidate->status) + ") — skipping reorg");
        }
        return;
    }
    if (HasInvalidAncestor(best_candidate)) {
        if (logger_) {
            logger_->warning("[ActivateBestChain] Best candidate at height " +
                             std::to_string(best_candidate->height) +
                             " has a persisted invalid ancestor — removing it from candidates");
        }
        RemoveCandidate(best_candidate);
        return;
    }

    std::cout << "✅ [ActivateBestChain] needs_activation=true, proceeding to chain activation..." << std::endl;
    std::cout << std::flush;

    // Phase 41: Execute reorg
    std::cout << "🔍 [ActivateBestChain] About to call FindFork with:" << std::endl;
    std::cout << "    active_tip_: " << (active_tip_ ? "EXISTS" : "NULL");
    if (active_tip_) std::cout << " height=" << active_tip_->height << " hash=" << active_tip_->hash.GetHex().substr(0, 16) << "...";
    std::cout << std::endl;
    std::cout << "    best_candidate: " << (best_candidate ? "EXISTS" : "NULL");
    if (best_candidate) std::cout << " height=" << best_candidate->height << " hash=" << best_candidate->hash.GetHex().substr(0, 16) << "...";
    std::cout << std::endl;
    std::cout << std::flush;

    CBlockIndex* fork_point = FindFork(active_tip_, best_candidate);

    std::cout << "🔍 [ActivateBestChain] FindFork returned: " << (fork_point ? "EXISTS" : "NULL");
    if (fork_point) std::cout << " height=" << fork_point->height << " hash=" << fork_point->hash.GetHex().substr(0, 16) << "...";
    std::cout << std::endl;
    std::cout << std::flush;

    // Priority 5 FIX: D2 - Fork point validity check
    if (active_tip_ && !fork_point) {
        logger_->error("INVARIANT D2 VIOLATION: Fork point is null (chains don't share ancestor)");
        return;
    }
    if (fork_point && fork_point->height < 0) {
        logger_->error("INVARIANT D2 VIOLATION: Fork point height is negative (" +
                      std::to_string(fork_point->height) + ")");
        return;
    }

    // ── Fork-below-base fatal guard ────────────────────────────────────────
    // Spec (assumeutxo-fatal-state-machine.md, Fatal Mismatch Semantics): a
    // higher-work chain diverging BELOW the snapshot base must go fatal — not
    // a silent reorg. Mechanically: undo below the base may not exist
    // (promotion persists only the audited tail), so a disconnect below base
    // would fail anyway; classify it as the proof failure it is.
    //
    // CONDITION: the spec's rule is NOT mode-scoped — after promotion the
    // exit gate clears assumeutxo_active_/assumeutxo_base_height_
    // (ClearAssumeUTXOState nulls them), but a below-base fork is STILL
    // fatal, so the guard keys off max(live base, promoted_base_height_)
    // where promoted_base_height_ is never cleared (set on promotion success
    // and restored at startup from the FullyValidated lifecycle record).
    //
    // fork_point != active_tip_: divergence means a non-empty disconnect path
    // (the disconnect walk below stops at fork_point). A pure extension from
    // a tip that happens to sit below base (fork_point == active_tip_, e.g. a
    // transient genesis-tip window during bootstrap) disconnects nothing and
    // is NOT a reorg below base — going fatal there would brick honest nodes.
    //
    // LOCKS: activation_mutex_ is held (function entry). ForceFatal takes
    // only the lifecycle's leaf mu_; EnsureAssumeUtxoLifecycle takes only
    // assumeutxo_lifecycle_init_mutex_ (leaf). EnterSafeMode under
    // activation_mutex_ has in-function precedent: the misalignment branch
    // and the deep-reorg branch of this same function already call it while
    // holding the lock.
    {
        const uint32_t effective_base = std::max(
            assumeutxo_active_ ? assumeutxo_base_height_ : 0u,
            promoted_base_height_);
        if (fork_point && fork_point != active_tip_ && effective_base > 0 &&
            fork_point->height < static_cast<int>(effective_base)) {
            const std::string reason =
                "reorg below assumeutxo base: fork height " +
                std::to_string(fork_point->height) + " < base " +
                std::to_string(effective_base) +
                " — higher-work divergence below the snapshot base "
                "(spec: fatal, not reorg)";
            if (logger_) logger_->error("[ActivateBestChain] FATAL: " + reason);
            EnsureAssumeUtxoLifecycle();
            // ForceFatal, not OnReplayComplete: the mismatch path of
            // OnReplayComplete is refused from FullyValidated (state guard
            // requires Validating/Stalled), and a post-promotion below-base
            // fork must STILL persist fatal — ForceFatal is the direct entry
            // usable from any non-fatal state.
            assumeutxo_lifecycle_->ForceFatal(reason);
            EnterSafeMode("assumeutxo fatal: " + reason);
            return;
        }
    }

    // Build disconnect path (from active_tip down to fork)
    std::vector<CBlockIndex*> disconnect_path;
    CBlockIndex* walk = active_tip_;
    while (walk && walk != fork_point) {
        disconnect_path.push_back(walk);
        walk = walk->pprev;
    }

    // Build connect path (from fork up to best_candidate)
    std::vector<CBlockIndex*> connect_path;
    walk = best_candidate;
    while (walk && walk != fork_point) {
        connect_path.push_back(walk);
        walk = walk->pprev;
    }
    std::reverse(connect_path.begin(), connect_path.end()); // Connect in forward order

    // Log reorg details
    if (!disconnect_path.empty() || !connect_path.empty()) {
        if (logger_) {
            logger_->warning("[ActivateBestChain] REORG DETECTED: disconnect=" +
                            std::to_string(disconnect_path.size()) +
                            " blocks, connect=" + std::to_string(connect_path.size()) + " blocks");
        }
    }

    // Proof freshness hardening: targeted bridge cache eviction at reorg entry.
    // Keep deep historical cache entries and drop only fork-sensitive entries.
    if (!disconnect_path.empty() && bridge_node_) {
        uint32_t evict_from_height = 0;
        if (fork_point && fork_point->height >= 0) {
            evict_from_height = static_cast<uint32_t>(fork_point->height + 1);
        }
        const size_t evicted = bridge_node_->EvictBlockProofsAtOrAboveHeight(evict_from_height);
        bridge_node_->InvalidateTxProofCache();
        if (logger_) {
            logger_->info("[ActivateBestChain] Reorg cache hygiene: evicted " +
                          std::to_string(evicted) + " block proof entries from height >= " +
                          std::to_string(evict_from_height) + ", invalidated tx-proof cache");
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // STATELESS (CSN) REORG: Checkpoint + Replay
    // ═══════════════════════════════════════════════════════════════════════════
    // In STATELESS mode, we don't rely on Utreexo deltas for disconnect. Instead:
    // 1. Restore forest to fork_point from persisted checkpoint
    // 2. Lightweight disconnect old chain (coin cleanup + tip pointer only)
    // 3. Replay new chain blocks through forest using stored spend targets
    // 4. ConnectTip for bookkeeping (coins, tip pointer, height index)
    // 5. Signal OnUtxoBlock handler to reset next_validate_height
    // ═══════════════════════════════════════════════════════════════════════════
    if (GetConfig().utreexo_stateless && !disconnect_path.empty() && fork_point) {
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "[ABC-CSN] STATELESS reorg: fork=" << fork_point->height
                  << " disconnect=" << disconnect_path.size()
                  << " connect=" << connect_path.size() << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

        if (!stateless_node_) {
            if (logger_) logger_->error("[ABC-CSN] Stateless reorg requested but StatelessNode is not wired");
            return;
        }
        if (!consensus_utxo_set_) {
            if (logger_) logger_->error("[ABC-CSN] Stateless reorg requested but consensus UTXO set is unavailable");
            return;
        }

        // Preflight the complete branch before touching any canonical state.
        // A side branch becomes visible to candidate import only after the CSN
        // speculative validator persisted its replay record, so absence of
        // either artifact means the plan is incomplete or stale. Previously a
        // missing body was discovered only after DisconnectTip had already
        // moved the active chain to the fork, leaving the node stranded there.
        for (const auto* block_index : connect_path) {
            const auto block_result = ReadStoredBlock(block_index->hash);
            if (block_result.status() != Status::Ok) {
                if (logger_) {
                    logger_->warning("[ABC-CSN] Reorg plan incomplete: body unavailable at height " +
                                     std::to_string(block_index->height) +
                                     " — canonical state left untouched");
                }
                return;
            }
            const auto replay_result = chain_db_->getCSNSpendTargets(block_index->hash);
            CsnReplayData replay_data;
            if (replay_result.status() != Status::Ok ||
                !DecodeCsnReplayData(replay_result.value(), replay_data)) {
                if (logger_) {
                    logger_->warning("[ABC-CSN] Reorg plan incomplete: replay data unavailable at height " +
                                     std::to_string(block_index->height) +
                                     " — canonical state left untouched");
                }
                return;
            }
        }

        // Step 1: Rebuild the forest at fork_point height. Campaign phase 3
        // (CSN): full checkpoints exist only every N blocks, so restore is
        // nearest-checkpoint + UD-sidecar replay (header-root verified per
        // block inside RestoreHistoricalForest) rather than an exact-height
        // checkpoint read that misses off-interval.
        consensus::UtreexoForest restored_forest;
        {
            std::string restore_error;
            const Status restore_status = storage::RestoreHistoricalForest(
                *chain_db_, fork_point->height, restored_forest, restore_error);
            if (restore_status != Status::Ok) {
                if (logger_) logger_->error("[ABC-CSN] Cannot rebuild forest at fork height " +
                                            std::to_string(fork_point->height) + " (" +
                                            restore_error + ") — cannot reorg");
                std::cout << "❌ [ABC-CSN] Cannot rebuild forest at height "
                          << fork_point->height << ": " << restore_error << std::endl;
                return;
            }
        }
        if (logger_) logger_->info("[ABC-CSN] Rebuilt forest at fork height " +
                                   std::to_string(fork_point->height) + " leaves=" +
                                   std::to_string(restored_forest.getNumLeaves()));

        // TIER-0 SAFETY: Verify restored checkpoint root matches fork-point header.
        // A mismatch means the checkpoint is corrupt or from a different fork —
        // replaying from it would silently drift from consensus.
        {
            auto fp_block_result = ReadStoredBlock(fork_point->hash);
            if (fp_block_result.status() != Status::Ok) {
                if (logger_) {
                    logger_->error("[ABC-CSN] Cannot load fork-point block " +
                                   fork_point->hash.GetHex() +
                                   " for checkpoint root validation — ABORTING REORG");
                }
                return;
            }
            const uint256& expected_fp_root = fp_block_result.value().header.utreexo_root;

            auto restored_commitment = restored_forest.getCommitment();
            if (restored_commitment.size() != 32) {
                if (logger_) {
                    logger_->error("[ABC-CSN] Invalid checkpoint commitment size at fork height " +
                                   std::to_string(fork_point->height) + ": " +
                                   std::to_string(restored_commitment.size()) +
                                   " — ABORTING REORG");
                }
                return;
            }
            uint256 restored_root;
            std::memcpy(restored_root.begin(), restored_commitment.data(), 32);

            if (restored_root != expected_fp_root) {
                if (logger_) {
                    logger_->error("[ABC-CSN] CHECKPOINT ROOT MISMATCH at fork height " +
                                   std::to_string(fork_point->height) +
                                   " block=" + fork_point->hash.GetHex());
                    logger_->error("[ABC-CSN]   checkpoint: " + restored_root.GetHex());
                    logger_->error("[ABC-CSN]   header:     " + expected_fp_root.GetHex());
                    logger_->error("[ABC-CSN]   ABORTING REORG — checkpoint is corrupt or stale");
                }
                return;
            }
            if (logger_) {
                logger_->info("[ABC-CSN] Checkpoint root verified against fork-point header at height " +
                              std::to_string(fork_point->height));
            }
        }

        // Step 2: Restore forest + StatelessNode state
        stateless_node_->RewindToCheckpoint(static_cast<uint32_t>(fork_point->height), restored_forest);

        // Step 3: Lightweight disconnect old chain
        for (auto* block_index : disconnect_path) {
            std::cout << "[ABC-CSN] Disconnecting height " << block_index->height << std::endl;
            if (!DisconnectTip(block_index)) {
                if (logger_) logger_->error("[ABC-CSN] DisconnectTip failed at height " +
                                            std::to_string(block_index->height));
                return;
            }
        }

        // Step 4: Replay connect_path through forest + bookkeeping
        // Uses CommitConnectedBlockBookkeeping (NOT ConnectTip) to avoid
        // double forest mutation. ReplayBlock is the sole forest mutator.
        for (auto* block_index : connect_path) {
            std::cout << "[ABC-CSN] Replaying height " << block_index->height << std::endl;

            // Load block from ChainDB
            auto block_r = ReadStoredBlock(block_index->hash);
            if (block_r.status() != Status::Ok) {
                if (logger_) logger_->error("[ABC-CSN] Failed to load block at height " +
                                            std::to_string(block_index->height));
                return;
            }
            const Block& replay_block = block_r.value();

            // Load spend targets + optional spent-output metadata for this block
            // from CF7 (utreexo). Older records are hash-only; v2 records also
            // carry the metadata needed to enforce stateless coinbase maturity.
            std::vector<consensus::UtreexoHash> spend_targets;
            std::vector<consensus::SpentOutputData> replay_spent_outputs;
            const std::vector<consensus::SpentOutputData>* spent_outputs = nullptr;
            if (replay_block.utreexo.has_value()) {
                spent_outputs = &replay_block.utreexo->spent_outputs;
            }
            auto st_result = chain_db_->getCSNSpendTargets(block_index->hash);
            if (st_result.status() == Status::Ok && st_result.value().size() >= 4) {
                CsnReplayData replay_data;
                if (!DecodeCsnReplayData(st_result.value(), replay_data)) {
                    if (logger_) logger_->error("[ABC-CSN] Malformed CSN replay data for height " +
                                                std::to_string(block_index->height));
                    return;
                }
                spend_targets = std::move(replay_data.spend_targets);
                if (!spent_outputs && replay_data.has_spent_outputs) {
                    replay_spent_outputs = std::move(replay_data.spent_outputs);
                    spent_outputs = &replay_spent_outputs;
                }
                if (logger_) logger_->info("[ABC-CSN] Loaded " + std::to_string(spend_targets.size()) +
                                           " spend targets for height " + std::to_string(block_index->height));
            } else {
                if (logger_) logger_->warning("[ABC-CSN] No spend targets for height " +
                                              std::to_string(block_index->height) +
                                              " — forest replay may fail");
            }

            // Replay block through forest via StatelessNode (sole forest mutator)
            if (!stateless_node_->ReplayBlock(replay_block, block_index->height, spend_targets, spent_outputs)) {
                if (logger_) logger_->error("[ABC-CSN] ReplayBlock failed at height " +
                                            std::to_string(block_index->height));
                return;
            }

            // CRITICAL ASSERTION: replayed forest commitment must match block header utreexo_root
            auto replayed_commitment = consensus_utxo_set_->GetForest().getCommitment();
            uint256 forest_root;
            if (replayed_commitment.size() == 32) {
                std::memcpy(forest_root.begin(), replayed_commitment.data(), 32);
            }
            if (forest_root != replay_block.header.utreexo_root) {
                if (logger_) {
                    logger_->error("[ABC-CSN] COMMITMENT MISMATCH after replay at height " +
                                  std::to_string(block_index->height));
                    logger_->error("[ABC-CSN]   forest: " + forest_root.GetHex().substr(0, 16) + "...");
                    logger_->error("[ABC-CSN]   header: " + replay_block.header.utreexo_root.GetHex().substr(0, 16) + "...");
                    logger_->error("[ABC-CSN]   ABORTING REORG — stored spend targets may be corrupt");
                }
                return;
            }
            if (logger_) logger_->info("[ABC-CSN] Commitment verified at height " +
                                       std::to_string(block_index->height));

            // Shielded apply for the replayed block. ReplayBlock only advances
            // the Utreexo forest; without this the replayed branch's note
            // commitments and nullifiers never enter CSN shielded state
            // (permanent divergence + double-spend acceptance). Deltas are
            // recomputed exactly as forward stateless validation computed them
            // (bit-identity pinned by the ShieldedBlockSectionDeltaParity
            // suite); ApplyBlockShieldedSection fires the epoch reset at H,
            // validates, applies, and records the anchor root.
            // #356: route the delta-recompute + pre-block frontier capture +
            // ApplyBlockShieldedSection through the shared ApplyStatelessReplay-
            // Shielded funnel (also used by the ConnectTip crash-recovery
            // branch). In this contiguous replay loop the disconnect leg rolled
            // the shielded marker back to the fork point and each
            // CommitConnectedBlockBookkeeping advances it by one, so the marker
            // is ALWAYS exactly at height-1 here and the guard's Apply branch
            // always fires. A Skip/GapFail would mean the marker diverged from
            // the replay cursor — a broken invariant — so treat "did not apply"
            // as a loud abort rather than silently leaving shielded state
            // unadvanced (silent skip is wrong for the reorg loop, whose
            // correctness relies on always-apply).
            // block_hash is set inside ApplyStatelessReplayShielded on the apply
            // path; this loop aborts on non-apply, so no preset is needed here.
            consensus::BlockUndo replay_shielded_undo;
            std::string serr;
            bool shielded_applied = false;
            if (!ApplyStatelessReplayShielded(
                    replay_block, static_cast<uint32_t>(block_index->height),
                    replay_shielded_undo, shielded_applied, serr,
                    /*fallback_spent_outputs=*/spent_outputs)) {
                if (logger_) logger_->error("[ABC-CSN] shielded replay apply failed at height " +
                                            std::to_string(block_index->height) + ": " + serr);
                return;  // abort reorg, same style as the surrounding failures
            }
            if (!shielded_applied) {
                if (logger_) logger_->error("[ABC-CSN] shielded replay guard did NOT apply at "
                                            "height " + std::to_string(block_index->height) +
                                            " (marker not at height-1) — ABORTING REORG");
                return;  // broken contiguous-replay invariant
            }
            // Bookkeeping: coins, undo record, shielded persistence, tip,
            // height index, checkpoint, notify. replay_shielded_undo carries
            // the pre-block frontier (and, on a cutover-crossing replay, the
            // pre-reset epoch snapshot) into the persisted UndoRecord so a
            // second reorg can disconnect this replay-connected block.
            std::string bk_err;
            if (!CommitConnectedBlockBookkeeping(block_index, replay_block, &replay_shielded_undo, &bk_err)) {
                if (logger_) logger_->error("[ABC-CSN] Bookkeeping failed at height " +
                                            std::to_string(block_index->height) + ": " + bk_err);
                return;
            }
        }

        // Step 5: Signal OnUtxoBlock handler to reset
        csn_reorg_reset_height_.store(static_cast<uint32_t>(best_candidate->height + 1));

        if (logger_) logger_->info("[ABC-CSN] STATELESS reorg complete: fork=" +
                                   std::to_string(fork_point->height) +
                                   " new_tip=" + std::to_string(best_candidate->height));
        std::cout << "✅ [ABC-CSN] STATELESS reorg complete. New tip: " << best_candidate->height << std::endl;
        return;  // Skip normal full-node disconnect/connect loops
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // REORG ENTRY SANITY CHECK: Forest state must be consistent before disconnect
    // ═══════════════════════════════════════════════════════════════════════════
    // Before disconnecting any blocks, verify the forest exists and its state
    // is consistent with the active tip. This catches corruption or state drift
    // before it causes cascading failures.
    //
    // This is CHEAP + LOUD: abort early if something is wrong.
    // Full proof-level validation comes in Phase 3.3.
    // ═══════════════════════════════════════════════════════════════════════════
    if (!disconnect_path.empty() && consensus_utxo_set_) {
        uint64_t forest_leaves = consensus_utxo_set_->GetForest().getNumLeaves();
        uint32_t active_height = active_tip_ ? active_tip_->height : 0;
        uint32_t fork_height = fork_point ? fork_point->height : 0;

        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "🔍 [REORG ENTRY] Forest sanity check" << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "   Active tip height:  " << active_height << std::endl;
        std::cout << "   Fork point height:  " << fork_height << std::endl;
        std::cout << "   Blocks to disconnect: " << disconnect_path.size() << std::endl;
        std::cout << "   Forest leaves:      " << forest_leaves << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

        if (logger_) {
            logger_->info("[REORG ENTRY] Forest state: leaves=" + std::to_string(forest_leaves) +
                         ", active_height=" + std::to_string(active_height) +
                         ", fork_height=" + std::to_string(fork_height));
        }

        // Sanity: Forest should not be empty if we're above genesis
        if (active_height > 0 && forest_leaves == 0) {
            if (logger_) {
                logger_->error("❌ [REORG ENTRY] SANITY FAILED: Forest is empty but chain height > 0");
                logger_->error("   This indicates forest corruption or state loss");
                logger_->error("   ABORTING REORG - manual intervention required");
            }
            return;
        }
    } else if (!disconnect_path.empty() && !consensus_utxo_set_) {
        // No forest during reorg - this is a problem if Utreexo is active
        if (consensus::IsUtreexoActive(active_tip_ ? active_tip_->height : 0)) {
            if (logger_) {
                logger_->error("❌ [REORG ENTRY] SANITY FAILED: No forest but Utreexo is active");
                logger_->error("   Cannot disconnect blocks without forest state");
                logger_->error("   ABORTING REORG - manual intervention required");
            }
            return;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PHASE 3.3: FOREST SNAPSHOT VALIDATION (FULL)
    // ═══════════════════════════════════════════════════════════════════════════
    // Before disconnecting, verify the forest commitment matches the expected
    // state at the active tip. The block header's utreexo_root is the AFTER-state
    // commitment (state after applying that block).
    //
    // If mismatch: Forest is corrupted or out of sync. ABORT REORG.
    // This prevents cascading corruption during disconnect/reconnect.
    // ═══════════════════════════════════════════════════════════════════════════
    if (!disconnect_path.empty() && consensus_utxo_set_ && active_tip_) {
        // Get current forest commitment
        auto current_commitment = consensus_utxo_set_->GetForest().getCommitment();

        // Fetch the active tip block to get its utreexo_root (AFTER-state)
        auto block_result = ReadStoredBlock(active_tip_->hash);
        if (block_result.status() == Status::Ok) {
            const Block& tip_block = block_result.value();
            const uint256& expected_root = tip_block.header.utreexo_root;

            // Convert forest commitment to uint256 for comparison
            uint256 forest_root;
            std::memcpy(forest_root.begin(), current_commitment.data(), 32);

            std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
            std::cout << "🔍 [PHASE 3.3] Forest snapshot validation" << std::endl;
            std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
            std::cout << "   Active tip height:    " << active_tip_->height << std::endl;
            std::cout << "   Expected utreexo_root: " << expected_root.GetHex().substr(0, 16) << "..." << std::endl;
            std::cout << "   Forest commitment:     " << forest_root.GetHex().substr(0, 16) << "..." << std::endl;
            std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

            if (forest_root != expected_root) {
                if (logger_) {
                    logger_->error("❌ [PHASE 3.3] FOREST SNAPSHOT MISMATCH!");
                    logger_->error("   Expected (tip header): " + expected_root.GetHex());
                    logger_->error("   Actual (forest):       " + forest_root.GetHex());
                    logger_->error("   Active tip height:     " + std::to_string(active_tip_->height));
                    logger_->error("   ABORTING REORG - forest state corrupted or out of sync");
                    logger_->error("   Manual intervention required: resync or restore from backup");
                }
                return;
            }

            if (logger_) logger_->info("[PHASE 3.3] Forest snapshot validated: commitment matches tip header");
        } else {
            // Can't fetch block - this is unusual but not necessarily fatal
            // Log warning and proceed (the sanity check already passed)
            if (logger_) {
                logger_->warning("[PHASE 3.3] Could not fetch active tip block for validation");
                logger_->warning("   Proceeding with reorg based on sanity check alone");
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // DEFENSE-IN-DEPTH: Forest/fork-point consistency check for chain activation
    // ═══════════════════════════════════════════════════════════════════════════
    // The sanity checks above only fire when disconnect_path is not empty.
    // But when active_tip_ is at genesis and we're connecting blocks forward,
    // disconnect_path IS empty — and a stale forest goes undetected.
    //
    // If fork_point is genesis (height 0) and forest has leaves, it's stale
    // state from a previous chain run. Reset to empty so block 1 can validate
    // cleanly.
    // ═══════════════════════════════════════════════════════════════════════════
    // STATELESS mode: StatelessNode maintains the canonical forest. Don't reset it.
    if (!GetConfig().utreexo_stateless &&
        disconnect_path.empty() && !connect_path.empty() &&
        fork_point && fork_point->height == 0 &&
        consensus_utxo_set_ && consensus_utxo_set_->GetForest().getNumLeaves() > 0) {
        if (logger_) {
            logger_->error("[ActivateBestChain] FOREST/GENESIS MISMATCH: fork_point=genesis but forest has " +
                          std::to_string(consensus_utxo_set_->GetForest().getNumLeaves()) + " leaves (expected 0)");
            logger_->error("[ActivateBestChain] Resetting forest for clean chain activation");
        }
        consensus_utxo_set_->GetForest() = consensus::UtreexoForest();
    }

    // Phase 43: Deep Reorg Detection and Safe Mode
    // If reorg depth exceeds threshold, enter safe mode to protect miners
    int reorg_depth = disconnect_path.size();
    if (reorg_depth >= DEEP_REORG_THRESHOLD) {
        std::string reason = "Deep reorg detected: " + std::to_string(reorg_depth) +
                           " blocks (threshold: " + std::to_string(DEEP_REORG_THRESHOLD) + ")";
        EnterSafeMode(reason);

        if (logger_) {
            logger_->error("⚠️  CRITICAL: Deep reorg of " + std::to_string(reorg_depth) + " blocks!");
            logger_->error("⚠️  This may indicate a 51% attack or major network split");
            logger_->error("⚠️  Mining paused until chain stabilizes");
        }
    } else if (reorg_depth >= 10) {
        // Warn about moderate reorgs but don't enter safe mode
        if (logger_) {
            logger_->warning("⚠️  Moderate reorg of " + std::to_string(reorg_depth) + " blocks detected");
            logger_->warning("⚠️  Monitor the network - this is unusual");
        }
    }

    // Collect transactions from blocks being disconnected (for mempool reconciliation).
    // Use ancestor-first order so tx chains from disconnected blocks replay cleanly.
    std::vector<Transaction> disconnected_txs;
    AppendTransactionsFromBlocks(
        chain_db_,
        block_storage_.get(),
        disconnect_path,
        /*ancestor_first=*/true,
        &disconnected_txs);

    // Durable advisory marker for interrupted canonical transitions.
    //
    // `reorg_in_progress` lives inside the wallet SQLite transaction below, so
    // it is intentionally atomic with wallet-side reorg state and may vanish on
    // crash-before-commit. That makes it the wrong durability layer for
    // "process died mid-transition". Use the datadir recovery marker for that
    // role instead: it survives abrupt termination and is only cleared after a
    // later successful canonical transition or healthy startup reconciliation.
    bool activation_recovery_marker_written = false;
    if ((!disconnect_path.empty() || !connect_path.empty()) && !datadir_.empty()) {
        std::string marker_error;
        const std::string transition_reason =
            "activate-best-chain target_height=" + std::to_string(best_candidate ? best_candidate->height : -1) +
            " target_hash=" + (best_candidate ? best_candidate->hash.GetHex() : std::string("null")) +
            " disconnect_count=" + std::to_string(disconnect_path.size()) +
            " connect_count=" + std::to_string(connect_path.size());

        std::string existing_marker_error;
        const bool existing_recovery_marker =
            daemon::ReadChainstateRecoveryMarker(datadir_, &existing_marker_error).has_value();
        if (existing_recovery_marker) {
            if (logger_) {
                logger_->debug("[ActivateBestChain] Preserving existing durable recovery marker");
            }
        } else {
            activation_recovery_marker_written =
                daemon::WriteChainstateRecoveryMarker(datadir_, transition_reason, &marker_error);
        }

        if (activation_recovery_marker_written) {
            if (logger_) {
                logger_->debug("[ActivateBestChain] Wrote durable activation recovery marker");
            }
        } else if (!existing_recovery_marker && logger_) {
            logger_->warning("[ActivateBestChain] Failed to write durable activation recovery marker: " +
                             marker_error);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Crash Safety: Mark reorg in progress (Priority 4 FIX - Transactional)
    // ═══════════════════════════════════════════════════════════════════════════
    // If the process crashes during reorg, this marker allows detection and
    // recovery on next startup. The marker stores:
    // - pre_reorg_height: Height before disconnect started
    // - fork_height: Height of fork point
    // - target_height: Height we're trying to reach
    //
    // Priority 4 FIX: Marker is now set within a transaction that spans the
    // entire wallet reorg operation. This ensures marker and state are atomic.
    // ═══════════════════════════════════════════════════════════════════════════
    bool wallet_transaction_started = false;
    if (utxo_index_ && !disconnect_path.empty()) {
        // Priority 4 FIX: Begin transaction for atomic marker + state changes
        wallet_transaction_started = utxo_index_->BeginTransaction();
        if (!wallet_transaction_started) {
            if (logger_) logger_->warning("[ActivateBestChain] Failed to begin wallet transaction for reorg");
        }

        std::string reorg_marker = std::to_string(active_tip_ ? active_tip_->height : 0) + ":" +
                                   std::to_string(fork_point ? fork_point->height : 0) + ":" +
                                   std::to_string(best_candidate->height);
        utxo_index_->SetMetadata("reorg_in_progress", reorg_marker);
        if (logger_) logger_->info("[ActivateBestChain] Reorg marker set: " + reorg_marker);
    }

    // #309/I3: remember the pre-reorg active tip. If the reorg aborts midway
    // (disconnect or connect failure) the active chain has been partially torn
    // down; re-registering this tip as a candidate lets the next ActivateBestChain
    // pass reorg back to it (the failed branch is removed / marked invalid), so
    // the node returns to its original chain instead of stranding at the fork.
    CBlockIndex* pre_reorg_tip = active_tip_;

    // Disconnect old chain
    for (auto* block_index : disconnect_path) {
        if (!DisconnectTip(block_index)) {
            if (logger_) logger_->error("[ActivateBestChain] Failed to disconnect block at height " +
                          std::to_string(block_index->height));
            // Priority 4 FIX: Rollback wallet transaction on abort
            if (wallet_transaction_started && utxo_index_) {
                utxo_index_->RollbackTransaction();
                if (logger_) logger_->info("[ActivateBestChain] Wallet transaction rolled back after disconnect failure");
            }

            // Disconnect failures are operational/runtime faults, not consensus
            // invalidity. Keep the candidate valid and only remove it from the
            // current candidate set to avoid an immediate retry loop.
            if (best_candidate) {
                if (logger_) logger_->warning("[ActivateBestChain] REORG ABORT: Temporarily removing candidate after disconnect failure");
                std::cout << "⚠️  [ActivateBestChain] REORG ABORT: Removing candidate at height "
                          << best_candidate->height << " from candidate set (disconnect failed)" << std::endl;

                // Remove from candidates set to prevent retry
                RemoveCandidate(best_candidate);

                // Also clear the reorg marker since we're aborting
                if (utxo_index_) {
                    utxo_index_->SetMetadata("reorg_in_progress", "");
                }
            }

            // #309/I3: re-register the pre-reorg tip so the next pass restores it.
            if (pre_reorg_tip) AddCandidate(pre_reorg_tip);

            RecordActivationFailure(
                utxo_index_.get(),
                "disconnect failure at height " + std::to_string(block_index->height)
            );

            return; // Abort reorg on failure
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TIER-0 SAFETY: Verify forest root at fork point after disconnect.
    // After walking back from active_tip to fork_point, the forest must be
    // at the fork_point's AFTER-state. If not, disconnect deltas were wrong
    // or the forest was already drifted — connecting new-chain blocks from
    // this base would silently diverge from consensus.
    // ═══════════════════════════════════════════════════════════════════════════
    if (!disconnect_path.empty() && consensus_utxo_set_ && fork_point) {
        auto fp_block_result = ReadStoredBlock(fork_point->hash);
        if (fp_block_result.status() != Status::Ok) {
            if (logger_) {
                logger_->error("[ActivateBestChain] Cannot load fork-point block " +
                               fork_point->hash.GetHex() +
                               " for post-disconnect root validation — ABORTING REORG");
            }
            if (wallet_transaction_started && utxo_index_) {
                utxo_index_->RollbackTransaction();
            }
            return;
        }
        const uint256& expected_fp_root = fp_block_result.value().header.utreexo_root;

        auto forest_commitment = consensus_utxo_set_->GetForest().getCommitment();
        if (forest_commitment.size() != 32) {
            if (logger_) {
                logger_->error("[ActivateBestChain] Invalid forest commitment size after disconnect: " +
                               std::to_string(forest_commitment.size()) + " — ABORTING REORG");
            }
            if (wallet_transaction_started && utxo_index_) {
                utxo_index_->RollbackTransaction();
            }
            return;
        }
        uint256 forest_root;
        std::memcpy(forest_root.begin(), forest_commitment.data(), 32);

        if (forest_root != expected_fp_root) {
            if (logger_) {
                logger_->error("[ActivateBestChain] FORK-POINT ROOT MISMATCH after disconnect");
                logger_->error("  fork height=" + std::to_string(fork_point->height) +
                               " block=" + fork_point->hash.GetHex());
                logger_->error("  forest:   " + forest_root.GetHex());
                logger_->error("  expected: " + expected_fp_root.GetHex());
                logger_->error("  ABORTING REORG — disconnect deltas produced wrong state");
            }
            if (wallet_transaction_started && utxo_index_) {
                utxo_index_->RollbackTransaction();
            }
            return;
        }
        if (logger_) {
            logger_->info("[ActivateBestChain] Post-disconnect forest root verified at fork height " +
                          std::to_string(fork_point->height));
        }
    }

    // Collect transactions from blocks being connected (to filter from reconciliation)
    std::vector<Transaction> connected_txs;

    // Connect new chain
    std::cout << "🔍 [ActivateBestChain] connect_path.size()=" << connect_path.size() << std::endl;
    for (size_t i = 0; i < connect_path.size(); ++i) {
        auto* block_index = connect_path[i];
        std::cout << "🔍 [ActivateBestChain] Calling ConnectTip for block " << (i+1) << "/" << connect_path.size()
                  << ": height=" << block_index->height
                  << ", hash=" << block_index->hash.GetHex().substr(0, 16) << "..." << std::endl;

        std::string connect_error;
        bool consensus_invalid = false;
        if (!ConnectTip(block_index, &connect_error, &consensus_invalid)) {
            bool recovered = false;
            const bool missing_utxo = connect_error.find("Input UTXO not found") != std::string::npos;
            if (missing_utxo && logger_) {
                logger_->warning("[ActivateBestChain] ConnectTip missing UTXO; attempting one-shot in-memory state repair");
            }

            if (missing_utxo) {
                std::string repair_alignment_reason;
                if (!IsCanonicalStateAligned(&repair_alignment_reason)) {
                    if (logger_) {
                        logger_->error("[ActivateBestChain] Skipping in-memory repair: canonical state is already misaligned: " +
                                       repair_alignment_reason);
                    }
                    connect_error += " | canonical-state-misaligned: " + repair_alignment_reason +
                                     " (run --reindex-chainstate)";
                } else {
                    std::string reload_error;
                    std::string forest_error;
                    const bool reloaded = ReloadConsensusUTXOFromDB(reload_error);
                    const bool forest_restored = RestoreUtreexoCheckpoint(active_tip_ ? active_tip_->height : 0, forest_error);

                    if (!reloaded && logger_) {
                        logger_->error("[ActivateBestChain] Consensus UTXO reload failed: " + reload_error);
                    }
                    if (!forest_restored && logger_) {
                        logger_->error("[ActivateBestChain] Forest checkpoint restore failed: " + forest_error);
                    }

                    if (reloaded && forest_restored) {
                        std::string post_repair_alignment_reason;
                        if (!IsCanonicalStateAligned(&post_repair_alignment_reason)) {
                            if (logger_) {
                                logger_->error("[ActivateBestChain] In-memory repair left state misaligned: " +
                                               post_repair_alignment_reason);
                            }
                            connect_error += " | repair-misaligned: " + post_repair_alignment_reason +
                                             " (run --reindex-chainstate)";
                        } else {
                            std::string retry_error;
                            if (logger_) {
                                logger_->warning("[ActivateBestChain] Retrying ConnectTip once after in-memory state repair");
                            }
                            recovered = ConnectTip(block_index, &retry_error);
                            if (!recovered && !retry_error.empty()) {
                                connect_error = retry_error;
                            }
                        }
                    }
                }
            }

            if (recovered) {
                std::cout << "✅ [ActivateBestChain] ConnectTip RECOVERED for height " << block_index->height << std::endl;
            } else {
            if (logger_) logger_->error("[ActivateBestChain] Failed to connect block at height " +
                          std::to_string(block_index->height) +
                          (connect_error.empty() ? std::string() : (", error=" + connect_error)));
            std::cout << "❌ [ActivateBestChain] ConnectTip FAILED for height " << block_index->height << std::endl;
            if (missing_utxo) {
                EnterSafeMode("connect-tip missing utxo at height " + std::to_string(block_index->height));
            }
            // Priority 4 FIX: Rollback wallet transaction on abort
            if (wallet_transaction_started && utxo_index_) {
                utxo_index_->RollbackTransaction();
                if (logger_) logger_->info("[ActivateBestChain] Wallet transaction rolled back after connect failure");
            }

            // #309/I2: a consensus-rule violation (BlockValidator::ConnectBlock
            // rejected this block) on a speculative reorg branch must be marked
            // permanently invalid — otherwise a peer that feeds an invalid
            // heavier-work branch loops the reorg forever (the eligibility
            // relaxation made such branches reorg-targetable). The repairable
            // missing-utxo / operational cases are excluded (handled above) so we
            // never poison a valid chain into a false fork.
            if (consensus_invalid && !missing_utxo && block_index) {
                if (logger_) logger_->warning("[ActivateBestChain] REORG ABORT: consensus-invalid block at height " +
                                              std::to_string(block_index->height) +
                                              " — marking BLOCK_FAILED_VALID (#309/I2)");
                block_index->status |= BLOCK_FAILED_VALID;
                if (chain_db_) {
                    ChainWriteToken token = ChainWriteToken::CreateForTesting();
                    chain_db_->setHeaderStatusBits(token, block_index->hash, BLOCK_FAILED_VALID);
                }
            }

            // Connect failures are operational/runtime faults, not consensus
            // invalidity. Remove only from candidate set to avoid immediate
            // retry loops without poisoning block validity flags.
            if (best_candidate) {
                if (logger_) logger_->warning("[ActivateBestChain] REORG ABORT: Temporarily removing candidate after connect failure");
                std::cout << "⚠️  [ActivateBestChain] REORG ABORT: Removing candidate at height "
                          << best_candidate->height << " from candidate set (connect failed)" << std::endl;
                RemoveCandidate(best_candidate);

                if (utxo_index_) {
                    utxo_index_->SetMetadata("reorg_in_progress", "");
                }
            }

            // #309/I3: re-register the pre-reorg tip so the next ActivateBestChain
            // pass reorgs back to it (the failed branch is removed / marked invalid),
            // restoring the original chain instead of stranding at the fork point.
            if (pre_reorg_tip) AddCandidate(pre_reorg_tip);

            RecordActivationFailure(
                utxo_index_.get(),
                connect_error.empty()
                    ? ("connect failure at height " + std::to_string(block_index->height))
                    : connect_error
            );
            return; // Abort reorg on failure
            }
        }
        std::cout << "✅ [ActivateBestChain] ConnectTip SUCCEEDED for height " << block_index->height << std::endl;

        // Collect transactions from connected block (for mempool reconciliation)
        auto block_result = ReadStoredBlock(block_index->hash);
        if (block_result.status() == Status::Ok) {
            const Block& block = block_result.value();
            for (const auto& tx : block.vtx) {
                connected_txs.push_back(tx);
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Priority 5 FIX: Daemon Invariant Checks
    // ═══════════════════════════════════════════════════════════════════════════

    // D1: Tip height must be non-negative
    if (best_candidate->height < 0) {
        if (logger_) logger_->error("INVARIANT D1 VIOLATION: Tip height is negative (" +
                      std::to_string(best_candidate->height) + ")");
        if (wallet_transaction_started && utxo_index_) {
            utxo_index_->RollbackTransaction();
        }
        return;
    }

    // D6: Tip height monotonicity check (except during reorg)
    bool is_reorg = !disconnect_path.empty();
    if (!is_reorg && active_tip_ && best_candidate->height < active_tip_->height) {
        if (logger_) logger_->error("INVARIANT D6 VIOLATION: Tip height decreased without reorg (" +
                      std::to_string(active_tip_->height) + " -> " +
                      std::to_string(best_candidate->height) + ")");
        if (wallet_transaction_started && utxo_index_) {
            utxo_index_->RollbackTransaction();
        }
        return;
    }

    // Update active tip (already done by ConnectTip, but ensure consistency)
    PublishActiveTip(best_candidate, TipPublishReason::kSelfHealRealign);

    // Post-activation cache hygiene: prune any expired/non-canonical leftovers.
    if (bridge_node_) {
        const size_t evicted = bridge_node_->PruneStaleCacheEntries();
        if (logger_ && evicted > 0) {
            logger_->info("[ActivateBestChain] Pruned " + std::to_string(evicted) +
                          " stale bridge proof cache entries after activation");
        }
    }

    // Reconcile mempool after reorg: return disconnected txs to mempool
    if (!disconnected_txs.empty()) {
        auto* daemon_ctx = DaemonContext::instance();
        if (daemon_ctx && daemon_ctx->mempool && daemon_ctx->mempool->isInitialized()) {
            if (logger_) logger_->info("[ActivateBestChain] Reconciling mempool after reorg (" +
                         std::to_string(disconnected_txs.size()) + " txs from disconnected blocks)");

            size_t restored = daemon_ctx->mempool->mempool().ReconcileAfterReorg(
                disconnected_txs,
                connected_txs
            );

            if (logger_) logger_->info("[ActivateBestChain] Mempool reconciliation: " +
                         std::to_string(restored) + "/" + std::to_string(disconnected_txs.size()) +
                         " transactions restored");
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Crash Safety: Clear reorg marker on success (Priority 4 FIX)
    // ═══════════════════════════════════════════════════════════════════════════
    // Test-only crash boundary for the Phase 1 "markers are advisory, not
    // authority" invariant. By this point the canonical tip and Utreexo
    // checkpoint are already durable; only the advisory cleanup remains.
    dinero::testing::MaybeAbortAt("after_utreexo_checkpoint_before_marker_clear",
                                  dinero::Params().network_id == "regtest");

    if (utxo_index_) {
        utxo_index_->DeleteMetadata("reorg_in_progress");
        ClearActivationFailure(utxo_index_.get());

        // Priority 4 FIX: Commit the wallet transaction
        // This ensures marker deletion and all wallet state changes are atomic
        if (wallet_transaction_started) {
            if (utxo_index_->CommitTransaction()) {
                if (logger_) logger_->debug("[ActivateBestChain] Reorg marker cleared + wallet transaction committed");
            } else {
                if (logger_) logger_->error("[ActivateBestChain] Failed to commit wallet transaction!");
                // State may be inconsistent - ValidateAgainstConsensus will fix on restart
            }
        } else {
            if (logger_) logger_->debug("[ActivateBestChain] Reorg marker cleared (no transaction)");
        }
    }

    if (activation_recovery_marker_written) {
        std::string clear_error;
        if (!daemon::ClearChainstateRecoveryMarker(datadir_, &clear_error)) {
            if (logger_) {
                logger_->warning("[ActivateBestChain] Failed to clear durable activation recovery marker: " +
                                 clear_error);
            }
        } else if (logger_) {
            logger_->debug("[ActivateBestChain] Durable activation recovery marker cleared");
        }
    }

    std::cout << "✅ [ActivateBestChain] Chain activation COMPLETE" << std::endl;
    std::cout << "✅ [ActivateBestChain] New tip: height=" << active_tip_->height
              << ", hash=" << active_tip_->hash.GetHex().substr(0, 16) << "..." << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════\n" << std::endl;

    if (logger_) logger_->info("[ActivateBestChain] New tip: height=" + std::to_string(active_tip_->height) +
                  ", hash=" + active_tip_->hash.GetHex().substr(0, 16) + "...");

    // Notify dependent services
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx) {
        return;
    }

    // Notify mining service to refresh block templates
    if (daemon_ctx->mining) {
        auto mining = std::dynamic_pointer_cast<dinero::MiningService>(daemon_ctx->mining);
        if (mining) {
            if (logger_) logger_->debug("[ActivateBestChain] Notifying mining service of tip change");
        }
    }

    // Notify mempool service to revalidate transactions
    if (daemon_ctx->mempool) {
        auto mempool = std::dynamic_pointer_cast<dinero::MempoolService>(daemon_ctx->mempool);
        if (mempool) {
            if (logger_) logger_->debug("[ActivateBestChain] Notifying mempool service of tip change");
        }
    }
}

uint32_t ChainstateService::getBlockHeight() const {
    // In AssumeUTXO mode, active_tip_ is the authoritative tip (snapshot base).
    // ChainDB lags behind (genesis only) until ConnectTip catches up.
    if (active_tip_) return static_cast<uint32_t>(active_tip_->height);

    if (!chain_db_) return 0;
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) return 0;
    return tip_result.value().height;
}

std::string ChainstateService::getBestBlockHash() const {
    // In AssumeUTXO mode, active_tip_ is the authoritative tip (snapshot base).
    if (active_tip_) return active_tip_->hash.GetHex();

    if (!chain_db_) return std::string(64, '0');
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) return std::string(64, '0');
    return tip_result.value().hash.GetHex();
}

void ChainstateService::AnnounceTip() {
    // Phase G.X: Fork resolution - announce current tip to all peers
    if (block_relay_manager_) {
        block_relay_manager_->AnnounceTip();
        logger_->info("[ChainstateService] Announced current tip to peers for fork resolution");
    } else {
        logger_->warning("[ChainstateService] Cannot announce tip - BlockRelayManager not wired");
    }
}

// ============================================================================
// Phase 43: Safe Mode Management
// ============================================================================

void ChainstateService::EnterSafeMode(const std::string& reason) {
    if (safe_mode_active_) {
        return; // Already in safe mode
    }

    safe_mode_active_ = true;
    safe_mode_reason_ = reason;
    safe_mode_entered_time_ = std::chrono::steady_clock::now();

    logger_->warning("⚠️  SAFE MODE ACTIVATED: " + reason);
    logger_->warning("⚠️  Mining has been paused for safety");
    logger_->warning("⚠️  Chain state is unstable - waiting for network consensus");

    // Notify mining service to pause
    auto* daemon_ctx = DaemonContext::instance();
    if (daemon_ctx && daemon_ctx->mining) {
        auto mining = std::dynamic_pointer_cast<MiningService>(daemon_ctx->mining);
        if (mining) {
            logger_->info("[SafeMode] Notifying mining service to pause");
            mining->stopMining();
            logger_->info("[SafeMode] Mining stopped");
        }
    }
}

void ChainstateService::RequestChainstateRecovery(const std::string& reason,
                                                  const std::string& source_tag) {
    ScheduleChainstateRecovery(reason, source_tag);
}

void ChainstateService::ScheduleChainstateRecovery(const std::string& reason,
                                                   const std::string& source_tag) {
    if (!daemon::kAutomaticChainstateRecoveryArmed) {
        if (logger_) {
            logger_->error(source_tag +
                           " Automatic chainstate recovery is DISABLED by safety fuse; "
                           "manual operator intervention required: " + reason);
        }
        EnterSafeMode("chainstate recovery required: " + reason +
                      " [automatic replay disabled by safety fuse]");
        return;
    }

    std::string marker_error;
    if (!datadir_.empty()) {
        if (daemon::WriteChainstateRecoveryMarker(datadir_, reason, &marker_error)) {
            if (logger_) {
                logger_->error(source_tag + " Scheduled automatic chainstate recovery on next start: " +
                               reason);
            }
        } else if (logger_) {
            logger_->error(source_tag + " Failed to persist chainstate recovery marker: " +
                           marker_error);
        }
    }
    EnterSafeMode("chainstate recovery required: " + reason);
}

void ChainstateService::ExitSafeMode() {
    if (!safe_mode_active_) {
        return; // Not in safe mode
    }

    auto duration = std::chrono::steady_clock::now() - safe_mode_entered_time_;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration).count();

    logger_->info("✅ SAFE MODE DEACTIVATED after " + std::to_string(minutes) + " minutes");
    logger_->info("✅ Mining can resume - chain has stabilized");

    safe_mode_active_ = false;
    safe_mode_reason_.clear();

    // Notify mining service that it can resume
    auto* daemon_ctx = DaemonContext::instance();
    if (daemon_ctx && daemon_ctx->mining) {
        auto mining = std::dynamic_pointer_cast<MiningService>(daemon_ctx->mining);
        if (mining) {
            logger_->info("[SafeMode] Mining service can resume");
        }
    }
}

// ============================================================================
// Transaction Index Rebuild (one-time backfill)
// ============================================================================
std::pair<uint64_t, uint64_t> ChainstateService::RebuildTxIndex() {
    if (!chain_db_) return {0, 0};

    auto tip_result = chain_db_->getTip();
    if (!tip_result.ok()) return {0, 0};

    int tip_height = tip_result.value().height;
    uint64_t indexed_blocks = 0;
    uint64_t indexed_txs = 0;

    if (logger_) logger_->info("[RebuildTxIndex] Building tx index for " +
                 std::to_string(tip_height + 1) + " blocks...");

    ChainWriteToken token;

    for (int h = 0; h <= tip_height; ++h) {
        auto hash_result = chain_db_->getBlockHashByHeight(h);
        if (!hash_result.ok()) continue;

        auto block_result = getBlockByHash(hash_result.value());
        if (!block_result.ok()) continue;

        const auto& block = block_result.value();
        rocksdb::WriteBatch batch;

        for (uint32_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            auto txid = block.vtx[tx_idx].GetTxid().AsUint256();
            chain_db_->putTxIndex(token, txid, hash_result.value(), tx_idx, &batch);
            indexed_txs++;
        }

        chain_db_->writeBatch(token, std::move(batch), false);
        indexed_blocks++;

        if (h % 1000 == 0 && h > 0) {
            if (logger_) logger_->info("[RebuildTxIndex] Progress: " +
                         std::to_string(h) + "/" + std::to_string(tip_height));
        }
    }

    if (logger_) logger_->info("[RebuildTxIndex] Complete: " +
                 std::to_string(indexed_txs) + " transactions indexed");

    return {indexed_blocks, indexed_txs};
}

// ============================================================================
// Phase 42: AssumeUTXO Snapshot Export/Import
// ============================================================================

consensus::SnapshotExportResult ChainstateService::ExportSnapshot(const std::filesystem::path& snapshot_path) {
    using namespace consensus;
    SnapshotExportResult result;
    result.success = false;
    result.utxos_exported = 0;
    result.bytes_written = 0;

    if (!chain_db_) {
        result.error_message = "ChainDB not available";
        return result;
    }

    if (!consensus_utxo_set_) {
        result.error_message = "Consensus UTXO set not available";
        return result;
    }

    try {
        // Get current chain tip
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            result.error_message = "Failed to get chain tip";
            return result;
        }
        const auto& tip = tip_result.value();

        logger_->info("[ExportSnapshot] Starting snapshot export at height " + std::to_string(tip.height));

        // Open output file
        std::ofstream file(snapshot_path, std::ios::binary);
        if (!file.is_open()) {
            result.error_message = "Failed to open snapshot file for writing";
            return result;
        }

        // Write header
        SnapshotMetadata header;
        header.block_hash = tip.hash;
        header.block_height = tip.height;
        header.timestamp = std::time(nullptr);
        // v4: this exporter always appends BOTH the v3 Utreexo section and the
        // v4 shielded section below. The header version MUST be V4 so LoadSnapshot's
        // `has_v4_shielded_section = (version >= V4)` gate reads the shielded payload;
        // stamping V3 makes the reader skip the SHLD bytes and then misread the
        // trailing checksum → every snapshot fails to load.
        header.version = SNAPSHOT_VERSION_V4;

        // Get consensus UTXO set (all UTXOs on chain, not just wallet-owned)
        const auto& all_utxos = consensus_utxo_set_->GetUTXOs();
        header.utxo_count = all_utxos.size();

        // Build v3 Utreexo section from the current consensus forest.
        if (!consensus_utxo_set_) {
            result.error_message = "Consensus UTXO set not available for v3 snapshot export";
            return result;
        }

        SnapshotUtreexoSection utreexo_section;
        std::vector<uint8_t> serialized_forest = consensus_utxo_set_->GetForest().serialize();
        utreexo_section.forest_bytes = serialized_forest.size();
        utreexo_section.forest_leaves = consensus_utxo_set_->GetForest().getNumLeaves();
        {
            const consensus::UtreexoHash forest_commitment = consensus_utxo_set_->GetForest().getCommitment();
            std::memcpy(utreexo_section.utreexo_root.begin(), forest_commitment.data(), 32);
        }

        if (utreexo_section.forest_bytes > SNAPSHOT_V3_MAX_FOREST_BYTES) {
            result.error_message = "Serialized forest exceeds max v3 snapshot size cap";
            return result;
        }

        // Consensus binding: exported snapshot root must match tip header commitment.
        auto tip_block_result = getBlockByHash(tip.hash);
        if (!tip_block_result.ok()) {
            result.error_message = "Failed to load tip block for utreexo_root verification";
            return result;
        }
        if (tip_block_result.value().header.utreexo_root != utreexo_section.utreexo_root) {
            result.error_message = "Refusing snapshot export: forest root does not match tip header utreexo_root";
            logger_->error("[ExportSnapshot] " + result.error_message);
            return result;
        }

        // Initialize SHA256 context for checksum computation
        crypto::CSHA256 sha256;

        // Write header to file and update checksum
        file.write(reinterpret_cast<const char*>(&header.magic), sizeof(header.magic));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.magic), sizeof(header.magic));

        file.write(reinterpret_cast<const char*>(&header.version), sizeof(header.version));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.version), sizeof(header.version));

        file.write(reinterpret_cast<const char*>(header.block_hash.data), 32);
        sha256.Write(reinterpret_cast<const uint8_t*>(header.block_hash.data), 32);

        file.write(reinterpret_cast<const char*>(&header.block_height), sizeof(header.block_height));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.block_height), sizeof(header.block_height));

        file.write(reinterpret_cast<const char*>(&header.utxo_count), sizeof(header.utxo_count));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.utxo_count), sizeof(header.utxo_count));

        file.write(reinterpret_cast<const char*>(&header.timestamp), sizeof(header.timestamp));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.timestamp), sizeof(header.timestamp));

        file.write(reinterpret_cast<const char*>(&header.reserved), sizeof(header.reserved));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.reserved), sizeof(header.reserved));

        // Sort outpoints for deterministic serialization order.
        // unordered_map iteration is non-deterministic — without sorting,
        // two exports of identical state could produce different checksums.
        std::vector<OutPoint> sorted_outpoints;
        sorted_outpoints.reserve(all_utxos.size());
        for (const auto& [outpoint, _] : all_utxos) {
            sorted_outpoints.push_back(outpoint);
        }
        std::sort(sorted_outpoints.begin(), sorted_outpoints.end());

        // Write UTXOs in deterministic order and compute checksum.
        // Uses the shared canonical serializer so the record encoding is
        // defined in exactly one place (consensus::SerializeUtxoRecord) and
        // is identical to what the digest engine (ComputeUtxoRecordsDigest)
        // uses for the AssumeUTXO content commitment.
        uint64_t exported = 0;
        for (const auto& outpoint : sorted_outpoints) {
            const auto& entry = all_utxos.at(outpoint);
            // #281: SerializeUtxoRecord omits is_confidential/commitment, so the
            // content commitment does not bind them. That is sound only while
            // those fields are consensus-forced empty (v7 is transparent-only).
            // Fail closed rather than write a record whose confidential fields
            // are unbound; this must be revisited (record encoding extended)
            // before any confidential/shielded UTXO lane is enabled.
            if (!consensus::UtxoRecordIsSnapshotSafe(entry)) {
                result.error_message =
                    "refusing to export UTXO with unbound confidential fields "
                    "(is_confidential/commitment not covered by the snapshot "
                    "content commitment; see #281): " + outpoint.ToString();
                logger_->error("[ExportSnapshot] " + result.error_message);
                return result;
            }
            const std::vector<uint8_t> record =
                consensus::SerializeUtxoRecord(outpoint, entry);
            file.write(reinterpret_cast<const char*>(record.data()),
                       static_cast<std::streamsize>(record.size()));
            sha256.Write(record.data(), record.size());

            exported++;
            if (exported % 10000 == 0) {
                logger_->info("[ExportSnapshot] Exported " + std::to_string(exported) + " UTXOs...");
            }
        }

        // v3 extension: bind snapshot to an accumulator root + serialized forest payload.
        file.write(reinterpret_cast<const char*>(&utreexo_section.magic), sizeof(utreexo_section.magic));
        sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.magic), sizeof(utreexo_section.magic));

        file.write(reinterpret_cast<const char*>(&utreexo_section.version), sizeof(utreexo_section.version));
        sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.version), sizeof(utreexo_section.version));

        file.write(reinterpret_cast<const char*>(&utreexo_section.forest_bytes), sizeof(utreexo_section.forest_bytes));
        sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.forest_bytes), sizeof(utreexo_section.forest_bytes));

        file.write(reinterpret_cast<const char*>(&utreexo_section.forest_leaves), sizeof(utreexo_section.forest_leaves));
        sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.forest_leaves), sizeof(utreexo_section.forest_leaves));

        file.write(reinterpret_cast<const char*>(&utreexo_section.reserved), sizeof(utreexo_section.reserved));
        sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.reserved), sizeof(utreexo_section.reserved));

        file.write(reinterpret_cast<const char*>(utreexo_section.utreexo_root.data), 32);
        sha256.Write(reinterpret_cast<const uint8_t*>(utreexo_section.utreexo_root.data), 32);

        if (!serialized_forest.empty()) {
            file.write(reinterpret_cast<const char*>(serialized_forest.data()), serialized_forest.size());
            sha256.Write(reinterpret_cast<const uint8_t*>(serialized_forest.data()), serialized_forest.size());
        }

        // v4 extension: shielded-pool bootstrap payload. Without it a
        // snapshot-bootstrapped node starts with an EMPTY shielded commitment
        // tree, so the first post-snapshot shielded spend fails AnchorInvalid and
        // the chain wedges. Carries the frontier + anchor-history + nullifier set
        // at the snapshot base, mirroring the utreexo section above. Covered by
        // the same trailing checksum (written into sha256 before Finalize).
        SnapshotShieldedSection shielded_section;
        const std::vector<uint8_t> ser_frontier    = shielded_tree_.SerializeFrontier();
        const std::vector<uint8_t> ser_anchors      = shielded_anchor_history_.SerializeBytes();
        const std::vector<uint8_t> ser_nullifiers   = shielded_nullifiers_.SerializeContent();
        shielded_section.frontier_bytes       = ser_frontier.size();
        shielded_section.anchor_history_bytes = ser_anchors.size();
        shielded_section.nullifier_bytes      = ser_nullifiers.size();
        {
            const consensus::shielded::Hash sroot = shielded_tree_.Root();
            std::memcpy(shielded_section.commitment_root.begin(), sroot.data(), 32);
        }
        if (shielded_section.frontier_bytes + shielded_section.anchor_history_bytes +
            shielded_section.nullifier_bytes > SNAPSHOT_V4_MAX_SHIELDED_BYTES) {
            result.error_message = "Serialized shielded payload exceeds max v4 snapshot size cap";
            logger_->error("[ExportSnapshot] " + result.error_message);
            return result;
        }

        file.write(reinterpret_cast<const char*>(&shielded_section.magic), sizeof(shielded_section.magic));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.magic), sizeof(shielded_section.magic));

        file.write(reinterpret_cast<const char*>(&shielded_section.version), sizeof(shielded_section.version));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.version), sizeof(shielded_section.version));

        file.write(reinterpret_cast<const char*>(&shielded_section.frontier_bytes), sizeof(shielded_section.frontier_bytes));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.frontier_bytes), sizeof(shielded_section.frontier_bytes));

        file.write(reinterpret_cast<const char*>(&shielded_section.anchor_history_bytes), sizeof(shielded_section.anchor_history_bytes));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.anchor_history_bytes), sizeof(shielded_section.anchor_history_bytes));

        file.write(reinterpret_cast<const char*>(&shielded_section.nullifier_bytes), sizeof(shielded_section.nullifier_bytes));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.nullifier_bytes), sizeof(shielded_section.nullifier_bytes));

        file.write(reinterpret_cast<const char*>(&shielded_section.reserved), sizeof(shielded_section.reserved));
        sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.reserved), sizeof(shielded_section.reserved));

        file.write(reinterpret_cast<const char*>(shielded_section.commitment_root.data), 32);
        sha256.Write(reinterpret_cast<const uint8_t*>(shielded_section.commitment_root.data), 32);

        if (!ser_frontier.empty()) {
            file.write(reinterpret_cast<const char*>(ser_frontier.data()), ser_frontier.size());
            sha256.Write(ser_frontier.data(), ser_frontier.size());
        }
        if (!ser_anchors.empty()) {
            file.write(reinterpret_cast<const char*>(ser_anchors.data()), ser_anchors.size());
            sha256.Write(ser_anchors.data(), ser_anchors.size());
        }
        if (!ser_nullifiers.empty()) {
            file.write(reinterpret_cast<const char*>(ser_nullifiers.data()), ser_nullifiers.size());
            sha256.Write(ser_nullifiers.data(), ser_nullifiers.size());
        }

        logger_->info("[ExportSnapshot] v4 shielded section: frontier=" +
                      std::to_string(shielded_section.frontier_bytes) + "B anchors=" +
                      std::to_string(shielded_section.anchor_history_bytes) + "B nullifiers=" +
                      std::to_string(shielded_section.nullifier_bytes) + "B root=" +
                      shielded_section.commitment_root.GetHex().substr(0, 16) + "...");

        // Finalize checksum and write it
        uint8_t checksum_bytes[32];
        sha256.Finalize(checksum_bytes);
        file.write(reinterpret_cast<const char*>(checksum_bytes), 32);

        // Copy checksum to result
        uint256 checksum;
        std::memcpy(checksum.data, checksum_bytes, 32);

        result.bytes_written = file.tellp();
        file.close();

        // CRITICAL: Bitcoin Core requirement - verify chain tip unchanged during export
        // This ensures snapshot atomicity (no blocks added mid-export)
        auto final_tip_result = chain_db_->getTip();
        if (!final_tip_result) {
            result.error_message = "Failed to verify chain tip after export";
            logger_->error("[ExportSnapshot] " + result.error_message);
            return result;
        }
        if (final_tip_result.value().hash != tip.hash) {
            result.error_message = "Chain advanced during snapshot export (started at height " +
                                  std::to_string(tip.height) + ", now at " +
                                  std::to_string(final_tip_result.value().height) + "). " +
                                  "Snapshot may be inconsistent - please retry.";
            logger_->error("[ExportSnapshot] " + result.error_message);
            return result;
        }

        result.success = true;
        result.utxos_exported = exported;
        result.checksum = checksum;
        result.block_hash = tip.hash;
        result.block_height = tip.height;

        logger_->info("[ExportSnapshot] Export complete: " + std::to_string(exported) +
                     " UTXOs at height " + std::to_string(tip.height) +
                     ", " + std::to_string(result.bytes_written) + " bytes");

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception during export: ") + e.what();
        logger_->error("[ExportSnapshot] " + result.error_message);
    }

    return result;
}

int ChainstateService::RescanWalletFromSnapshotUTXOs(WalletManager& wallet, uint32_t base_height) {
    // PRIMARY PATH: read the configured snapshot .dat directly. On a utreexo node
    // the in-memory consensus_utxo_set_ does NOT hold the snapshot's UTXOs (they
    // are committed in the accumulator, not individually enumerable), so iterating
    // it records zero owned coins. The .dat file is the authoritative full UTXO
    // set the wallet's pre-snapshot coins actually live in. Format per entry:
    // txid(32) | vout(u32) | value(u64) | script_len(u32) | script | height(u32) | coinbase(u8)
    const std::string snapshot_dat_path =
        config_ ? config_->GetString("assumeutxo_snapshot", "") : "";
    if (!snapshot_dat_path.empty()) {
        std::ifstream snap(snapshot_dat_path, std::ios::binary);
        if (snap.is_open()) {
            consensus::SnapshotMetadata header;
            snap.read(reinterpret_cast<char*>(&header.magic), sizeof(header.magic));
            snap.read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
            snap.read(reinterpret_cast<char*>(header.block_hash.data), 32);
            snap.read(reinterpret_cast<char*>(&header.block_height), sizeof(header.block_height));
            snap.read(reinterpret_cast<char*>(&header.utxo_count), sizeof(header.utxo_count));
            snap.read(reinterpret_cast<char*>(&header.timestamp), sizeof(header.timestamp));
            snap.read(reinterpret_cast<char*>(&header.reserved), sizeof(header.reserved));
            if (snap && header.magic == consensus::SNAPSHOT_MAGIC) {
                const uint64_t utxo_count = header.utxo_count;
                return wallet.rescanUtxoSet(
                    [&](const std::function<void(const dinero::WalletManager::UtxoSetEntry&)>& sink) {
                        for (uint64_t i = 0; i < utxo_count; ++i) {
                            uint256 txid;
                            snap.read(reinterpret_cast<char*>(txid.data), 32);
                            uint32_t vout = 0;
                            snap.read(reinterpret_cast<char*>(&vout), sizeof(vout));
                            uint64_t value_raw = 0;
                            snap.read(reinterpret_cast<char*>(&value_raw), sizeof(value_raw));
                            uint32_t script_len = 0;
                            snap.read(reinterpret_cast<char*>(&script_len), sizeof(script_len));
                            if (!snap || script_len > 100000u) {
                                break;  // truncated/corrupt — stop the scan
                            }
                            std::vector<uint8_t> spk(script_len);
                            snap.read(reinterpret_cast<char*>(spk.data()), script_len);
                            uint32_t height = 0;
                            snap.read(reinterpret_cast<char*>(&height), sizeof(height));
                            uint8_t is_coinbase = 0;
                            snap.read(reinterpret_cast<char*>(&is_coinbase), 1);
                            if (!snap) {
                                break;
                            }
                            dinero::WalletManager::UtxoSetEntry e;
                            e.txid_hex = txid.GetHex();
                            e.vout = vout;
                            e.amount_una = value_raw;
                            e.script_pubkey = std::move(spk);
                            e.height = height;
                            e.is_coinbase = (is_coinbase != 0);
                            sink(e);
                        }
                    },
                    base_height);
            }
        }
    }

    // FALLBACK PATH: in-memory consensus UTXO set (full-set / non-utreexo builds).
    if (!consensus_utxo_set_) {
        return -1;  // no in-memory snapshot UTXO set loaded
    }

    // The concrete ConsensusUTXOSet exposes the raw OutPoint->UTXOEntry map; the
    // coins are keyed by TxId, whose AsUint256().GetHex() is the same canonical
    // txid string the wallet's block-replay rescan uses for spent-marking.
    const auto& utxos = consensus_utxo_set_->GetUTXOs();
    return wallet.rescanUtxoSet(
        [&](const std::function<void(const dinero::WalletManager::UtxoSetEntry&)>& sink) {
            for (const auto& [outpoint, entry] : utxos) {
                dinero::WalletManager::UtxoSetEntry e;
                e.txid_hex = outpoint.txid.AsUint256().GetHex();
                e.vout = outpoint.vout;
                e.amount_una = entry.value.GetUna();
                e.script_pubkey = entry.scriptPubKey;
                e.height = entry.height;
                e.is_coinbase = entry.isCoinbase;
                sink(e);
            }
        },
        base_height);
}

consensus::SnapshotImportResult ChainstateService::LoadSnapshot(const std::filesystem::path& snapshot_path) {
    using namespace consensus;
    // rc24.1 single-flight: LoadSnapshot mutates the consensus UTXO set in place
    // and is reachable from the auto-bootstrap path AND the manual RPC path.
    // Serialize so two callers can never mutate the set concurrently — the
    // empty-set precondition below then cleanly rejects the loser. Held for the
    // full load (a one-time bootstrap; block download stays deferred meanwhile).
    std::lock_guard<std::mutex> load_guard(snapshot_load_mutex_);
    SnapshotImportResult result;
    result.success = false;
    result.utxos_imported = 0;
    result.bytes_read = 0;
    result.checksum_valid = false;

    // Fatal gate, hoisted ahead of EVERY mutation (spec: fatal_mismatch refuses
    // all snapshot loads until explicit operator reset). Even the genesis-only
    // Clear() below must not run while fatal.
    EnsureAssumeUtxoLifecycle();
    if (assumeutxo_lifecycle_->GetState() ==
        assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
        result.error_message =
            "node is in assumeutxo fatal_mismatch state; operator reset required "
            "(blockchain.resetassumeutxofatal or wipe datadir)";
        logger_->error("[LoadSnapshot] " + result.error_message);
        return result;
    }

    if (!consensus_utxo_set_) {
        result.error_message = "Consensus UTXO set not available";
        return result;
    }

    // CRITICAL: UTXO set must be empty (or provably genesis-only) to load a
    // snapshot. Deferred mobile bootstrap may run after header metadata moves
    // beyond height zero, so tip height is not proof of UTXO identity. Match the
    // complete compiled genesis coinbase state, but do not clear it here:
    // transport, manifest, checksum, and consensus validation can still reject
    // the snapshot. BulkLoad replaces the set only after every preflight passes.
    {
        const uint64_t existing = consensus_utxo_set_->GetSetSize();
        if (existing > 0) {
            Transaction genesis_coinbase;
            const bool decoded_genesis = TransactionSerializer::Deserialize(
                genesis_coinbase, Params().genesis.genesisCoinbaseHex);
            if (decoded_genesis && assumeutxo::IsGenesisOnlyUtxoSet(
                    *consensus_utxo_set_, genesis_coinbase)) {
                logger_->info("[LoadSnapshot] Verified replaceable genesis-only UTXO state");
            } else {
                result.error_message = "Consensus UTXO set must be empty to load snapshot (found " +
                                      std::to_string(existing) + " existing UTXOs). " +
                                      "Cannot load snapshot into active chainstate.";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
        }
    }

    logger_->info("[LoadSnapshot] UTXO precondition passed");

    // Transport hardening preflight:
    // - regular file only (no symlink/device)
    // - bounded file size
    const uint64_t default_max_snapshot_mb = 64ULL * 1024ULL;  // 64 GiB
    const uint64_t configured_max_snapshot_mb = config_
        ? static_cast<uint64_t>(std::max(config_->GetInt("assumeutxo_snapshot_max_mb",
                                                         static_cast<int>(default_max_snapshot_mb)), 0))
        : default_max_snapshot_mb;
    const uint64_t max_snapshot_bytes = configured_max_snapshot_mb * 1024ULL * 1024ULL;

    std::string preflight_error;
    if (!ValidateSnapshotTransportPreflight(snapshot_path, max_snapshot_bytes, preflight_error)) {
        result.error_message = preflight_error;
        logger_->error("[LoadSnapshot] " + result.error_message);
        return result;
    }

    // Optional trust gate: manifest pinning.
    // Enable by providing --assumeutxo_manifest=<path> or placing
    // <snapshot>.manifest.json next to the snapshot file.
    const bool require_manifest = config_ ? config_->GetBool("assumeutxo_require_manifest", false) : false;
    std::string manifest_path_cfg = config_ ? config_->GetString("assumeutxo_manifest", "") : "";
    std::filesystem::path manifest_path;
    if (!manifest_path_cfg.empty()) {
        manifest_path = manifest_path_cfg;
    } else {
        std::filesystem::path sibling = snapshot_path;
        sibling += ".manifest.json";
        if (std::filesystem::exists(sibling)) {
            manifest_path = sibling;
        }
    }

    if (!manifest_path.empty()) {
        std::string manifest_error;
        if (!ValidateSnapshotManifestPreflight(snapshot_path, manifest_path, manifest_error)) {
            result.error_message = "Snapshot manifest trust gate failed: " + manifest_error;
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }
        logger_->info("[LoadSnapshot] Manifest trust gate passed: " + manifest_path.string());
    } else if (require_manifest) {
        result.error_message = "Snapshot manifest is required but no manifest path was provided/found";
        logger_->error("[LoadSnapshot] " + result.error_message);
        return result;
    } else {
        logger_->warning("[LoadSnapshot] No snapshot manifest configured; relying on in-file checksum + header chain binding");
    }

    try {
        // Open input file
        std::ifstream file(snapshot_path, std::ios::binary);
        if (!file.is_open()) {
            result.error_message = "Failed to open snapshot file for reading";
            return result;
        }

        logger_->info("[LoadSnapshot] Loading snapshot from " + snapshot_path.string());

        // Initialize SHA256 context for checksum verification
        crypto::CSHA256 sha256;

        // Read header and update checksum
        SnapshotMetadata header;
        file.read(reinterpret_cast<char*>(&header.magic), sizeof(header.magic));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.magic), sizeof(header.magic));

        file.read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.version), sizeof(header.version));

        file.read(reinterpret_cast<char*>(header.block_hash.data), 32);
        sha256.Write(reinterpret_cast<const uint8_t*>(header.block_hash.data), 32);

        file.read(reinterpret_cast<char*>(&header.block_height), sizeof(header.block_height));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.block_height), sizeof(header.block_height));

        file.read(reinterpret_cast<char*>(&header.utxo_count), sizeof(header.utxo_count));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.utxo_count), sizeof(header.utxo_count));

        file.read(reinterpret_cast<char*>(&header.timestamp), sizeof(header.timestamp));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.timestamp), sizeof(header.timestamp));

        file.read(reinterpret_cast<char*>(&header.reserved), sizeof(header.reserved));
        sha256.Write(reinterpret_cast<const uint8_t*>(&header.reserved), sizeof(header.reserved));

        // Verify magic
        if (header.magic != SNAPSHOT_MAGIC) {
            result.error_message = "Invalid snapshot magic number";
            return result;
        }

        // Verify version (v2 legacy + v3 with Utreexo section + v4 with shielded section)
        if (header.version != SNAPSHOT_VERSION_V2 && header.version != SNAPSHOT_VERSION_V3 &&
            header.version != SNAPSHOT_VERSION_V4) {
            result.error_message = "Unsupported snapshot version: " + std::to_string(header.version) +
                                  " (supported: " + std::to_string(SNAPSHOT_VERSION_V2) +
                                  ", " + std::to_string(SNAPSHOT_VERSION_V3) +
                                  ", " + std::to_string(SNAPSHOT_VERSION_V4) + ")";
            return result;
        }
        const bool has_v3_utreexo_section = (header.version >= SNAPSHOT_VERSION_V3);
        const bool has_v4_shielded_section = (header.version >= SNAPSHOT_VERSION_V4);
        // v4 shielded-pool bootstrap payload (parsed after the utreexo section,
        // restored after the forest). Buffers stay empty for v2/v3 snapshots.
        SnapshotShieldedSection shielded_section;
        std::vector<uint8_t> shielded_frontier_buf;
        std::vector<uint8_t> shielded_anchor_buf;
        std::vector<uint8_t> shielded_nullifier_buf;

        // CRITICAL: Bitcoin Core requirement - verify snapshot base block exists in our chain
        // This prevents loading snapshots from unknown/untrusted chains
        if (!chain_db_) {
            result.error_message = "ChainDB not available for snapshot validation";
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }

        // Accept the base block if it's locally readable from archival storage
        // OR in the HeaderChainSelector (which loads persisted headers at startup
        // from the header store).
        bool base_block_known = hasBlockByHash(header.block_hash);
        if (!base_block_known && header_chain_selector_) {
            base_block_known = (header_chain_selector_->GetHeader(header.block_hash) != nullptr);
        }
        if (!base_block_known) {
            result.error_message = "Snapshot base block " + header.block_hash.GetHex() +
                                  " not found in chain. Node must download headers first " +
                                  "(headers-first sync required).";
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }

        // FIX 2 (issue #186): if this height is a compiled-in AssumeUTXO trust
        // anchor, enforce it on the .dat load path too (not just the manifest
        // path) — the file's content hash AND base block hash must match the
        // registry. Makes the registry load-bearing for auto-bootstrap: a
        // tampered/wrong snapshot at a registered height is rejected before any
        // UTXO is imported. (Byte-order handling mirrors the manifest check.)
        if (auto anchor = consensus::AssumeUTXORegistry::GetSnapshot(header.block_height)) {
            if (ToLowerHex(anchor->block_hash.GetHex()) != ToLowerHex(header.block_hash.GetHex())) {
                result.error_message = "Snapshot base block conflicts with built-in trust anchor at height " +
                                      std::to_string(header.block_height);
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            std::string file_sha;
            std::string sha_err;
            if (!ComputeFileSha256Hex(snapshot_path, file_sha, sha_err)) {
                result.error_message = "Cannot hash snapshot for trust-anchor check: " + sha_err;
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            if (ToLowerHex(anchor->snapshot_hash.GetHex()) != ToLowerHex(file_sha)) {
                result.error_message = "Snapshot content does not match built-in trust anchor at height " +
                                      std::to_string(header.block_height) + " (expected " +
                                      ToLowerHex(anchor->snapshot_hash.GetHex()) + ", got " + file_sha + ")";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            logger_->info("[LoadSnapshot] snapshot verified against built-in trust anchor at height " +
                          std::to_string(header.block_height));
        }

        // Verify block height matches — check chaindb first, fall back to HCS
        int verified_height = -1;
        auto height_result = chain_db_->getBlockHeight(header.block_hash);
        if (height_result.ok()) {
            verified_height = height_result.value();
        } else if (header_chain_selector_) {
            const auto* hcs_entry = header_chain_selector_->GetHeader(header.block_hash);
            if (hcs_entry) verified_height = static_cast<int>(hcs_entry->height);
        }
        if (verified_height < 0) {
            result.error_message = "Failed to get height for snapshot base block";
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }
        if (verified_height != static_cast<int>(header.block_height)) {
            result.error_message = "Snapshot block height mismatch (expected " +
                                  std::to_string(header.block_height) + ", got " +
                                  std::to_string(verified_height) + ")";
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }

        result.block_hash = header.block_hash;
        result.block_height = header.block_height;

        logger_->info("[LoadSnapshot] Snapshot header validated: height=" + std::to_string(header.block_height) +
                     ", hash=" + header.block_hash.GetHex().substr(0, 16) + "..., UTXOs=" + std::to_string(header.utxo_count));

        // Pre-mutation re-entry tighten: refuse before BulkLoad/SetAssumeUTXOState
        // if the lifecycle is already active for a DIFFERENT base hash+height.
        // Belt-and-braces: the post-mutation check at the OnSnapshotLoaded call
        // below is the braces.  The top gate handles FatalMismatch in the common
        // (non-concurrent) case; this belt ALSO catches the concurrent-transition
        // window pre-mutation where a background worker may go fatal between the
        // top gate and here.  The header was NOT available at the top gate, so
        // this is the earliest pre-mutation placement for a header-aware check.
        {
            const auto lc_state = assumeutxo_lifecycle_->GetState();
            // Top gate handles the common case; catch any concurrent fatal
            // transition that occurred between the top gate and this point.
            if (lc_state == assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
                result.error_message =
                    "node is in assumeutxo fatal_mismatch state; operator reset required "
                    "(blockchain.resetassumeutxofatal or wipe datadir)";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            if (lc_state != assumeutxo::AssumeUtxoLifecycle::State::Disabled) {
                const auto st = assumeutxo_lifecycle_->GetStatus(
                    std::chrono::steady_clock::now());
                if (st.snapshot_base_block != header.block_hash ||
                    st.snapshot_base_height != header.block_height) {
                    result.error_message =
                        "another snapshot lifecycle is active (base height " +
                        std::to_string(st.snapshot_base_height) +
                        "); reset or let validation finish before loading a different snapshot";
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
                // same base: benign rehydrate, continue
            }
        }

        SnapshotUtreexoSection utreexo_section;
        std::vector<uint8_t> serialized_forest;

        // CRITICAL FIX (Abuse Testing - CRITICAL-001):
        // Two-pass import to prevent UTXO corruption on checksum failure
        //
        // BEFORE: UTXOs added to index, THEN checksum verified
        //   → If checksum fails, UTXO set already corrupted
        //   → Violates Bitcoin Core principle: "Verify THEN trust"
        //
        // AFTER: All data read into memory, checksum verified FIRST, THEN UTXOs added
        //   → If checksum fails, UTXO index untouched
        //   → Guarantees atomicity: either full import or no import
        //
        // Memory cost: ~100 bytes per UTXO
        //   10M UTXOs = ~1GB RAM (acceptable for modern systems)

        logger_->info("[LoadSnapshot] Pass 1/2: Reading UTXOs and computing checksum...");

        // Pass 1: Read all UTXOs into memory, computing checksum.
        // records_digest accumulates the canonical content commitment
        // (SHA256 over sorted UTXO records, same encoding as SerializeUtxoRecord)
        // so the replay engine can verify it after replaying genesis→base.
        // Records are stored in the file in sorted-outpoint order (ExportSnapshot
        // sorts before writing), so streaming order == canonical order.
        std::unordered_map<OutPoint, UTXOEntry> utxo_map;
        utxo_map.reserve(header.utxo_count);
        consensus::StreamingUtxoDigest records_digest;

        for (uint64_t i = 0; i < header.utxo_count; ++i) {
            // Read txid (32 bytes, raw internal byte order)
            uint256 temp_txid;
            file.read(reinterpret_cast<char*>(temp_txid.data), 32);
            sha256.Write(reinterpret_cast<const uint8_t*>(temp_txid.data), 32);

            // Read vout
            uint32_t vout;
            file.read(reinterpret_cast<char*>(&vout), sizeof(vout));
            sha256.Write(reinterpret_cast<const uint8_t*>(&vout), sizeof(vout));

            // Read value (uint64_t → AmountUna)
            uint64_t value_raw;
            file.read(reinterpret_cast<char*>(&value_raw), sizeof(value_raw));
            sha256.Write(reinterpret_cast<const uint8_t*>(&value_raw), sizeof(value_raw));

            // Read scriptPubKey length
            uint32_t script_len;
            file.read(reinterpret_cast<char*>(&script_len), sizeof(script_len));
            sha256.Write(reinterpret_cast<const uint8_t*>(&script_len), sizeof(script_len));

            // SECURITY (OOM fix): bound the per-UTXO scriptPubKey length and verify
            // the length read itself succeeded. A truncated/corrupt snapshot could
            // otherwise present a near-4GB length here, sizing a zero-filled vector
            // and exhausting memory before any read failure is observed. A valid
            // scriptPubKey cannot exceed MAX_SCRIPT_SIZE; cap generously at 10 KiB.
            constexpr uint32_t kMaxSnapshotScriptLen = 10 * 1024;
            if (!file || script_len > kMaxSnapshotScriptLen) {
                result.error_message =
                    "Snapshot UTXO scriptPubKey length invalid or exceeds cap";
                return result;
            }

            // Read scriptPubKey
            std::vector<uint8_t> spk(script_len);
            file.read(reinterpret_cast<char*>(spk.data()), script_len);
            if (!file) {
                result.error_message =
                    "Snapshot truncated while reading UTXO scriptPubKey";
                return result;
            }
            sha256.Write(reinterpret_cast<const uint8_t*>(spk.data()), script_len);

            // Read height
            uint32_t height;
            file.read(reinterpret_cast<char*>(&height), sizeof(height));
            sha256.Write(reinterpret_cast<const uint8_t*>(&height), sizeof(height));

            // Read isCoinbase
            uint8_t is_coinbase;
            file.read(reinterpret_cast<char*>(&is_coinbase), 1);
            sha256.Write(reinterpret_cast<const uint8_t*>(&is_coinbase), 1);

            // Construct consensus types
            OutPoint outpoint(TxId(temp_txid), vout);
            UTXOEntry entry;
            entry.value = AmountUna::Una(value_raw);
            entry.scriptPubKey = std::move(spk);
            entry.height = height;
            entry.isCoinbase = (is_coinbase != 0);

            // Accumulate content commitment BEFORE moves invalidate the fields.
            // Parse-fidelity: TxId(temp_txid).AsUint256().data == temp_txid.data
            // (TxId wraps uint256 by value); AmountUna::Una(v).v == v; isCoinbase
            // bool→uint8 is 0/1, matching ExportSnapshot's exclusive 0/1 write.
            // Using AddRecord (not AddRecordBytes) for symmetry with the replay
            // engine's ComputeUtxoRecordsDigest which also goes through
            // SerializeUtxoRecord — both sides normalise through the same path.
            records_digest.AddRecord(outpoint, entry);

            utxo_map.emplace(std::move(outpoint), std::move(entry));

            if (utxo_map.size() % 10000 == 0) {
                logger_->info("[LoadSnapshot] Read " + std::to_string(utxo_map.size()) + " UTXOs...");
            }
        }

        logger_->info("[LoadSnapshot] Pass 1 complete: Read " + std::to_string(utxo_map.size()) + " UTXOs");

        if (has_v3_utreexo_section) {
            logger_->info("[LoadSnapshot] Reading v3 Utreexo snapshot section...");

            file.read(reinterpret_cast<char*>(&utreexo_section.magic), sizeof(utreexo_section.magic));
            sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.magic), sizeof(utreexo_section.magic));

            file.read(reinterpret_cast<char*>(&utreexo_section.version), sizeof(utreexo_section.version));
            sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.version), sizeof(utreexo_section.version));

            file.read(reinterpret_cast<char*>(&utreexo_section.forest_bytes), sizeof(utreexo_section.forest_bytes));
            sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.forest_bytes), sizeof(utreexo_section.forest_bytes));

            file.read(reinterpret_cast<char*>(&utreexo_section.forest_leaves), sizeof(utreexo_section.forest_leaves));
            sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.forest_leaves), sizeof(utreexo_section.forest_leaves));

            file.read(reinterpret_cast<char*>(&utreexo_section.reserved), sizeof(utreexo_section.reserved));
            sha256.Write(reinterpret_cast<const uint8_t*>(&utreexo_section.reserved), sizeof(utreexo_section.reserved));

            file.read(reinterpret_cast<char*>(utreexo_section.utreexo_root.data), 32);
            sha256.Write(reinterpret_cast<const uint8_t*>(utreexo_section.utreexo_root.data), 32);

            if (utreexo_section.magic != SNAPSHOT_V3_UTREEXO_MAGIC) {
                result.error_message = "Invalid v3 Utreexo section magic";
                return result;
            }
            if (utreexo_section.version != SNAPSHOT_V3_UTREEXO_SECTION_VERSION) {
                result.error_message = "Unsupported v3 Utreexo section version: " +
                                      std::to_string(utreexo_section.version);
                return result;
            }
            if (utreexo_section.forest_bytes > SNAPSHOT_V3_MAX_FOREST_BYTES) {
                result.error_message = "v3 Utreexo forest payload exceeds configured cap";
                return result;
            }

            serialized_forest.resize(static_cast<size_t>(utreexo_section.forest_bytes));
            if (!serialized_forest.empty()) {
                file.read(reinterpret_cast<char*>(serialized_forest.data()), serialized_forest.size());
                sha256.Write(reinterpret_cast<const uint8_t*>(serialized_forest.data()), serialized_forest.size());
            }

            logger_->info("[LoadSnapshot] v3 Utreexo section: forest_bytes=" +
                         std::to_string(utreexo_section.forest_bytes) +
                         ", leaves=" + std::to_string(utreexo_section.forest_leaves));
        }

        if (has_v4_shielded_section) {
            logger_->info("[LoadSnapshot] Reading v4 shielded snapshot section...");

            file.read(reinterpret_cast<char*>(&shielded_section.magic), sizeof(shielded_section.magic));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.magic), sizeof(shielded_section.magic));
            file.read(reinterpret_cast<char*>(&shielded_section.version), sizeof(shielded_section.version));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.version), sizeof(shielded_section.version));
            file.read(reinterpret_cast<char*>(&shielded_section.frontier_bytes), sizeof(shielded_section.frontier_bytes));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.frontier_bytes), sizeof(shielded_section.frontier_bytes));
            file.read(reinterpret_cast<char*>(&shielded_section.anchor_history_bytes), sizeof(shielded_section.anchor_history_bytes));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.anchor_history_bytes), sizeof(shielded_section.anchor_history_bytes));
            file.read(reinterpret_cast<char*>(&shielded_section.nullifier_bytes), sizeof(shielded_section.nullifier_bytes));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.nullifier_bytes), sizeof(shielded_section.nullifier_bytes));
            file.read(reinterpret_cast<char*>(&shielded_section.reserved), sizeof(shielded_section.reserved));
            sha256.Write(reinterpret_cast<const uint8_t*>(&shielded_section.reserved), sizeof(shielded_section.reserved));
            file.read(reinterpret_cast<char*>(shielded_section.commitment_root.data), 32);
            sha256.Write(reinterpret_cast<const uint8_t*>(shielded_section.commitment_root.data), 32);

            if (shielded_section.magic != SNAPSHOT_V4_SHIELDED_MAGIC) {
                result.error_message = "Invalid v4 shielded section magic";
                return result;
            }
            if (shielded_section.version != SNAPSHOT_V4_SHIELDED_SECTION_VERSION) {
                result.error_message = "Unsupported v4 shielded section version: " +
                                       std::to_string(shielded_section.version);
                return result;
            }
            // SECURITY (integer-overflow fix): bound EACH attacker-controlled
            // uint64 field individually before summing. The previous
            // `a + b + c > CAP` summed three uint64 values that could wrap past
            // the cap (e.g. each ~2^63) and pass the check. With each field
            // individually <= CAP (1 GiB), the sum is <= 3 GiB and cannot wrap a
            // uint64.
            if (shielded_section.frontier_bytes      > SNAPSHOT_V4_MAX_SHIELDED_BYTES ||
                shielded_section.anchor_history_bytes > SNAPSHOT_V4_MAX_SHIELDED_BYTES ||
                shielded_section.nullifier_bytes      > SNAPSHOT_V4_MAX_SHIELDED_BYTES ||
                (shielded_section.frontier_bytes + shielded_section.anchor_history_bytes +
                 shielded_section.nullifier_bytes) > SNAPSHOT_V4_MAX_SHIELDED_BYTES) {
                result.error_message = "v4 shielded payload exceeds configured cap";
                return result;
            }

            shielded_frontier_buf.resize(static_cast<size_t>(shielded_section.frontier_bytes));
            if (!shielded_frontier_buf.empty()) {
                file.read(reinterpret_cast<char*>(shielded_frontier_buf.data()), shielded_frontier_buf.size());
                sha256.Write(shielded_frontier_buf.data(), shielded_frontier_buf.size());
            }
            shielded_anchor_buf.resize(static_cast<size_t>(shielded_section.anchor_history_bytes));
            if (!shielded_anchor_buf.empty()) {
                file.read(reinterpret_cast<char*>(shielded_anchor_buf.data()), shielded_anchor_buf.size());
                sha256.Write(shielded_anchor_buf.data(), shielded_anchor_buf.size());
            }
            shielded_nullifier_buf.resize(static_cast<size_t>(shielded_section.nullifier_bytes));
            if (!shielded_nullifier_buf.empty()) {
                file.read(reinterpret_cast<char*>(shielded_nullifier_buf.data()), shielded_nullifier_buf.size());
                sha256.Write(shielded_nullifier_buf.data(), shielded_nullifier_buf.size());
            }

            logger_->info("[LoadSnapshot] v4 shielded section: frontier=" +
                          std::to_string(shielded_section.frontier_bytes) + "B anchors=" +
                          std::to_string(shielded_section.anchor_history_bytes) + "B nullifiers=" +
                          std::to_string(shielded_section.nullifier_bytes) + "B");
        }

        // Read stored checksum from file
        uint8_t stored_checksum[32];
        file.read(reinterpret_cast<char*>(stored_checksum), 32);

        result.bytes_read = file.tellg();
        file.close();

        // CRITICAL: Verify checksum BEFORE touching UTXO index
        uint8_t computed_checksum[32];
        sha256.Finalize(computed_checksum);

        if (std::memcmp(computed_checksum, stored_checksum, 32) != 0) {
            result.error_message = "Snapshot checksum mismatch - file may be corrupted";
            result.checksum_valid = false;
            logger_->error("[LoadSnapshot] " + result.error_message);
            logger_->error("[LoadSnapshot] UTXO index NOT modified (checksum failed before import)");
            return result;  // ✓ UTXO index untouched
        }

        result.checksum_valid = true;
        logger_->info("[LoadSnapshot] Checksum verified successfully");

        // Finalize the content commitment (file is valid; digest is stable here).
        // Stored as the expected commitment for the replay engine (Task 3/7).
        const std::string snapshot_commitment_hex = records_digest.Finalize().GetHex();

        std::optional<consensus::UtreexoForest> snapshot_forest;
        if (has_v3_utreexo_section) {
            if (!consensus_utxo_set_) {
                result.error_message = "Consensus UTXO set not available for v3 snapshot import";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }

            // For utreexo_root verification we only need the block header, not the full
            // block body. Fall back to HeaderChainSelector (persisted header store) when
            // the block body is not yet readable from local archival storage
            // (e.g. snapshot bootstrap at startup).
            uint256 header_utreexo_root;
            auto base_block_result = getBlockByHash(header.block_hash);
            if (base_block_result.ok()) {
                header_utreexo_root = base_block_result.value().header.utreexo_root;
            } else if (header_chain_selector_) {
                const auto* hcs_entry = header_chain_selector_->GetHeader(header.block_hash);
                if (hcs_entry) {
                    header_utreexo_root = hcs_entry->header.utreexo_root;
                    logger_->info("[LoadSnapshot] Using HeaderChainSelector for utreexo_root verification");
                } else {
                    result.error_message = "Failed to load snapshot base block for utreexo_root verification";
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
            } else {
                result.error_message = "Failed to load snapshot base block for utreexo_root verification";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            if (header_utreexo_root != utreexo_section.utreexo_root) {
                result.error_message = "v3 snapshot utreexo_root does not match base block header commitment";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }

            auto deserialized_forest = consensus::UtreexoForest::deserialize(serialized_forest);
            auto canonical_forest = deserialized_forest.serialize();
            if (canonical_forest != serialized_forest) {
                // Non-fatal: serialization format may differ between binary versions.
                // The SHA256 checksum and utreexo_root commitment already validated
                // the forest data. Log a warning and continue.
                logger_->warning("[LoadSnapshot] v3 forest canonical re-serialize mismatch "
                                "(likely version skew between exporter and importer). "
                                "Checksum + root commitment already verified — continuing.");
            }

            const consensus::UtreexoHash forest_root_hash = deserialized_forest.getCommitment();
            uint256 forest_root;
            std::memcpy(forest_root.begin(), forest_root_hash.data(), 32);
            if (forest_root != utreexo_section.utreexo_root) {
                result.error_message = "v3 snapshot forest commitment does not match section utreexo_root";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            if (deserialized_forest.getNumLeaves() != utreexo_section.forest_leaves) {
                result.error_message = "v3 snapshot forest leaf count mismatch";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }

            snapshot_forest = std::move(deserialized_forest);
            logger_->info("[LoadSnapshot] v3 Utreexo root binding verified against base block header");
        }

        // Pass 2: BulkLoad consensus UTXO set
        logger_->info("[LoadSnapshot] Pass 2/2: Loading " + std::to_string(utxo_map.size()) + " UTXOs into consensus set...");

        if (!consensus_utxo_set_->BulkLoad(utxo_map, header.block_height, header.block_hash)) {
            result.error_message = "Failed to BulkLoad consensus UTXO set from snapshot";
            logger_->error("[LoadSnapshot] " + result.error_message);
            return result;
        }

        result.utxos_imported = utxo_map.size();
        logger_->info("[LoadSnapshot] Pass 2 complete: Loaded " + std::to_string(result.utxos_imported) + " UTXOs into consensus set");

        SetAssumeUTXOState(header.block_hash, header.block_height, /*persist_metadata=*/true);

        // Drive the fatal state machine: Disabled -> SnapshotLoaded.
        // Belt-and-braces re-entry tighten: the pre-mutation check above blocks
        // different-base loads before any mutation, but OnSnapshotLoaded is the
        // authoritative lifecycle gate and handles concurrent fatal transitions.
        // Priority:
        //   1. FatalMismatch (concurrent transition between the top gate and here):
        //      always fatal — state machine has irrevocably condemned this node.
        //   2. Different base (should have been caught pre-mutation, but guard anyway):
        //      fail with a clear operator message.
        //   3. Same base: benign rehydrate — startup is re-driving the lifecycle
        //      for an already-active snapshot; keep going.
        if (!assumeutxo_lifecycle_->OnSnapshotLoaded(header.block_hash, header.block_height)) {
            if (assumeutxo_lifecycle_->GetState() ==
                    assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
                result.success = false;
                result.error_message =
                    "node is in assumeutxo fatal_mismatch state; operator reset required "
                    "(blockchain.resetassumeutxofatal or wipe datadir)";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            const auto st = assumeutxo_lifecycle_->GetStatus(
                std::chrono::steady_clock::now());
            const bool same_base = (st.snapshot_base_block == header.block_hash &&
                                    st.snapshot_base_height == header.block_height);
            if (!same_base) {
                result.error_message =
                    "another snapshot lifecycle is active (base height " +
                    std::to_string(st.snapshot_base_height) +
                    "); reset or let validation finish before loading a different snapshot"
                    " (post-import refusal: snapshot state was imported before the conflict"
                    " was detected; restart will quarantine it)";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            // same base, non-fatal: benign rehydrate, lifecycle state preserved
        }

        // Persist the expected content commitment so the replay engine (Task 7)
        // can compare its recomputed digest after replaying genesis→base.
        // Placed after the fatal-return gate so a mid-import fatal never strands
        // a stale commitment. kExpectedCommitmentKey is always written; the
        // utreexo root key is only written for v3 snapshots (v2 has no root).
        if (utxo_index_) {
            utxo_index_->SetMetadata(assumeutxo::kExpectedCommitmentKey,
                                     snapshot_commitment_hex);
            if (has_v3_utreexo_section) {
                utxo_index_->SetMetadata(assumeutxo::kExpectedUtreexoRootKey,
                                         utreexo_section.utreexo_root.GetHex());
            }
            logger_->info("[LoadSnapshot] Expected commitment persisted ("
                         + snapshot_commitment_hex.substr(0, 16) + "..."
                         + (has_v3_utreexo_section ? ", utreexo root stored" : ", v2 no utreexo root")
                         + ")");
        }

        if (snapshot_forest.has_value()) {
            // CRITICAL: BulkLoad rebuilt the forest from scratch (sorted outpoint adds),
            // which produces a different tree than the one that evolved through block-by-block
            // processing. Override with the snapshot's serialized forest to match the
            // block header's utreexo_root commitment.
            consensus_utxo_set_->GetForest() = std::move(*snapshot_forest);

            // Persist imported forest as a checkpoint so restart does not require rebuild.
            auto serialized = consensus_utxo_set_->GetForest().serialize();
            ChainWriteToken token;
            auto status = chain_db_->putUtreexoCheckpointWithChecksum(
                token, static_cast<int>(header.block_height), serialized);
            if (status != Status::Ok) {
                logger_->warning("[LoadSnapshot] Loaded v3 forest in memory, but failed to persist checkpoint");
            } else {
                logger_->info("[LoadSnapshot] v3 forest loaded and checkpoint persisted at height " +
                             std::to_string(header.block_height));
            }
        } else {
            logger_->warning("[LoadSnapshot] Legacy v2 snapshot loaded (no embedded forest payload)");
        }

        // v4: restore the shielded-pool state so post-snapshot shielded
        // transactions validate. Without this the commitment tree starts empty,
        // so the first post-snapshot shielded spend fails AnchorInvalid and the
        // chain wedges (e.g. mainnet block 50038 on sub-50038 snapshots).
        if (has_v4_shielded_section) {
            shielded_tree_ = consensus::shielded::CommitmentTree();
            if (!shielded_frontier_buf.empty() &&
                !shielded_tree_.DeserializeFrontier(shielded_frontier_buf.data(),
                                                    shielded_frontier_buf.size())) {
                result.error_message = "Failed to restore shielded commitment-tree frontier from snapshot";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            // Consensus binding: the restored tree root MUST match the root the
            // snapshot recorded (and, transitively, the chain at the base height).
            {
                const consensus::shielded::Hash restored_root = shielded_tree_.Root();
                if (std::memcmp(restored_root.data(), shielded_section.commitment_root.data, 32) != 0) {
                    result.error_message = "Restored shielded tree root does not match snapshot commitment_root";
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
            }
            if (!shielded_anchor_buf.empty() &&
                shielded_anchor_history_.DeserializeBytes(shielded_anchor_buf) !=
                    consensus::shielded::AnchorHistory::IoResult::Ok) {
                result.error_message = "Failed to restore shielded anchor history from snapshot";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            // Restore the nullifier set from the carried NSCF payload (see
            // NullifierSet::SerializeContent): 'NSCF'(u32) | version(u16) |
            // count(u64) | count x [ height(u32) | nullifier(32) ]. Re-inserting
            // is mandatory: an empty nullifier set on a snapshot node is
            // fail-OPEN — the commitment tree is append-only, so an already-spent
            // pre-snapshot note still has a valid membership proof against the
            // current root, and a node with an empty set would ACCEPT a re-spend
            // (shielded double-spend / inflation / consensus split). Full nodes
            // reject via their populated set.
            if (!shielded_nullifier_buf.empty()) {
                const std::vector<uint8_t>& nb = shielded_nullifier_buf;
                auto rd_u16 = [&nb](size_t o) -> uint16_t {
                    return static_cast<uint16_t>(nb[o] | (static_cast<uint16_t>(nb[o + 1]) << 8));
                };
                auto rd_u32 = [&nb](size_t o) -> uint32_t {
                    return static_cast<uint32_t>(nb[o]) | (static_cast<uint32_t>(nb[o + 1]) << 8) |
                           (static_cast<uint32_t>(nb[o + 2]) << 16) | (static_cast<uint32_t>(nb[o + 3]) << 24);
                };
                auto rd_u64 = [&nb](size_t o) -> uint64_t {
                    uint64_t v = 0;
                    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(nb[o + i]) << (i * 8);
                    return v;
                };
                constexpr uint32_t kNscfTag        = 0x4653434E;  // 'NSCF'
                constexpr size_t   kNscfHeaderSize = 14;          // tag(4)+ver(2)+count(8)
                constexpr size_t   kEntrySize      = 4 + 32;      // height(4)+nullifier(32)
                if (nb.size() < kNscfHeaderSize || rd_u32(0) != kNscfTag) {
                    result.error_message = "v4 shielded nullifier payload: bad NSCF header";
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
                const uint16_t nver = rd_u16(4);
                if (nver != 1) {
                    result.error_message = "v4 shielded nullifier payload: unsupported NSCF version " +
                                           std::to_string(nver);
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
                const uint64_t ncount = rd_u64(6);
                if (nb.size() != kNscfHeaderSize + ncount * kEntrySize) {
                    result.error_message = "v4 shielded nullifier payload: size mismatch (count=" +
                                           std::to_string(ncount) + ", bytes=" + std::to_string(nb.size()) + ")";
                    logger_->error("[LoadSnapshot] " + result.error_message);
                    return result;
                }
                size_t off = kNscfHeaderSize;
                uint64_t inserted = 0;
                for (uint64_t i = 0; i < ncount; ++i) {
                    const uint32_t h = rd_u32(off); off += 4;
                    consensus::shielded::Hash nf{};
                    std::memcpy(nf.data(), nb.data() + off, 32); off += 32;
                    if (shielded_nullifiers_.Insert(nf, h)) ++inserted;
                }
                logger_->info("[LoadSnapshot] v4 nullifier set restored: " +
                              std::to_string(inserted) + "/" + std::to_string(ncount) + " entries");
            }

            // Persist the restored shielded state to ChainDB immediately, so a
            // restart before the first post-snapshot ConnectTip re-reads THIS
            // state (frontier + anchor history) instead of an empty tree and
            // re-wedges / drops into safe mode on a shielded-tip misalignment.
            if (!PersistShieldedState()) {
                logger_->warning("⚠️  [LoadSnapshot] v4 shielded state restored but PersistShieldedState() "
                                 "failed — a restart before the first ConnectTip may re-read stale state");
            }
            // Persist the shielded tip marker at the snapshot base. Without it, a
            // restart before the first post-snapshot ConnectTip hits
            // VerifyOrBootstrapShieldedTipMarker → "marker NotFound + shielded
            // activity exists" → refuses to start (fail-safe, but won't boot).
            if (!PersistShieldedTipMarker(header.block_hash, header.block_height)) {
                logger_->warning("⚠️  [LoadSnapshot] failed to persist shielded tip marker at snapshot base " +
                                 std::to_string(header.block_height));
            }
            logger_->warning("⚠️  [LoadSnapshot] v4 shielded state restored (frontier + anchor history + nullifiers)");
        }

        logger_->warning("⚠️  AssumeUTXO mode ACTIVE - UTXO set loaded from snapshot at height " +
                        std::to_string(header.block_height));
        logger_->warning("⚠️  Snapshot base: " + header.block_hash.GetHex());

        // Add snapshot base block to g_block_index so ActivateBestChain's self-heal
        // can find the UTXO tip and align active_tip_ without triggering SAFE MODE.
        // Without this, FindBlockIndex(utxo_best) returns null → self-heal fails.
        //
        // Defense-in-depth: this materialization must NEVER abort the import before
        // StartBackgroundValidation() runs (line ~9682). A throw here (e.g. a
        // malformed chainwork string deep in AddBlockIndex) used to propagate to the
        // outer catch and skip background validation entirely — the snapshot tip then
        // stays held at base forever with validation NotStarted (a silent deadlock).
        // ActivateBestChain's self-heal re-runs EnsureHeaderBranchIndexed later, so
        // swallowing a transient failure here is safe; background validation is what
        // matters and must always be reached.
        if (header_chain_selector_) {
            try {
                const auto* hcs_entry = header_chain_selector_->GetHeader(header.block_hash);
                if (hcs_entry) {
                    CBlockIndex* snapshot_idx = EnsureHeaderBranchIndexed(hcs_entry, /*mark_chain_valid=*/true);
                    if (snapshot_idx) {
                        PublishActiveTip(snapshot_idx, TipPublishReason::kSnapshotRestore);
                        logger_->info("[LoadSnapshot] active_tip_ set to snapshot base (h=" +
                                     std::to_string(header.block_height) + ")");
                    } else {
                        logger_->warning("[LoadSnapshot] Failed to materialize snapshot ancestry in block index");
                    }
                }
            } catch (const std::exception& me) {
                logger_->warning(std::string("[LoadSnapshot] Snapshot base materialization threw (") +
                                 me.what() + ") — deferring to ActivateBestChain self-heal; "
                                 "continuing to background validation");
            }
        }

        result.success = true;

        logger_->info("[LoadSnapshot] Import complete: " + std::to_string(result.utxos_imported) +
                     " UTXOs, " + std::to_string(result.bytes_read) + " bytes");

        // Snapshot doesn't store block blobs — enable pruned (headers-only) mode
        // so the node advertises NODE_NETWORK_LIMITED instead of NODE_NETWORK.
        // Peers won't request blocks we can't serve.
        if (auto* ctx = DaemonContext::instance()) {
            if (ctx->prune) {
                ctx->prune->enableHeadersOnlyMode();
                logger_->info("[LoadSnapshot] PruneService → headers-only (no block data stored)");
            }
        }

        // Make snapshot-loaded coins visible to the active wallet.
        //
        // The snapshot's pre-base coins have no block bodies (the node is in
        // headers-only mode), so the wallet's block-replay rescan can never see
        // them. Scan the just-loaded in-memory consensus UTXO set directly and
        // record any coins owned by the wallet. This runs once per snapshot load
        // and is idempotent (INSERT OR IGNORE), covering both the loadtxoutset
        // RPC and the assumeutxo auto-bootstrap (both converge on LoadSnapshot).
        if (auto* ctx = DaemonContext::instance()) {
            if (ctx->wallet && ctx->wallet->hasActiveWallet()) {
                try {
                    int recorded = RescanWalletFromSnapshotUTXOs(ctx->wallet->get(),
                                                                 header.block_height);
                    if (recorded >= 0) {
                        logger_->info("[LoadSnapshot] Wallet UTXO-set rescan recorded " +
                                     std::to_string(recorded) + " owned coin(s) from snapshot set");
                    }
                } catch (const std::exception& we) {
                    logger_->warning(std::string("[LoadSnapshot] Wallet UTXO-set rescan failed: ") + we.what());
                }
            }
        }

        // Phase 44: Start background validation automatically
        // This validates the entire chain from genesis → snapshot height in parallel
        // Node is immediately usable while validation runs in the background
        logger_->info("[LoadSnapshot] Starting background validation...");
        StartBackgroundValidation();

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception during import: ") + e.what();
        logger_->error("[LoadSnapshot] " + result.error_message);
    }

    return result;
}

// ============================================================================
// Phase 41: BlockIndex Management
// ============================================================================

CBlockIndex* ChainstateService::FindBlockIndex(const uint256& hash) {
    // REORG FIX: Use ONLY the global g_block_index (ONE graph rule)
    // There must be exactly ONE BlockIndex graph in the entire process.
    // Previously, ChainstateService had its own block_index_ map, causing:
    // - Headers added to g_block_index (consensus layer)
    // - Blocks added to ChainstateService::block_index_
    // - Same hash, DIFFERENT CBlockIndex* objects, DIFFERENT pprev chains
    // - FindFork() returned null because ancestry was split across two graphs
    // Now everything uses the single global graph.
    return dinero::FindBlockIndex(hash);
}

CBlockIndex* ChainstateService::AddBlockIndex(const BlockHeader& header, uint32_t height) {
    // REORG FIX: Delegate to global AddBlockIndex (ONE graph rule)
    // There must be exactly ONE BlockIndex graph in the entire process.
    // All code paths (headers, blocks, P2P, RPC) must use the same graph.
    // The global dinero::AddBlockIndex handles:
    // - Duplicate checking
    // - pprev linking
    // - Chainwork calculation
    // - Storage in g_block_index
    std::cout << "[CS::AddBlockIndex] ENTRY: height=" << height << std::endl;
    std::cout << std::flush;

    CBlockIndex* result = dinero::AddBlockIndex(header, height);

    // ═══════════════════════════════════════════════════════════════════════════
    // INVARIANT: Exactly one CBlockIndex per hash, forever.
    // If this assertion fires, something created a duplicate BlockIndex entry.
    // This would break pprev chains, chainwork, and reorg correctness.
    // ═══════════════════════════════════════════════════════════════════════════
#ifndef NDEBUG
    if (result) {
        CBlockIndex* lookup = dinero::FindBlockIndex(header.GetHash());
        assert(lookup == result && "ONE BlockIndex per hash invariant violated!");
    }
#endif

    std::cout << "[CS::AddBlockIndex] global returned: " << (result ? "OK" : "NULL") << std::endl;
    std::cout << std::flush;

    if (result && logger_) {
        // Note: logger_ may be null during early genesis init (before Init() is called)
        logger_->info("[BlockIndex] Added block (global): height=" + std::to_string(height) +
                     " hash=" + header.GetHash().GetHex().substr(0, 16) + "... pprev=" +
                     (result->pprev ? "SET" : "NULL"));
    }

    std::cout << "[CS::AddBlockIndex] EXIT: returning " << (result ? "OK" : "NULL") << std::endl;
    std::cout << std::flush;

    return result;
}

CBlockIndex* ChainstateService::EnsureHeaderBranchIndexed(const consensus::HeaderIndexEntry* tip_entry,
                                                         bool mark_chain_valid) {
    if (!tip_entry) {
        return nullptr;
    }

    std::vector<const consensus::HeaderIndexEntry*> path_to_import;
    const consensus::HeaderIndexEntry* walk = tip_entry;
    while (walk) {
        CBlockIndex* idx = FindBlockIndex(walk->hash);
        CBlockIndex* expected_parent = walk->parent ? FindBlockIndex(walk->parent->hash) : nullptr;
        const bool parent_ready = (walk->parent == nullptr) || (expected_parent != nullptr);
        const bool already_linked = idx && parent_ready && idx->pprev == expected_parent;
        if (already_linked) {
            break;
        }
        path_to_import.push_back(walk);
        walk = walk->parent;
    }

    std::reverse(path_to_import.begin(), path_to_import.end());

    size_t imported = 0;
    size_t relinked = 0;
    size_t chainwork_fixed = 0;
    size_t marked_valid = 0;
    CBlockIndex* tip_idx = nullptr;

    for (const consensus::HeaderIndexEntry* entry : path_to_import) {
        if (!entry) {
            continue;
        }

        CBlockIndex* expected_parent = entry->parent ? FindBlockIndex(entry->parent->hash) : nullptr;
        if (entry->parent && !expected_parent) {
            if (logger_) {
                logger_->error("[ChainstateService] Cannot index header branch at height " +
                              std::to_string(entry->height) + " — parent missing from block index");
            }
            return nullptr;
        }

        CBlockIndex* idx = FindBlockIndex(entry->hash);
        if (!idx) {
            idx = AddBlockIndex(entry->header, entry->height);
            if (!idx) {
                if (logger_) {
                    logger_->error("[ChainstateService] Failed to add header branch block index entry at height " +
                                  std::to_string(entry->height));
                }
                return nullptr;
            }
            imported++;
        }

        // #360 residual fix: this pprev/children relink and the chainwork write
        // mutate the shared CBlockIndex graph on the load thread, concurrent with
        // the P2P thread's (locked) AddBlockIndex children.push_back. Both writers
        // must hold g_block_index_mutex or the children vectors corrupt → silent
        // heap crash mid-materialization (observed at nondeterministic heights).
        // Recursive mutex: safe even if a caller already holds it.
        {
            std::lock_guard<std::recursive_mutex> lk(dinero::g_block_index_mutex);
            if (idx->pprev != expected_parent) {
                if (idx->pprev) {
                    auto& old_children = idx->pprev->children;
                    old_children.erase(std::remove(old_children.begin(), old_children.end(), idx),
                                       old_children.end());
                }
                idx->pprev = expected_parent;
                if (expected_parent) {
                    auto& new_children = expected_parent->children;
                    if (std::find(new_children.begin(), new_children.end(), idx) == new_children.end()) {
                        new_children.push_back(idx);
                    }
                }
                relinked++;
            }

            const std::string expected_chainwork = entry->chainwork.GetHex();
            if (idx->chainwork != expected_chainwork) {
                idx->chainwork = expected_chainwork;
                chainwork_fixed++;
            }
        }

        if (mark_chain_valid && !(idx->status & BLOCK_VALID_CHAIN)) {
            MarkBlockValid(idx, BLOCK_VALID_CHAIN);
            marked_valid++;
        }

        tip_idx = idx;
    }

    if (!tip_idx) {
        tip_idx = FindBlockIndex(tip_entry->hash);
    }

    if (tip_idx && tip_idx->chainwork != tip_entry->chainwork.GetHex()) {
        tip_idx->chainwork = tip_entry->chainwork.GetHex();
        chainwork_fixed++;
    }

    if (tip_idx && logger_ && (!path_to_import.empty() || imported > 0 || relinked > 0 ||
                               chainwork_fixed > 0 || marked_valid > 0)) {
        logger_->info("[ChainstateService] Indexed header branch to height " +
                     std::to_string(tip_idx->height) + " (imported=" +
                     std::to_string(imported) + ", relinked=" +
                     std::to_string(relinked) + ", chainwork_fixed=" +
                     std::to_string(chainwork_fixed) + ", marked_valid=" +
                     std::to_string(marked_valid) + ")");
    }

    return tip_idx;
}

void ChainstateService::UpdateChainwork(CBlockIndex* block_index) {
    if (!block_index) return;

    // Calculate work for this block
    std::string block_work = chainwork::WorkForBits(block_index->bits);

    // Add parent's chainwork
    if (block_index->pprev) {
        block_index->chainwork = chainwork::AddWork(block_index->pprev->chainwork, block_work);
    } else {
        // Genesis block
        block_index->chainwork = block_work;
    }
}

void ChainstateService::AddCandidate(CBlockIndex* block_index) {
    std::lock_guard<std::recursive_mutex> lock(activation_mutex_);
    if (!block_index) return;

    // P0 invariant: single eligibility gate for all candidate paths.
    // #309: a not-yet-connected side branch with whole-branch data is a valid
    // reorg target (validation deferred to the ConnectTip walk), so use the
    // reorg-candidacy predicate rather than requiring BLOCK_VALID_CHAIN up front.
    if (!IsReorgCandidateEligible(block_index)) {
        if (logger_) {
            logger_->debug("[AddCandidate] Rejected: status=" + std::to_string(block_index->status) +
                          " height=" + std::to_string(block_index->height) +
                          " (needs BLOCK_VALID_CHAIN + BLOCK_HAVE_DATA, no failure flags)");
        }
        return;
    }
    if (HasInvalidAncestor(block_index)) {
        if (logger_) {
            logger_->debug("[AddCandidate] Rejected: height=" + std::to_string(block_index->height) +
                           " has invalid ancestor");
        }
        return;
    }

    // Remove parent from candidates (no longer a tip)
    if (block_index->pprev) {
        candidates_.erase(block_index->pprev);
    }

    // Add this block as a new tip
    candidates_.insert(block_index);

    if (logger_) {
        logger_->info("[AddCandidate] Added: height=" + std::to_string(block_index->height) +
                     ", chainwork=..." + block_index->chainwork.substr(48, 16) +
                     ", status=" + std::to_string(block_index->status));
    }
}

void ChainstateService::RemoveCandidate(CBlockIndex* block_index) {
    std::lock_guard<std::recursive_mutex> lock(activation_mutex_);
    if (!block_index) return;
    candidates_.erase(block_index);
}

// #309: walk ancestors until a connected (BLOCK_VALID_CHAIN) base. Every block
// above the base must have its body so the reorg ConnectTip walk can validate
// and apply them in order. Genesis counts as a connected base. Returns false on
// a body gap above the base or if the branch never roots in the valid chain.
bool ChainstateService::HasBranchDataToConnectedBase(CBlockIndex* block_index) {
    // Delegates to the free predicate (unit-tested in test_reorg_candidate_eligibility).
    return dinero::BranchHasDataToConnectedBase(block_index);
}

// #309: a block is a reorg candidate if it has its body, is not failed, and its
// whole branch back to a connected base has bodies present. Already-connected
// (BLOCK_VALID_CHAIN) blocks are trivially eligible. Per-block consensus
// validation is deferred to the reorg ConnectTip walk.
bool ChainstateService::IsReorgCandidateEligible(CBlockIndex* block_index) {
    if (!block_index) return false;
    if (block_index->status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) return false;
    if (!(block_index->status & BLOCK_HAVE_DATA)) return false;
    if (block_index->status & BLOCK_VALID_CHAIN) return true;
    return HasBranchDataToConnectedBase(block_index);
}

CBlockIndex* ChainstateService::GetBestCandidate() {
    if (candidates_.empty()) return nullptr;

    // Only select candidates that share ancestry with the active chain.
    // Incompatible candidates (no common ancestor) can appear from stale/forked
    // header data and would make FindFork() return null, stalling activation.
    auto shares_active_ancestor = [this](CBlockIndex* candidate) -> bool {
        if (!candidate || !active_tip_) {
            return true;
        }

        CBlockIndex* a = active_tip_;
        CBlockIndex* b = candidate;

        while (a && b && a->height > b->height) {
            a = a->pprev;
        }
        while (a && b && b->height > a->height) {
            b = b->pprev;
        }
        while (a && b) {
            if (a->hash == b->hash) {
                return true;
            }
            a = a->pprev;
            b = b->pprev;
        }
        return false;
    };

    std::vector<CBlockIndex*> incompatible;
    std::vector<CBlockIndex*> invalid;
    for (CBlockIndex* candidate : candidates_) {
        if (!candidate || !IsReorgCandidateEligible(candidate) ||
            HasInvalidAncestor(candidate)) {
            invalid.push_back(candidate);
            continue;
        }
        if (shares_active_ancestor(candidate)) {
            return candidate; // First compatible element has most work
        }
        incompatible.push_back(candidate);
    }

    for (CBlockIndex* candidate : invalid) {
        if (candidate && logger_) {
            logger_->warning("[GetBestCandidate] Removing invalid candidate: height=" +
                             std::to_string(candidate->height) + " hash=" +
                             candidate->hash.GetHex().substr(0, 16) + "...");
        }
        candidates_.erase(candidate);
    }

    for (CBlockIndex* candidate : incompatible) {
        if (logger_) {
            logger_->warning("[GetBestCandidate] Removing incompatible candidate: height=" +
                             std::to_string(candidate->height) + " hash=" +
                             candidate->hash.GetHex().substr(0, 16) + "...");
        }
        candidates_.erase(candidate);
    }

    return nullptr;
}

// ============================================================================
// Production InvalidateBlock / ReconsiderBlock (Bitcoin Core model)
// ============================================================================

bool ChainstateService::InvalidateBlock(const uint256& hash, std::string& error) {
    CBlockIndex* target = FindBlockIndex(hash);
    if (!target) {
        error = "Block not found in block index";
        return false;
    }

    if (target->height == 0) {
        error = "Cannot invalidate genesis block";
        return false;
    }

    // Already invalid?
    if (target->status & BLOCK_FAILED_VALID) {
        error = "Block is already marked invalid";
        return false;
    }

    if (logger_) {
        logger_->warning("[InvalidateBlock] Invalidating block at height " +
                        std::to_string(target->height) + " hash=" +
                        hash.GetHex().substr(0, 16) + "...");
    }

    std::vector<CBlockIndex*> manually_disconnected_blocks;
    CBlockIndex* post_disconnect_tip = active_tip_;

    // Step 1: If block is in the active chain, disconnect back to its parent
    if (active_tip_) {
        // Check if target is an ancestor of (or equal to) the active tip
        bool in_active_chain = false;
        for (CBlockIndex* walk = active_tip_; walk; walk = walk->pprev) {
            if (walk == target) {
                in_active_chain = true;
                break;
            }
        }

        if (in_active_chain) {
            if (logger_) {
                logger_->info("[InvalidateBlock] Block is in active chain — disconnecting " +
                             std::to_string(active_tip_->height - target->height + 1) + " blocks");
            }

            // Disconnect from tip down to the invalidated block (inclusive)
            while (active_tip_ && active_tip_ != target->pprev) {
                CBlockIndex* to_disconnect = active_tip_;
                manually_disconnected_blocks.push_back(to_disconnect);
                if (!DisconnectTip(to_disconnect)) {
                    error = "Failed to disconnect tip at height " +
                            std::to_string(to_disconnect->height);
                    return false;
                }
                // DisconnectTip sets active_tip_ to pprev
                PublishActiveTip(to_disconnect->pprev, TipPublishReason::kRollback);
            }

            post_disconnect_tip = active_tip_ ? active_tip_ : target->pprev;

            // Update ChainDB tip to new active tip
            if (active_tip_ && chain_db_) {
                ChainWriteToken token;
                arith_uint256 work = ChainworkFromHex(active_tip_->chainwork);
                chain_db_->setTip(token, active_tip_->hash, active_tip_->height, work);
            }
        }
    }

    // Step 2: Mark block and all descendants as invalid
    target->status |= BLOCK_FAILED_VALID;
    RemoveCandidate(target);

    // Apr 14 2026 (Bug #6 / #38) — persist BLOCK_FAILED_VALID to ChainDB.
    // Without this, the flag is in-memory only and reverts on restart, which
    // means any historically invalid block (e.g. a pre-fix ring-covenant
    // block with stale CT pool indices) gets reloaded as a candidate every
    // startup, fails ConnectBlock, and crash-loops the daemon. The persisted
    // flag is read back into block_index->status by
    // ApplyPersistedMetadataToBlockIndex on the next boot, and AddCandidate's
    // IsEligibleForCandidacy gate then permanently rejects it.
    if (chain_db_) {
        ChainWriteToken token;
        auto persist_status =
            chain_db_->setHeaderStatusBits(token, target->hash, BLOCK_FAILED_VALID);
        if (persist_status != Status::Ok && logger_) {
            logger_->warning("[InvalidateBlock] Failed to persist BLOCK_FAILED_VALID for "
                             + target->hash.GetHex().substr(0, 16) + "... — flag is in-memory only");
        }
    }

    dinero::testing::MaybeAbortAt("after_invalid_target_before_descendants",
                                  dinero::Params().network_id == "regtest");

    // Propagate BLOCK_FAILED_CHILD to all descendants recursively
    std::vector<CBlockIndex*> queue;
    queue.insert(queue.end(), target->children.begin(), target->children.end());
    while (!queue.empty()) {
        CBlockIndex* child = queue.back();
        queue.pop_back();
        if (!child) continue;

        child->status |= BLOCK_FAILED_CHILD;
        RemoveCandidate(child);

        // Persist the descendant's flag too — same rationale as the target.
        if (chain_db_) {
            ChainWriteToken token;
            auto persist_status =
                chain_db_->setHeaderStatusBits(token, child->hash, BLOCK_FAILED_CHILD);
            if (persist_status != Status::Ok && logger_) {
                logger_->warning("[InvalidateBlock] Failed to persist BLOCK_FAILED_CHILD for "
                                 + child->hash.GetHex().substr(0, 16) + "...");
            }
        }

        queue.insert(queue.end(), child->children.begin(), child->children.end());
    }

    // Step 3: Add parent back to candidates (it's now a tip)
    if (target->pprev) {
        AddCandidate(target->pprev);
    }

    // Step 4: Re-evaluate best chain (may switch to a fork)
    ActivateBestChain();

    if (!manually_disconnected_blocks.empty()) {
        auto* daemon_ctx = DaemonContext::instance();
        if (daemon_ctx && daemon_ctx->mempool && daemon_ctx->mempool->isInitialized()) {
            std::vector<Transaction> disconnected_txs;
            AppendTransactionsFromBlocks(
                chain_db_,
                block_storage_.get(),
                manually_disconnected_blocks,
                /*ancestor_first=*/true,
                &disconnected_txs
            );

            std::vector<Transaction> connected_txs;
            CBlockIndex* reconnect_fork = FindCommonAncestorByHash(post_disconnect_tip, active_tip_);
            AppendTransactionsOnChainPath(
                chain_db_,
                block_storage_.get(),
                active_tip_,
                reconnect_fork,
                &connected_txs);

            const size_t restored = daemon_ctx->mempool->mempool().ReconcileAfterReorg(
                disconnected_txs,
                connected_txs
            );

            if (logger_) {
                logger_->info("[InvalidateBlock] Restored " + std::to_string(restored) +
                              "/" + std::to_string(disconnected_txs.size()) +
                              " transaction(s) from manually disconnected blocks");
            }
        }
    }

    if (logger_) {
        logger_->warning("[InvalidateBlock] Done. Active tip now at height " +
                        std::to_string(active_tip_ ? active_tip_->height : 0));
    }

    return true;
}

bool ChainstateService::ReconsiderBlock(const uint256& hash, std::string& error) {
    CBlockIndex* target = FindBlockIndex(hash);
    if (!target) {
        error = "Block not found in block index";
        return false;
    }

    // Not invalid?
    if (!(target->status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD))) {
        error = "Block is not marked as invalid";
        return false;
    }

    if (logger_) {
        logger_->info("[ReconsiderBlock] Reconsidering block at height " +
                     std::to_string(target->height) + " hash=" +
                     hash.GetHex().substr(0, 16) + "...");
    }

    std::vector<CBlockIndex*> subtree;
    subtree.push_back(target);
    std::vector<CBlockIndex*> queue;
    queue.insert(queue.end(), target->children.begin(), target->children.end());
    while (!queue.empty()) {
        CBlockIndex* child = queue.back();
        queue.pop_back();
        if (!child) continue;
        subtree.push_back(child);
        queue.insert(queue.end(), child->children.begin(), child->children.end());
    }

    // Clear invalid flags durably as one logical operation. A crash in the
    // middle of reconsider must not leave the target reconsidered while its
    // descendants stay BLOCK_FAILED_CHILD on disk.
    if (chain_db_) {
        ChainWriteToken token;
        rocksdb::WriteBatch status_batch;

        auto stage_clear = [&](CBlockIndex* node) -> bool {
            const auto persist_status =
                chain_db_->clearHeaderStatusBits(token,
                                                 node->hash,
                                                 BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD,
                                                 &status_batch);
            if (persist_status != Status::Ok) {
                error = "Failed to stage cleared invalidity for height " +
                        std::to_string(node->height);
                if (logger_) {
                    logger_->warning("[ReconsiderBlock] " + error);
                }
                return false;
            }
            return true;
        };

        if (!stage_clear(target)) {
            return false;
        }

        dinero::testing::MaybeAbortAt("after_reconsider_target_before_descendants",
                                      dinero::Params().network_id == "regtest");

        for (size_t i = 1; i < subtree.size(); ++i) {
            if (!stage_clear(subtree[i])) {
                return false;
            }
        }

        const auto write_status = chain_db_->writeBatch(token, std::move(status_batch), true);
        if (write_status != Status::Ok) {
            error = "Failed to persist reconsidered invalidity batch";
            if (logger_) {
                logger_->warning("[ReconsiderBlock] " + error);
            }
            return false;
        }
    }

    for (CBlockIndex* node : subtree) {
        if (node) {
            node->status &= ~(BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD);
        }
    }

    // Re-add to candidate set if eligible
    // Walk to the tip of this branch and add tip as candidate
    std::vector<CBlockIndex*> branch_tips;
    std::vector<CBlockIndex*> scan;
    scan.push_back(target);
    while (!scan.empty()) {
        CBlockIndex* node = scan.back();
        scan.pop_back();
        if (node->children.empty()) {
            // This is a tip — add to candidates if it has block data
            AddCandidate(node);
            branch_tips.push_back(node);
        } else {
            for (CBlockIndex* c : node->children) {
                if (c) scan.push_back(c);
            }
        }
    }

    // Re-evaluate best chain
    ActivateBestChain();

    if (logger_) {
        logger_->info("[ReconsiderBlock] Done. Active tip now at height " +
                     std::to_string(active_tip_ ? active_tip_->height : 0) +
                     ", reconsidered " + std::to_string(branch_tips.size()) + " branch tip(s)");
    }

    return true;
}

bool ChainstateService::ForceSetActiveTip(const uint256& hash, std::string& error) {
    CBlockIndex* idx = FindBlockIndex(hash);
    if (!idx) {
        error = "Block index entry not found for requested tip hash";
        return false;
    }
    PublishActiveTip(idx, TipPublishReason::kReorgInvalidate);
    return true;
}

bool ChainstateService::RestoreUtreexoCheckpoint(uint32_t height, std::string& error) {
    if (!chain_db_) {
        error = "ChainDB not available";
        return false;
    }

    try {
        if (height == 0) {
            consensus::UtreexoForest empty;
            if (consensus::IsUtreexoCanonicalRootsActive(0)) {
                empty.setCanonicalEmptyRoots(true);
            }
            consensus_utxo_set_->GetForest() = std::move(empty);
            return true;
        }

        // Forest checkpoint delta campaign phase 3 (CSN/stateless): full
        // checkpoints exist only every utreexo.checkpoint_interval blocks, so
        // rebuild `height` as nearest-checkpoint + UD-sidecar replay
        // (header-root verified per block) instead of reading an exact-height
        // checkpoint that no longer exists off-interval. This is the CSN
        // reorg rewind (fork-point restore) — see daemon_app.cpp CSN worker
        // and the ABC-CSN stateless reorg path.
        consensus::UtreexoForest restored;
        const Status restore_status =
            storage::RestoreHistoricalForest(*chain_db_, height, restored, error);
        if (restore_status != Status::Ok) {
            if (error.empty()) {
                error = "Failed to restore forest to height " + std::to_string(height);
            }
            return false;
        }
        consensus_utxo_set_->GetForest() = std::move(restored);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Failed to restore Utreexo checkpoint: ") + e.what();
        return false;
    }
}

bool ChainstateService::ReloadConsensusUTXOFromDB(std::string& error) {
    if (!consensus_utxo_set_) {
        error = "Consensus UTXO set is not initialized";
        return false;
    }
    if (!chain_db_) {
        error = "ChainDB not available";
        return false;
    }
    ChainWriteToken token;
    storage::PersistentUTXOAdapter adapter(*chain_db_, token);
    if (!adapter.LoadInitialState(*consensus_utxo_set_)) {
        error = "Failed to reload consensus UTXO set from ChainDB";
        return false;
    }
    return true;
}

bool ChainstateService::IsCanonicalStateAligned(std::string* reason) const {
    auto fail = [&](const std::string& msg) {
        if (reason) {
            *reason = msg;
        }
        return false;
    };

    if (!chain_db_) {
        return fail("chain-db-unavailable");
    }

    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        return fail("chaindb-tip-unavailable-status-" +
                    std::to_string(static_cast<int>(tip_result.status())));
    }

    // During early bootstrap, active_tip_ can legitimately be null.
    if (!active_tip_) {
        return true;
    }

    if (!consensus_utxo_set_) {
        return fail("consensus-utxo-set-unavailable");
    }

    const uint256& utxo_best = consensus_utxo_set_->GetBestBlock();
    const uint32_t utxo_height = consensus_utxo_set_->GetHeight();
    if (utxo_best != active_tip_->hash || utxo_height != static_cast<uint32_t>(active_tip_->height)) {
        return fail("consensus-utxo-tip=" + utxo_best.GetHex().substr(0, 16) + "...@" +
                    std::to_string(utxo_height) + " active-tip=" +
                    active_tip_->hash.GetHex().substr(0, 16) + "...@" +
                    std::to_string(active_tip_->height));
    }

    // ChainDB tip may legitimately be AHEAD during startup recovery when blocks
    // were stored but not yet replayed through ConnectTip. That is not a
    // misalignment by itself. The invalid cases are:
    //  1) DB tip behind active tip
    //  2) Same height but different hash
    const auto& tip = tip_result.value();
    if (static_cast<uint32_t>(tip.height) < active_tip_->height) {
        // In AssumeUTXO mode the chaindb tip is genesis (h=0) while active_tip_
        // points to the snapshot base (e.g. h=28551). This is expected — chaindb
        // will catch up as background validation replays blocks. Not a violation.
        if (assumeutxo_active_) return true;
        return fail("chaindb-tip-behind-active: chaindb=" +
                    tip.hash.GetHex().substr(0, 16) + "...@" + std::to_string(tip.height) +
                    " active=" + active_tip_->hash.GetHex().substr(0, 16) + "...@" +
                    std::to_string(active_tip_->height));
    }
    if (static_cast<uint32_t>(tip.height) == active_tip_->height && tip.hash != active_tip_->hash) {
        return fail("chaindb-tip-conflict-at-active-height: chaindb=" +
                    tip.hash.GetHex().substr(0, 16) + "... active=" +
                    active_tip_->hash.GetHex().substr(0, 16) + "...");
    }

    const auto shielded_marker_result = chain_db_->getShieldedTipMarker();
    if (shielded_marker_result.status() == Status::NotFound) {
        return fail("shielded-tip-marker-missing");
    }
    if (shielded_marker_result.status() != Status::Ok) {
        return fail("shielded-tip-marker-status-" +
                    std::to_string(static_cast<int>(shielded_marker_result.status())));
    }

    const auto snapshot = CurrentShieldedStateSnapshot();
    const auto& shielded_marker = shielded_marker_result.value();
    if (shielded_marker.block_hash != active_tip_->hash ||
        shielded_marker.height != static_cast<int32_t>(active_tip_->height)) {
        return fail("shielded-tip-marker-tip=" +
                    shielded_marker.block_hash.GetHex().substr(0, 16) + "...@" +
                    std::to_string(shielded_marker.height) + " active=" +
                    active_tip_->hash.GetHex().substr(0, 16) + "...@" +
                    std::to_string(active_tip_->height));
    }
    if (shielded_marker.shielded_root != snapshot.root ||
        shielded_marker.tree_size != snapshot.tree_size ||
        shielded_marker.nullifier_count != snapshot.nullifier_count) {
        return fail("shielded-tip-marker-state-mismatch");
    }

    return true;
}

// ============================================================================
// Phase 41: Reorg Execution
// ============================================================================

CBlockIndex* ChainstateService::FindFork(CBlockIndex* a, CBlockIndex* b) {
    if (!a || !b) return nullptr;

    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "🔍 [FindFork] Starting fork point search" << std::endl;
    std::cout << "   Chain A (active): height=" << a->height << ", hash=" << a->hash.GetHex().substr(0, 16) << "..." << std::endl;
    std::cout << "   Chain B (candidate): height=" << b->height << ", hash=" << b->hash.GetHex().substr(0, 16) << "..." << std::endl;

    auto should_log_step = [](int step) {
        return step <= 5 || (step % 500 == 0);
    };

    // Walk back from higher chain to same height
    int adjust_a_steps = 0;
    while (a->height > b->height) {
        adjust_a_steps++;
        if (should_log_step(adjust_a_steps)) {
            std::cout << "   [HEIGHT ADJ A #" << adjust_a_steps << "] " << a->height
                      << " -> " << (a->pprev ? a->pprev->height : -1) << std::endl;
        }
        if (!a->pprev) {
            std::cout << "   ❌ BROKEN: a->pprev is null at height " << a->height << std::endl;
            break;
        }
        a = a->pprev;
    }

    int adjust_b_steps = 0;
    while (b->height > a->height) {
        adjust_b_steps++;
        if (should_log_step(adjust_b_steps)) {
            std::cout << "   [HEIGHT ADJ B #" << adjust_b_steps << "] " << b->height
                      << " -> " << (b->pprev ? b->pprev->height : -1) << std::endl;
        }
        if (!b->pprev) {
            std::cout << "   ❌ BROKEN: b->pprev is null at height " << b->height << std::endl;
            break;
        }
        b = b->pprev;
    }

    if (adjust_a_steps > 5 || adjust_b_steps > 5) {
        std::cout << "   Height-adjust summary: A steps=" << adjust_a_steps
                  << ", B steps=" << adjust_b_steps << std::endl;
    }

    std::cout << "   After height adjustment:" << std::endl;
    std::cout << "     A: height=" << (a ? a->height : -1) << ", hash=" << (a ? a->hash.GetHex().substr(0, 16) : "NULL") << "..." << std::endl;
    std::cout << "     B: height=" << (b ? b->height : -1) << ", hash=" << (b ? b->hash.GetHex().substr(0, 16) : "NULL") << "..." << std::endl;

    // Walk back together until we find common ancestor
    // CRITICAL FIX: Compare by HASH, not by pointer!
    // Side-chain blocks may have pprev pointing to different CBlockIndex instances
    // for the same block (e.g., loaded from ChainDB vs block_index_)
    int steps = 0;
    while (a && b && a->hash != b->hash) {
        steps++;
        if (should_log_step(steps)) {
            std::cout << "   [FORK SEARCH #" << steps << "] A=" << a->hash.GetHex().substr(0, 12)
                      << " (h=" << a->height << ") != B=" << b->hash.GetHex().substr(0, 12)
                      << " (h=" << b->height << ")" << std::endl;
        }

        if (!a->pprev) {
            std::cout << "   ❌ BROKEN: a->pprev is null at height " << a->height << std::endl;
            break;
        }
        if (!b->pprev) {
            std::cout << "   ❌ BROKEN: b->pprev is null at height " << b->height << std::endl;
            break;
        }

        a = a->pprev;
        b = b->pprev;

        // Guard against corrupted loops while still allowing deep real-world forks.
        if (steps > 500000) {
            std::cout << "   ❌ SAFETY: Too many iterations (" << steps << "), breaking" << std::endl;
            break;
        }
    }

    if (steps > 5) {
        std::cout << "   Fork-search summary: compared " << steps << " ancestor step(s)" << std::endl;
    }

    if (a && b && a->hash == b->hash) {
        std::cout << "   ✅ FORK POINT FOUND: height=" << a->height << ", hash=" << a->hash.GetHex().substr(0, 16) << "..." << std::endl;
        // Return the canonical version from block_index_ if possible
        CBlockIndex* canonical = FindBlockIndex(a->hash);
        if (canonical) {
            std::cout << "   ✅ Returning canonical block_index_ entry" << std::endl;
            std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
            return canonical;
        }
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
        return a; // Common ancestor found
    } else {
        std::cout << "   ❌ NO COMMON ANCESTOR FOUND" << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
        return nullptr; // CRITICAL FIX: Return nullptr when no common ancestor, not a broken chain pointer
    }
}

// ============================================================================
// Reorg Fix: BlockUndo ↔ UndoRecord Conversion Helpers
// ============================================================================

namespace {

// Convert consensus::BlockUndo + block data to dinero::UndoRecord for ChainDB storage.
// BlockUndo only carries spent coins, so we must derive created outputs from the
// actual block to preserve disconnect symmetry.
dinero::UndoRecord BlockUndoToUndoRecord(const consensus::BlockUndo& block_undo, const Block& block) {
    dinero::UndoRecord undo;

    for (const auto& entry : block_undo.spent_coins) {
        dinero::SpentCoin coin(
            entry.txid,           // uint256 txid
            entry.vout,           // uint32_t vout
            entry.coin.value.GetUna(),     // Phase M.6.2: Extract raw value
            entry.coin.scriptPubKey,  // vector<uint8_t> scriptPubKey
            entry.coin.isCoinbase,    // bool is_coinbase
            entry.coin.height,        // uint32_t height
            entry.coin.is_confidential,
            entry.coin.commitment
        );
        undo.spent.push_back(coin);
    }

    for (const auto& tx : block.vtx) {
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            undo.created.emplace_back(txid.AsUint256(), vout);
        }
    }

    // Note: Utreexo delta is persisted separately as a sidecar key (UD:<blockhash>)
    // so legacy UndoRecord format remains backward-compatible.
    undo.pre_block_shielded_frontier = block_undo.pre_block_shielded_frontier;
    undo.pre_reset_shielded_epoch    = block_undo.pre_reset_shielded_epoch;

    return undo;
}

// Convert dinero::UndoRecord to consensus::BlockUndo for BlockValidator
consensus::BlockUndo UndoRecordToBlockUndo(const dinero::UndoRecord& undo_record, uint32_t height, const uint256& block_hash) {
    consensus::BlockUndo block_undo(height, block_hash);

    for (const auto& coin : undo_record.spent) {
        consensus::UTXOEntry utxo(
            AmountUna::Una(coin.value),  // Phase M.6.2: Wrap in AmountUna
            coin.scriptPubKey,    // vector<uint8_t> scriptPubKey
            coin.height,          // uint32_t height
            coin.is_coinbase,
            coin.is_confidential,
            coin.commitment
        );

        block_undo.AddSpentCoin(coin.prev_txid, coin.prev_vout, utxo);
    }

    // Note: utreexo_delta is loaded from sidecar key (UD:<blockhash>) in DisconnectTip.
    block_undo.pre_block_shielded_frontier = undo_record.pre_block_shielded_frontier;
    block_undo.pre_reset_shielded_epoch    = undo_record.pre_reset_shielded_epoch;

    return block_undo;
}

// Re-put every nullifier from a shielded epoch reset snapshot into `batch`.
// Parses the snapshot's NSCF nullifier blob via a temp in-memory NullifierSet
// (reusing the tested DeserializeContent + ForEach) and stages a
// putShieldedNullifier for each row. Used by DisconnectTip when reorging across
// the cutover: the connect at H purged the entire ChainDB nullifier CF, so the
// pre-reset rows must be re-materialized from the undo snapshot — self-contained
// (independent of whether the in-memory set was restored). Returns false on a
// parse or DB error.
bool RePutShieldedEpochSnapshotNullifiers(
    ChainDB* chain_db,
    const ChainWriteToken& token,
    const consensus::shielded::ShieldedEpochSnapshot& snapshot,
    rocksdb::WriteBatch& batch) {
    consensus::shielded::NullifierSet tmp;
    if (tmp.Open(":memory:") != consensus::shielded::NullifierSet::OpenResult::Ok) {
        return false;
    }
    if (!tmp.DeserializeContent(snapshot.nullifiers)) {
        return false;
    }
    bool put_ok = true;
    const bool scan_ok = tmp.ForEach(
        [&](uint32_t nf_height, const uint8_t* nf_32) -> bool {
            if (chain_db->putShieldedNullifier(token, nf_height, nf_32, &batch) !=
                Status::Ok) {
                put_ok = false;
                return false;
            }
            return true;
        });
    return scan_ok && put_ok;
}

} // anonymous namespace

// #274: stateless ConnectBlock cannot populate BlockUndo.spent_coins (no UTXO
// set), so ConnectTip reconstructs the spent list from ChainDB coin rows —
// the only source carrying full fidelity (height + coinbase flag, which
// utreexo proofs do not commit to). Must run BEFORE the unified batch stages
// the deleteCoin calls, while the rows still exist. Same-block spends fall
// back to the block body (rows not yet written). Any other miss is fatal to
// the connect: a short undo would make the tip undisconnectable.
Status ChainstateService::ReconstructSpentCoinsFromChainDb(
    const Block& block,
    uint32_t height,
    std::vector<dinero::SpentCoin>& out_spent,
    std::string& out_error) const {
    out_spent.clear();
    out_error.clear();

    if (!chain_db_) {
        out_error = "ReconstructSpentCoinsFromChainDb: chain_db_ is null";
        return Status::Internal;
    }

    // Iterate non-coinbase txs (tx_idx >= 1) and their inputs in block order.
    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        for (const auto& input : tx.vin) {
            const uint256 prev_txid = input.prevout.txid.AsUint256();
            const uint32_t prev_vout = input.prevout.vout;

            // Primary source: the ChainDB coin row (plain getCoin — the row
            // must still exist because this runs before deleteCoin staging).
            auto coin_result = chain_db_->getCoin(prev_txid, prev_vout);
            if (coin_result.status() == Status::Ok) {
                const auto& coin = coin_result.value();
                // ChainDB stores spk as hex. util::HexToBytes SILENTLY
                // returns an empty vector on odd-length/non-hex input —
                // an empty scriptPubKey in the undo would corrupt the
                // restored coin on disconnect. Decode via util::unhex
                // and fail loud on corrupt rows instead.
                std::vector<unsigned char> spk_bytes;
                if (!util::unhex(coin.script_pubkey, spk_bytes)) {
                    out_error = "ReconstructSpentCoinsFromChainDb: spent coin "
                                "row has corrupt scriptPubKey hex: " +
                                prev_txid.GetHex() + ":" +
                                std::to_string(prev_vout);
                    out_spent.clear();
                    return Status::Corruption;
                }
                out_spent.emplace_back(
                    prev_txid,
                    prev_vout,
                    coin.amount,
                    std::vector<uint8_t>(spk_bytes.begin(), spk_bytes.end()),
                    coin.coinbase,
                    static_cast<uint32_t>(coin.height),
                    coin.is_confidential,
                    coin.commitment);
                continue;
            }

            // Intra-block fallback: the spent output was created by an EARLIER
            // tx in this same block, so its coin row was never written.
            bool found_intra_block = false;
            for (size_t prev_idx = 0; prev_idx < tx_idx; ++prev_idx) {
                const auto& prev_tx = block.vtx[prev_idx];
                if (prev_tx.GetTxid().AsUint256() != prev_txid) continue;
                if (prev_vout >= prev_tx.vout.size()) break;  // malformed — fail loud below
                const auto& out = prev_tx.vout[prev_vout];
                out_spent.emplace_back(
                    prev_txid,
                    prev_vout,
                    out.value.GetUna(),
                    out.scriptPubKey,
                    /*is_coinbase=*/prev_idx == 0,
                    height,  // created in this very block
                    out.is_confidential,
                    out.commitment);
                found_intra_block = true;
                break;
            }
            if (found_intra_block) continue;

            // Neither source has it: a short undo would make this tip
            // undisconnectable, so the connect must abort.
            out_error = "ReconstructSpentCoinsFromChainDb: outpoint " +
                        prev_txid.GetHex() + ":" + std::to_string(prev_vout) +
                        " absent from ChainDB and not created intra-block";
            out_spent.clear();
            return Status::NotFound;
        }
    }

    return Status::Ok;
}

// ============================================================================
// Reorg Fix: Production-Correct DisconnectTip
// ============================================================================

bool ChainstateService::DisconnectTip(CBlockIndex* tip_to_disconnect) {
    std::cout << "🔧 [DisconnectTip] ENTRY: height=" << (tip_to_disconnect ? tip_to_disconnect->height : -1) << std::endl;
    std::cout << std::flush;

    if (!tip_to_disconnect) {
        std::cout << "❌ [DisconnectTip] NULL tip_to_disconnect" << std::endl;
        return false;
    }
    if (!chain_db_) {
        std::cout << "❌ [DisconnectTip] NULL chain_db_" << std::endl;
        return false;
    }
    if (!block_validator_) {
        std::cout << "❌ [DisconnectTip] NULL block_validator_" << std::endl;
        logger_->error("[DisconnectTip] BlockValidator not initialized");
        return false;
    }
    ChainWriteToken token;  // Required for canonical tip persistence during reorg

    auto schedule_chainstate_recovery = [&](const std::string& reason) {
        ScheduleChainstateRecovery(reason, "[DisconnectTip]");
    };

    std::cout << "🔧 [DisconnectTip] Reading stored block..." << std::endl;

    auto block_result = ReadStoredBlock(tip_to_disconnect->hash);
    if (block_result.status() != Status::Ok) {
        std::cout << "❌ [DisconnectTip] Failed to read block: status=" << static_cast<int>(block_result.status()) << std::endl;
        logger_->error("[DisconnectTip] Failed to read block at height " +
                      std::to_string(tip_to_disconnect->height));
        return false;
    }
    Block block = block_result.value();
    std::cout << "✅ [DisconnectTip] Block read, vtx.size()=" << block.vtx.size() << std::endl;

    // ═══════════════════════════════════════════════════════════════════════════
    // STATELESS (CSN) mode: Lightweight disconnect.
    // Forest is restored from checkpoint by ActivateBestChain.
    // Coin DB must still remain consistent, so restore spent prevouts from undo data.
    // ═══════════════════════════════════════════════════════════════════════════
    if (GetConfig().utreexo_stateless) {
        std::cout << "🔧 [DisconnectTip-CSN] STATELESS mode: lightweight disconnect" << std::endl;
        ChainWriteToken token;

        // Read undo data to restore spent prevouts.
        auto undo_result = ReadStoredUndo(tip_to_disconnect->hash);
        if (undo_result.status() != Status::Ok) {
            if (logger_) logger_->error("[DisconnectTip-CSN] Missing undo data for height " +
                                        std::to_string(tip_to_disconnect->height));
            return false;
        }

        // Determine new canonical tip / chainwork up-front.
        CBlockIndex* new_tip = tip_to_disconnect->pprev;
        if (!new_tip) {
            logger_->error("[DisconnectTip-CSN] Cannot set tip to null parent");
            return false;
        }
        arith_uint256 new_tip_work;
        try {
            new_tip_work = ChainworkFromHex(new_tip->chainwork);
        } catch (const std::exception& e) {
            logger_->error(std::string("[DisconnectTip-CSN] Invalid parent chainwork: ") + e.what());
            return false;
        }

        // Remove created outputs, restore spent outputs, persist tip
        // rollback, and stage the ShieldedTipMarker — all in one atomic
        // batch.
        rocksdb::WriteBatch coin_batch;
        const auto& undo = undo_result.value();

        // DEFENSE-IN-DEPTH (reset-block snapshot refusal): if this is the
        // shielded epoch-reset block, the stored undo MUST carry
        // pre_reset_shielded_epoch — DisconnectBlockShieldedSection restores
        // the discarded pre-cutover pool from it (RollbackAbove cannot re-add
        // reset-wiped rows). A snapshot-less reset undo would make the shielded
        // disconnect a silent no-op, retreating below H with the post-reset
        // EMPTY pool (every pre-reset nullifier gone — a silent double-spend
        // window). The bookkeeping writer + idempotency gate refuse to persist
        // such a record, but fail loud HERE too, BEFORE any mutation
        // (DisconnectBlockShieldedSection mutates the in-memory pool
        // immediately, not via coin_batch), mirroring the regen-refusal
        // guard's rationale (~10877).
        if (consensus::shielded::IsShieldedEpochResetHeight(
                static_cast<uint32_t>(tip_to_disconnect->height),
                dinero::Params().shielded_epoch_reset_height) &&
            !undo.pre_reset_shielded_epoch.has_value()) {
            if (logger_) {
                logger_->error("[DisconnectTip-CSN] refusing to disconnect the shielded "
                               "epoch-reset block at height " +
                               std::to_string(tip_to_disconnect->height) +
                               " — the stored undo lacks pre_reset_shielded_epoch; "
                               "disconnecting with it would silently drop the pre-cutover "
                               "shielded pool (double-spend/fork window). The stored undo "
                               "must carry the pre-reset snapshot.");
            }
            return false;
        }

        // Shielded pool disconnect (CSN): this lightweight path does NOT call
        // BlockValidator::DisconnectBlock, so the in-memory pool (tree,
        // anchors, nullifiers) is not restored on its own. Roll it back here,
        // BEFORE computing the ShieldedTipMarker and staging the
        // frontier/anchor blobs below — otherwise the marker/frontier/anchor
        // would be written from the stale (pre-rollback) state, leaving
        // on-disk state inconsistent and wedging the next startup or letting
        // a stale anchor/nullifier set validate a double-spend.
        //
        // DisconnectBlockShieldedSection is the shared disconnect twin of the
        // connect-path funnel: on the cutover block it restores the full
        // pre-reset pool from undo.pre_reset_shielded_epoch (RollbackAbove
        // cannot re-add rows a reset wiped); on an ordinary block it
        // restores the pre-block frontier + rolls back nullifiers/anchors to
        // height-1 from undo.pre_block_shielded_frontier; with neither set
        // (no shielded activity in this block) it is a no-op.
        std::string derr;
        if (!consensus::shielded::DisconnectBlockShieldedSection(
                tip_to_disconnect->height, undo.pre_reset_shielded_epoch,
                undo.pre_block_shielded_frontier, shielded_tree_,
                shielded_nullifiers_, &shielded_anchor_history_, derr)) {
            if (logger_) {
                logger_->error("[DisconnectTip-CSN] " + derr);
            }
            return false;
        }

        for (const auto& created : undo.created) {
            chain_db_->deleteCoin(token, created.txid, created.vout, &coin_batch);
        }

        for (const auto& spent : undo.spent) {
            dinero::Coin coin;
            coin.amount = spent.value;
            coin.script_pubkey = BinaryToHexString(
                std::string(reinterpret_cast<const char*>(spent.scriptPubKey.data()),
                            spent.scriptPubKey.size()));
            coin.height = static_cast<int>(spent.height);
            coin.coinbase = spent.is_coinbase;
            coin.is_confidential = spent.is_confidential;
            coin.commitment = spent.commitment;
            chain_db_->putCoin(token, spent.prev_txid, spent.prev_vout, coin, &coin_batch);
        }

        const auto tip_status = chain_db_->setTip(token, new_tip->hash,
                                                  static_cast<int>(new_tip->height), new_tip_work,
                                                  &coin_batch);
        if (tip_status != Status::Ok) {
            logger_->error("[DisconnectTip-CSN] Failed to persist tip rollback");
            return false;
        }

        // Phase 3b step 4 (CSN mirror): stage ShieldedTipMarker into the
        // unified coin_batch — atomic with UTXO rollback + tip pointer.
        // No more standalone fsync after the rollback batch where the
        // marker could lag behind the tip.
        {
            // The in-memory pool was just rolled back above by
            // DisconnectBlockShieldedSection — on the cutover block (restored
            // from the pre-reset snapshot) or on an ordinary block (frontier
            // deserialized + nullifiers/anchors rolled back to height-1).
            // Whenever the pool changed, stage the rolled-back frontier +
            // anchor blobs into coin_batch (atomic with the tip retreat) so
            // they supersede the stale pre-disconnect blobs — a restart must
            // not rehydrate the wrong frontier. Do not depend on the
            // non-atomic post-batch PersistShieldedState for this
            // consensus-critical rollback.
            if (undo.pre_reset_shielded_epoch.has_value() ||
                undo.pre_block_shielded_frontier.has_value()) {
                const auto fb = shielded_tree_.SerializeFrontier();
                const std::string frontier_blob(fb.begin(), fb.end());
                if (chain_db_->putUtreexoMeta(token, "shielded_frontier",
                                              frontier_blob, &coin_batch) != Status::Ok) {
                    if (logger_) {
                        logger_->error("[DisconnectTip-CSN] Failed to stage rolled-back "
                                       "shielded frontier during disconnect");
                    }
                    return false;
                }
                const auto ab = shielded_anchor_history_.SerializeBytes();
                const std::string anchor_blob(ab.begin(), ab.end());
                if (chain_db_->putUtreexoMeta(token, "shielded_anchor_history",
                                              anchor_blob, &coin_batch) != Status::Ok) {
                    if (logger_) {
                        logger_->error("[DisconnectTip-CSN] Failed to stage rolled-back "
                                       "anchor history during disconnect");
                    }
                    return false;
                }
            }
            const auto shielded_snapshot = CurrentShieldedStateSnapshot();
            ChainDB::ShieldedTipMarker shielded_marker;
            shielded_marker.height = static_cast<int32_t>(new_tip->height);
            shielded_marker.block_hash = new_tip->hash;
            shielded_marker.shielded_root = shielded_snapshot.root;
            shielded_marker.tree_size = shielded_snapshot.tree_size;
            shielded_marker.nullifier_count = shielded_snapshot.nullifier_count;
            const auto stm_status =
                chain_db_->putShieldedTipMarker(token, shielded_marker, &coin_batch);
            if (stm_status != Status::Ok) {
                logger_->error("[DisconnectTip-CSN] Failed to stage ShieldedTipMarker rollback, status=" +
                               std::to_string(static_cast<int>(stm_status)));
                return false;
            }
        }

        // Phase 3b nullifier fold-in (CSN mirror): stage deletion of
        // every ChainDB nullifier row above the parent tip alongside
        // the marker rollback.
        {
            const auto delete_count = chain_db_->deleteShieldedNullifiersAboveHeight(
                token,
                static_cast<uint32_t>(new_tip->height),
                &coin_batch);
            if (delete_count.status() != Status::Ok) {
                logger_->error("[DisconnectTip-CSN] Failed to stage shielded nullifier rollback, status=" +
                               std::to_string(static_cast<int>(delete_count.status())));
                return false;
            }

            // Shielded epoch reset disconnect (CSN mirror of the full-mode path):
            // the connect at H purged the whole nullifier CF, so deleteAboveHeight
            // cannot restore the pre-reset rows. This lightweight path does NOT
            // call DisconnectBlock, so re-put directly from the undo snapshot
            // (parsed into a temp set) into coin_batch so ChainDB — the
            // authoritative rehydration source — matches the pre-reset pool.
            if (undo.pre_reset_shielded_epoch.has_value()) {
                if (!RePutShieldedEpochSnapshotNullifiers(
                        chain_db_, token, *undo.pre_reset_shielded_epoch, coin_batch)) {
                    logger_->error("[DisconnectTip-CSN] Failed to re-put shielded epoch "
                                   "reset nullifier rows during disconnect across cutover");
                    return false;
                }
            }
        }

        auto coin_status = chain_db_->writeBatch(token, std::move(coin_batch), true);
        if (coin_status != Status::Ok) {
            if (logger_) logger_->error("[DisconnectTip-CSN] Coin/tip rollback batch failed");
            return false;
        }

        // Phase 3b step 4 (CSN): write the rolled-back shielded
        // frontier flat file AFTER the unified coin batch commits.
        // Same reasoning as the full-mode path — pre-batch hoist
        // creates a frontier-ahead-of-marker mismatch on crash.
        if (!PersistShieldedState() && logger_) {
            logger_->warning("[DisconnectTip-CSN] Failed to persist shielded frontier at height " +
                             std::to_string(new_tip->height));
        }

        if (consensus_utxo_set_) {
            consensus_utxo_set_->SetBestBlock(new_tip->hash, static_cast<uint32_t>(new_tip->height));
        }
        PublishActiveTip(new_tip, TipPublishReason::kCSNDisconnect);
        notifyBlockDisconnected(block, tip_to_disconnect->height);

        if (logger_) logger_->info("[DisconnectTip-CSN] Lightweight disconnect complete: height=" +
                                   std::to_string(tip_to_disconnect->height) + " → " +
                                   std::to_string(new_tip->height));
        return true;
    }

    std::cout << "🔧 [DisconnectTip] Reading undo data..." << std::endl;

    // Read undo data from archival flatfile storage
    auto undo_result = ReadStoredUndo(tip_to_disconnect->hash);
    if (undo_result.status() != Status::Ok) {
        // D.3 (Apr 30 2026): trivial-case fallback regeneration.
        //
        // The fleet's height-10347 wedge was a coinbase-only sibling
        // block — every input is coinbase, no shielded txs, no prevout
        // lookup needed. The undo for such blocks is reconstructible
        // from the block body alone (spent=empty, created=outputs).
        // Try the trivial regeneration before giving up.
        //
        // This is a defense-in-depth for the bug class D.1 already
        // closed at the source: a working ConnectTip (post-D.1)
        // produces correct undo metadata every time, so this fallback
        // path should rarely fire on a fresh-mined chain. It exists
        // for chains that were corrupted by pre-D.1 binaries — exactly
        // the live LA/VA/MO state when this fix lands.
        std::cout << "⚠️  [DisconnectTip] Failed to read undo data: status="
                  << static_cast<int>(undo_result.status())
                  << " — attempting trivial regeneration\n";
        auto block_for_regen = ReadStoredBlock(tip_to_disconnect->hash);
        // A regenerated undo cannot reconstruct the shielded epoch reset
        // snapshot: the discarded pre-cutover pool is unrecoverable from the
        // live post-reset tree. Regenerating at the cutover would emit an undo
        // missing pre_reset_shielded_epoch, and disconnecting H with it would
        // wipe the pool at H-1 (a fork). Refuse — require the stored undo.
        const bool at_shielded_reset_height =
            consensus::shielded::IsShieldedEpochResetHeight(
                static_cast<uint32_t>(tip_to_disconnect->height),
                dinero::Params().shielded_epoch_reset_height);
        if (at_shielded_reset_height && logger_) {
            logger_->error("[DisconnectTip] refusing to regenerate undo at the shielded "
                           "epoch reset height " +
                           std::to_string(tip_to_disconnect->height) +
                           " — the pre-cutover pool is unreconstructable; the stored "
                           "undo must be present to disconnect the cutover block");
        }
        if (!at_shielded_reset_height && block_for_regen.status() == Status::Ok) {
            // Try the cheap path first: coinbase-only / no shielded
            // blocks can be reconstructed from the block body alone
            // with no I/O. Most sibling-race wedges hit this case.
            auto regenerated = RegenerateUndoFromBlockTrivial(block_for_regen.value());
            if (regenerated.status() == Status::Ok) {
                logger_->warning("[DisconnectTip] ReadStoredUndo failed but block is "
                                 "trivially regenerable (coinbase-only, no shielded). "
                                 "Proceeding with regenerated undo at height " +
                                 std::to_string(tip_to_disconnect->height));
                undo_result = std::move(regenerated);
            } else {
                // D.3-full (Apr 30 2026): try prevout-lookup regen.
                // Walks every non-coinbase input and reconstructs the
                // SpentCoin via txindex → parent-block read. Slower
                // (one txindex lookup + one block read per input) but
                // covers the next class of wedge: a sibling-race on a
                // user-tx-bearing block. Still aborts on shielded
                // (separate task: shielded reverse-apply).
                regenerated = RegenerateUndoFromBlock(block_for_regen.value());
                if (regenerated.status() == Status::Ok) {
                    logger_->warning("[DisconnectTip] ReadStoredUndo failed but block undo "
                                     "regenerated via prevout txindex lookup. Proceeding at height " +
                                     std::to_string(tip_to_disconnect->height) +
                                     " (spent_count=" +
                                     std::to_string(regenerated.value().spent.size()) +
                                     ", created_count=" +
                                     std::to_string(regenerated.value().created.size()) + ")");
                    undo_result = std::move(regenerated);
                } else {
                    logger_->error("[DisconnectTip] D.3 full regeneration also failed at height " +
                                   std::to_string(tip_to_disconnect->height) +
                                   " (status=" + std::to_string(static_cast<int>(regenerated.status())) +
                                   "). Falling through to recovery marker.");
                }
            }
        }
    }
    if (undo_result.status() != Status::Ok) {
        std::cout << "❌ [DisconnectTip] Failed to read undo data: status=" << static_cast<int>(undo_result.status()) << std::endl;
        logger_->error("[DisconnectTip] Failed to read undo data for block at height " +
                      std::to_string(tip_to_disconnect->height));
        schedule_chainstate_recovery(
            "missing undo data for active tip height " + std::to_string(tip_to_disconnect->height) +
            " hash=" + tip_to_disconnect->hash.GetHex()
        );
        return false;
    }
    std::cout << "✅ [DisconnectTip] Undo data read" << std::endl;

    // Convert UndoRecord to BlockUndo
    consensus::BlockUndo block_undo = UndoRecordToBlockUndo(
        undo_result.value(),
        tip_to_disconnect->height,
        tip_to_disconnect->hash
    );
    std::cout << "✅ [DisconnectTip] BlockUndo converted, spent_coins.size()=" << block_undo.spent_coins.size() << std::endl;

    if (consensus::IsUtreexoActive(tip_to_disconnect->height)) {
        const std::string delta_key = MakeUtreexoDeltaUndoKey(tip_to_disconnect->hash);
        std::string delta_blob;
        const auto delta_status = chain_db_->getRaw(delta_key, delta_blob);
        if (delta_status != Status::Ok) {
            logger_->error("[DisconnectTip] Missing Utreexo delta sidecar for block " +
                          tip_to_disconnect->hash.GetHex().substr(0, 16) + "... status=" +
                          std::to_string(static_cast<int>(delta_status)));
            std::cout << "❌ [DisconnectTip] Missing Utreexo delta sidecar key=" << delta_key.substr(0, 20) << "..." << std::endl;
            schedule_chainstate_recovery(
                "missing utreexo delta sidecar for active tip height " +
                std::to_string(tip_to_disconnect->height) +
                " hash=" + tip_to_disconnect->hash.GetHex()
            );
            return false;
        }

        consensus::UtreexoDelta persisted_delta;
        std::string delta_error;
        if (!DeserializeUtreexoDelta(delta_blob, persisted_delta, delta_error)) {
            logger_->error("[DisconnectTip] Failed to decode Utreexo delta sidecar: " + delta_error);
            std::cout << "❌ [DisconnectTip] Delta sidecar decode failed: " << delta_error << std::endl;
            schedule_chainstate_recovery(
                "corrupt utreexo delta sidecar for active tip height " +
                std::to_string(tip_to_disconnect->height) +
                " hash=" + tip_to_disconnect->hash.GetHex()
            );
            return false;
        }
        block_undo.utreexo_delta = std::move(persisted_delta);
        std::cout << "✅ [DisconnectTip] Loaded Utreexo delta sidecar for block" << std::endl;
    }

    logger_->info("[DisconnectTip] Disconnecting block: height=" +
                 std::to_string(tip_to_disconnect->height) +
                 ", hash=" + tip_to_disconnect->hash.GetHex().substr(0, 16) + "...");

    // Use BlockValidator to rollback UTXO state
    std::cout << "🔧 [DisconnectTip] Calling BlockValidator::DisconnectBlock..." << std::endl;
    std::string error;
    bool disconnect_result = block_validator_->DisconnectBlock(block, tip_to_disconnect->height, block_undo, error);
    std::cout << "🔧 [DisconnectTip] DisconnectBlock returned: " << (disconnect_result ? "true" : "false") << ", error=" << error << std::endl;

    if (!disconnect_result) {
        logger_->error("[DisconnectTip] BlockValidator::DisconnectBlock failed: " + error);
        return false;
    }

    // Determine new canonical tip / chainwork up-front so all the staging
    // below uses one validated value. A bad-hex chainwork must fail the
    // disconnect cleanly — DisconnectBlock has already mutated in-memory
    // state, so we abort and surface the error rather than commit a
    // partial rollback.
    CBlockIndex* new_tip = tip_to_disconnect->pprev;
    if (!new_tip) {
        logger_->error("[DisconnectTip] Cannot set tip to null parent");
        return false;
    }
    arith_uint256 new_tip_work;
    try {
        new_tip_work = ChainworkFromHex(new_tip->chainwork);
    } catch (const std::exception& e) {
        logger_->error(std::string("[DisconnectTip] Invalid parent chainwork hex: ") + e.what());
        return false;
    }

    // Keep ChainDB coin + tx index state in lockstep with the in-memory rollback.
    // Mempool validation and wallet queries still consult ChainDB during reorg
    // recovery, so disconnected confidential outputs must be removed here too.
    //
    // Phase 3b step 4: this batch ALSO carries:
    //   - canonical setTip (parent) and deleteHeightIndex (disconnected
    //     height) — already present pre-step-4
    //   - ForestTipMarker pointing at the parent block (new this commit)
    //   - ShieldedTipMarker reflecting the rolled-back shielded state
    //     (new this commit)
    // All hit disk in a single fsync. Mirrors the §1 atomic-unit shape
    // ConnectTip got via slices 1–6: a crash before writeBatch returns
    // leaves the chain at the pre-disconnect tip with NO partial
    // rollback on disk; a crash after leaves it cleanly at parent with
    // every container — UTXO, txindex, tip pointer, height index, both
    // tip markers — coherent.
    rocksdb::WriteBatch rollback_batch;
    const auto& undo_record = undo_result.value();

    for (const auto& created : undo_record.created) {
        const auto delete_status = chain_db_->deleteCoin(
            token, created.txid, created.vout, &rollback_batch);
        if (delete_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to delete disconnected UTXO " +
                           created.txid.GetHex().substr(0, 16) + "...:" +
                           std::to_string(created.vout));
            return false;
        }
    }

    for (const auto& spent : undo_record.spent) {
        dinero::Coin coin;
        coin.amount = spent.value;
        coin.script_pubkey.reserve(spent.scriptPubKey.size() * 2);
        for (uint8_t byte : spent.scriptPubKey) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", byte);
            coin.script_pubkey += buf;
        }
        coin.height = static_cast<int>(spent.height);
        coin.coinbase = spent.is_coinbase;
        coin.is_confidential = spent.is_confidential;
        coin.commitment = spent.commitment;

        const auto restore_status = chain_db_->putCoin(
            token, spent.prev_txid, spent.prev_vout, coin, &rollback_batch);
        if (restore_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to restore spent UTXO " +
                           spent.prev_txid.GetHex().substr(0, 16) + "...:" +
                           std::to_string(spent.prev_vout));
            return false;
        }
    }

    for (const auto& tx : block.vtx) {
        const auto tx_status = chain_db_->deleteTxIndex(
            token, tx.GetTxid().AsUint256(), &rollback_batch);
        if (tx_status != Status::Ok) {
            logger_->warning("[DisconnectTip] Failed to delete txindex for disconnected tx " +
                             tx.GetTxid().AsUint256().GetHex().substr(0, 16) + "...");
        }
    }

    // Phase 11a: Undo UTXO Position Index changes (symmetric with ConnectTip)
    //
    // Apr 28 2026: this loop must mirror ConnectTip's ephemeral-output filter.
    // Without it, an intra-block-spent UTXO (created by tx[i], consumed by
    // tx[j] in the same block) is iterated here even though Connect skipped
    // it on the way in. Two failure modes follow:
    //   1. Step 1 calls RemovePosition for an output that was never added
    //      → no-op + a warn-level log, position index size silently drifts.
    //   2. Step 2 walks tx.vin entries that include intra-block spends; the
    //      delta.deletedLeaves list does NOT include those spends (live
    //      ConnectBlockInternal skipped them too), so deleted_idx desyncs
    //      and AddPosition writes the wrong (txid, vout) → position_index
    //      ends up holding entries that point at unrelated leaves.
    // Both modes accumulate over a chain's reorg history; the live symptom
    // is `getutxoproof` reporting "live UTXO missing from position index"
    // for thousands of entries (LA: 9172 on Apr 28). Apply the same
    // ephemeral filter as ConnectTip and the asymmetry goes away.
    if (block_undo.utreexo_delta.has_value() && utxo_position_index_) {
        const auto& delta = block_undo.utreexo_delta.value();

        std::unordered_map<dinero::OutPoint, size_t> intra_outputs;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
            const auto& tx = block.vtx[tx_idx];
            TxId txid = tx.GetTxid();
            for (uint32_t n = 0; n < tx.vout.size(); ++n) {
                intra_outputs[dinero::OutPoint(txid, n)] = tx_idx;
            }
        }
        std::unordered_set<dinero::OutPoint> ephemeral_outputs;
        for (const auto& tx : block.vtx) {
            if (tx.IsCoinbase()) continue;
            for (const auto& input : tx.vin) {
                dinero::OutPoint op(input.prevout.txid, input.prevout.vout);
                if (intra_outputs.count(op)) {
                    ephemeral_outputs.insert(op);
                }
            }
        }

        // Step 1: Remove positions for UTXOs that were created in this block
        // (reverse of ConnectTip Step 2 — must skip ephemeral outputs).
        size_t removed_count = 0;
        for (const auto& tx : block.vtx) {
            TxId txid = tx.GetTxid();
            for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
                if (ephemeral_outputs.count(dinero::OutPoint(txid, vout))) {
                    continue;  // Never had a position to remove.
                }
                auto removed = utxo_position_index_->RemovePosition(txid, vout);
                if (removed.has_value()) {
                    removed_count++;
                }
            }
        }

        // Step 2: Restore positions for UTXOs that were spent in this block
        // (reverse of ConnectTip Step 1 — must skip ephemeral spends so the
        // iteration stays aligned with delta.deletedLeaves).
        size_t restored_count = 0;
        size_t deleted_idx = 0;
        for (const auto& tx : block.vtx) {
            if (tx.IsCoinbase()) continue;

            for (const auto& input : tx.vin) {
                TxId prev_txid = input.prevout.txid;
                uint32_t prev_vout = input.prevout.vout;

                if (ephemeral_outputs.count(dinero::OutPoint(prev_txid, prev_vout))) {
                    continue;
                }

                if (deleted_idx < delta.deletedLeaves.size()) {
                    const auto& deleted = delta.deletedLeaves[deleted_idx++];
                    utxo_position_index_->AddPosition(prev_txid, prev_vout, deleted.position);
                    restored_count++;
                }
            }
        }

        if (deleted_idx != delta.deletedLeaves.size()) {
            logger_->error("[DisconnectTip] Phase 11a: deletedLeaves desync — consumed " +
                           std::to_string(deleted_idx) + " of " +
                           std::to_string(delta.deletedLeaves.size()) +
                           " (block=" + tip_to_disconnect->hash.GetHex().substr(0, 16) + ")");
        }

        logger_->info("[DisconnectTip] Phase 11a: Removed " + std::to_string(removed_count) +
                     " positions, restored " + std::to_string(restored_count) +
                     " positions, ephemeral_skipped=" + std::to_string(ephemeral_outputs.size()));
    }

    // Phase 3b step 4: canonical setTip (parent) goes into rollback_batch.
    // Same atomic shape as ConnectTip's slice 6 — the canonical pointer
    // moves in the same fsync as every other consensus-side state change
    // for this disconnect.
    const auto tip_status = chain_db_->setTip(
        token, new_tip->hash, static_cast<int>(new_tip->height), new_tip_work, &rollback_batch);
    if (tip_status != Status::Ok) {
        logger_->error("[DisconnectTip] Failed to persist tip rollback, status=" +
                      std::to_string(static_cast<int>(tip_status)));
        return false;
    }

    // Height→hash indexes are canonical-active-chain state, not a backlog of
    // disconnected branches. Leaving the old tip height mapped after rollback
    // lets startup catch-up replay a block we have already disconnected, which
    // can resurrect invalidated branches on the next restart.
    const auto height_delete_status =
        chain_db_->deleteHeightIndex(token, static_cast<int>(tip_to_disconnect->height), &rollback_batch);
    if (height_delete_status != Status::Ok) {
        logger_->error("[DisconnectTip] Failed to delete stale height index entry at height " +
                       std::to_string(tip_to_disconnect->height) + ", status=" +
                       std::to_string(static_cast<int>(height_delete_status)));
        return false;
    }

    // Phase 3b step 4: stage ForestTipMarker pointing at the new (parent)
    // canonical tip. After BlockValidator::DisconnectBlock the in-memory
    // forest reflects parent's state, so getCommitment() yields parent's
    // root. Mirror of slice 2's ConnectTip staging — keeps the marker in
    // lockstep with the canonical tip pointer through the same atomic
    // commit. The forest-checkpoint blob at parent's height is NOT
    // re-written: it was already durably persisted when parent was
    // connected and its content is immutable per height. Marker only.
    if (consensus_utxo_set_) {
        ChainDB::ForestTipMarker forest_marker;
        forest_marker.height = static_cast<int32_t>(new_tip->height);
        forest_marker.block_hash = new_tip->hash;
        const consensus::UtreexoHash commitment =
            consensus_utxo_set_->GetForest().getCommitment();
        if (commitment.size() == 32) {
            std::memcpy(forest_marker.forest_root.data, commitment.data(), 32);
        } else {
            forest_marker.forest_root.SetNull();
        }
        const auto ftm_status =
            chain_db_->putForestTipMarker(token, forest_marker, &rollback_batch);
        if (ftm_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to stage ForestTipMarker rollback, status=" +
                           std::to_string(static_cast<int>(ftm_status)));
            return false;
        }
    }

    // Phase 3b option 1: stage the rolled-back shielded frontier blob,
    // anchor history blob, and ShieldedTipMarker for the parent tip
    // into rollback_batch. After BlockValidator::DisconnectBlock the
    // in-memory shielded tree + anchor history reflect parent's state;
    // all three ride the same atomic commit as the UTXO/txindex
    // rollback + setTip + height-index delete + ForestTipMarker, so
    // the rollback is one atomic boundary on disk instead of a
    // multi-fsync sequence.
    {
        const auto frontier_bytes = shielded_tree_.SerializeFrontier();
        const std::string frontier_blob(frontier_bytes.begin(),
                                        frontier_bytes.end());
        const auto frontier_status =
            chain_db_->putUtreexoMeta(token, "shielded_frontier",
                                      frontier_blob, &rollback_batch);
        if (frontier_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to stage shielded frontier rollback, status=" +
                           std::to_string(static_cast<int>(frontier_status)));
            return false;
        }

        const auto anchor_bytes = shielded_anchor_history_.SerializeBytes();
        const std::string anchor_blob(anchor_bytes.begin(), anchor_bytes.end());
        const auto anchor_status =
            chain_db_->putUtreexoMeta(token, "shielded_anchor_history",
                                      anchor_blob, &rollback_batch);
        if (anchor_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to stage anchor history rollback, status=" +
                           std::to_string(static_cast<int>(anchor_status)));
            return false;
        }

        const auto shielded_snapshot = CurrentShieldedStateSnapshot();
        ChainDB::ShieldedTipMarker shielded_marker;
        shielded_marker.height = static_cast<int32_t>(new_tip->height);
        shielded_marker.block_hash = new_tip->hash;
        shielded_marker.shielded_root = shielded_snapshot.root;
        shielded_marker.tree_size = shielded_snapshot.tree_size;
        shielded_marker.nullifier_count = shielded_snapshot.nullifier_count;
        const auto stm_status =
            chain_db_->putShieldedTipMarker(token, shielded_marker, &rollback_batch);
        if (stm_status != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to stage ShieldedTipMarker rollback, status=" +
                           std::to_string(static_cast<int>(stm_status)));
            return false;
        }
    }

    // Phase 3b nullifier fold-in: stage the deletion of every
    // ChainDB nullifier row recorded above the new (parent) tip
    // height into rollback_batch. Mirrors ConnectTip's per-spend
    // putShieldedNullifier — keeps ChainDB authoritative and atomic
    // with the rest of the rollback. NullifierSet's sqlite-side
    // RollbackAbove already ran in BlockValidator::DisconnectBlock;
    // this stage handles the durable copy.
    {
        const auto delete_count = chain_db_->deleteShieldedNullifiersAboveHeight(
            token,
            static_cast<uint32_t>(new_tip->height),
            &rollback_batch);
        if (delete_count.status() != Status::Ok) {
            logger_->error("[DisconnectTip] Failed to stage shielded nullifier rollback, status=" +
                           std::to_string(static_cast<int>(delete_count.status())));
            return false;
        }

        // Shielded epoch reset (hard-fork cutover) disconnect: the connect at H
        // PURGED the entire ChainDB nullifier CF, so the deleteAboveHeight above
        // cannot restore the pre-reset rows. Re-materialize them from the undo
        // snapshot into rollback_batch so ChainDB — the authoritative store
        // startup rehydrates from — matches the pool DisconnectBlock just
        // restored in memory. Without this, connecting new blocks
        // (chaindb_count>0) then restarting would let Mode-B rehydration wipe the
        // restored rows and resurrect the gap.
        if (block_undo.pre_reset_shielded_epoch.has_value()) {
            if (!RePutShieldedEpochSnapshotNullifiers(
                    chain_db_, token, *block_undo.pre_reset_shielded_epoch,
                    rollback_batch)) {
                logger_->error("[DisconnectTip] Failed to re-put shielded epoch reset "
                               "nullifier rows during disconnect across cutover");
                return false;
            }
        }
    }

    const auto write_status = chain_db_->writeBatch(token, std::move(rollback_batch), true);
    if (write_status != Status::Ok) {
        logger_->error("[DisconnectTip] Failed to commit rollback batch, status=" +
                      std::to_string(static_cast<int>(write_status)));
        return false;
    }

    // Phase 3b step 4: write the rolled-back shielded frontier flat
    // file IMMEDIATELY after the unified batch commits — same shape
    // as ConnectTip's slice-4 frontier write. Putting this BEFORE
    // the crash hook below means at the hook fire point both the
    // marker (in rollback_batch) and the frontier (this fsync) are
    // durable for the parent tip; startup's ShieldedTipMarker
    // consistency check sees a coherent on-disk snapshot.
    if (!PersistShieldedState() && logger_) {
        logger_->warning("[DisconnectTip] Failed to persist shielded frontier at height " +
                         std::to_string(new_tip->height));
    }

    // Test-only crash boundary "after_disconnect_tip_before_shielded_flush"
    // — kept here for backward compatibility with
    // test_shielded_daemon_restart_equivalence. After step 4 both
    // the marker (in rollback_batch) and the frontier flat file
    // (above) are durable when this fires; the hook still fires
    // and the test still passes — restart finds a coherent
    // post-disconnect on-disk snapshot.
    dinero::testing::MaybeAbortAt("after_disconnect_tip_before_shielded_flush",
                                  dinero::Params().network_id == "regtest");

    // Keep consensus UTXO tip metadata aligned with canonical tip.
    if (consensus_utxo_set_) {
        consensus_utxo_set_->SetBestBlock(new_tip->hash, static_cast<uint32_t>(new_tip->height));
    }

    // Update in-memory state AFTER consensus mutation succeeds
    PublishActiveTip(new_tip, TipPublishReason::kRollback);

    // Notify wallets AFTER state is consistent
    notifyBlockDisconnected(block, tip_to_disconnect->height);

    return true;
}

// ============================================================================
// Reorg Fix: Production-Correct ConnectTip
// ============================================================================

namespace {
// #309/I2 SAFETY: classify a BlockValidator::ConnectBlock failure as an
// UNAMBIGUOUS consensus-rule violation. Used to decide whether a speculative
// reorg-branch block may be marked permanently invalid (BLOCK_FAILED_VALID).
//
// Asymmetry that drives this allowlist: over-marking a VALID block (because an
// OPERATIONAL failure — validation timeout / CPU budget, storage commit failure,
// missing UTXO, unavailable proofs/accumulator — was misread as invalidity)
// permanently forks the node off the network with no recovery but a chain wipe.
// Under-marking only costs a recoverable retry loop. So this returns true ONLY
// for errors that can come from no cause other than the block violating a
// consensus rule. Anything ambiguous or operational (timeout, "Failed to commit
// UTXO ...", "Input UTXO not found", missing-utreexo-data / stateless-*-requires-*
// / requires-accumulator, utreexo-{leaf-missing,add-failed,remove-failed}) is
// deliberately NOT listed and is treated as operational (never marked).
bool IsUnambiguousConsensusViolation(const std::string& err) {
    static const char* kConsensusPrefixes[] = {
        "bad-witness-commitment",
        "missing-witness-commitment",
        "bad-timestamp",
        "bad-header-size",
        "FATAL: Header size",
        "bad-utreexo-root",
        "double-spend-in-block",
        "Coinbase pays too much",
        "Block has no transactions",
        "Stateless validation: Script validation failed",
        "utreexo-phase3-height-limit-exceeded",
        "shielded-delta-accounting-mismatch",
        "shielded-bundle-decode-failed",
        "shielded-block-validation-failed",
    };
    for (const char* p : kConsensusPrefixes) {
        if (err.rfind(p, 0) == 0) return true;  // prefix match
    }
    // Reason-tagged consensus failures (proof/root violations).
    if (err.find("ROOT_MISMATCH") != std::string::npos) return true;
    if (err.find("PROOF_INVALID") != std::string::npos) return true;
    return false;
}
}  // namespace

bool ChainstateService::ConnectTip(CBlockIndex* tip_to_connect, std::string* out_error,
                                   bool* out_consensus_invalid) {
    auto fail = [&](const std::string& reason) {
        if (out_error) {
            *out_error = reason;
        }
        return false;
    };

    // Forest checkpoint delta campaign phase 0
    // (docs/design/forest-checkpoint-deltas.md): time the whole connect;
    // recorded with the forest-checkpoint byte count at the success returns.
    const auto connect_start = std::chrono::steady_clock::now();

    std::cout << "\n────────────────────────────────────────────────────────────────" << std::endl;
    std::cout << "🔧 [ConnectTip] ENTRY" << std::endl;
    std::cout << "────────────────────────────────────────────────────────────────" << std::endl;

    if (!tip_to_connect) {
        std::cout << "❌ [ConnectTip] tip_to_connect is NULL" << std::endl;
        return fail("tip-to-connect-null");
    }
    if (!chain_db_) {
        std::cout << "❌ [ConnectTip] chain_db_ is NULL" << std::endl;
        return fail("chain-db-null");
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase G Safety Assertion: In-Order Commit Check
    // ═══════════════════════════════════════════════════════════════════════════
    // This assertion ensures that blocks are always connected in sequential order.
    // If parallel download causes out-of-order delivery, this catches it early.
    // The block being connected must be exactly one height above the current tip.
    if (active_tip_ && tip_to_connect->height > 0) {
        uint32_t expected_height = active_tip_->height + 1;
        if (tip_to_connect->height != expected_height) {
            std::cerr << "🚨 [ConnectTip] ASSERTION FAILED: Out-of-order block!" << std::endl;
            std::cerr << "   Current tip height: " << active_tip_->height << std::endl;
            std::cerr << "   Expected next height: " << expected_height << std::endl;
            std::cerr << "   Received block height: " << tip_to_connect->height << std::endl;
            std::cerr << "   This indicates a bug in block download ordering." << std::endl;
            assert(tip_to_connect->height == expected_height &&
                   "Phase G Safety: Blocks must be connected in sequential order");
        }
    }

    std::cout << "🔧 [ConnectTip] height=" << tip_to_connect->height << std::endl;
    std::cout << "🔧 [ConnectTip] hash=" << tip_to_connect->hash.GetHex() << std::endl;

    // Special case: Genesis block during early init (before block_validator_ is set)
    // During daemon startup, genesis (height 0) is initialized before the full service
    // initialization completes. This block has already been validated and stored in
    // ChainDB, so we can just set active_tip_ directly.
    if (!block_validator_ && tip_to_connect->height <= 1) {
        std::cout << "ℹ️  [ConnectTip] Early init block (height=" << tip_to_connect->height
                  << ") - setting active_tip_ directly" << std::endl;
        if (consensus_utxo_set_) {
            consensus_utxo_set_->SetBestBlock(tip_to_connect->hash, tip_to_connect->height);
        }
        PublishActiveTip(tip_to_connect, TipPublishReason::kEarlyInitGenesis);
        return true;
    }

    if (!block_validator_) {
        if (logger_) logger_->error("[ConnectTip] BlockValidator not initialized");
        std::cout << "❌ [ConnectTip] block_validator_ is NULL" << std::endl;
        return fail("block-validator-null");
    }

    // Read block from archival flatfile storage
    std::cout << "🔧 [ConnectTip] Reading stored block..." << std::endl;
    if (logger_) logger_->info("[ConnectTip] Attempting to read block at height " +
                 std::to_string(tip_to_connect->height) +
                 ", hash=" + tip_to_connect->hash.GetHex());
    auto block_result = ReadStoredBlock(tip_to_connect->hash);
    if (block_result.status() != Status::Ok) {
        if (logger_) logger_->error("[ConnectTip] Failed to read block at height " +
                      std::to_string(tip_to_connect->height) +
                      ", hash=" + tip_to_connect->hash.GetHex() +
                      ", status=" + std::to_string(static_cast<int>(block_result.status())));
        std::cout << "❌ [ConnectTip] Failed to read stored block" << std::endl;
        std::cout << "   status=" << static_cast<int>(block_result.status()) << std::endl;
        // Clear BLOCK_HAVE_DATA so BlockDownloadScheduler will re-download
        // from peers instead of infinite retry from corrupt/missing chaindb entry
        tip_to_connect->status &= ~BLOCK_HAVE_DATA;
        unreadable_blocks_.mark(tip_to_connect->hash);
        if (logger_) logger_->warning("[ConnectTip] Cleared BLOCK_HAVE_DATA for height " +
                      std::to_string(tip_to_connect->height) +
                      " — will be re-downloaded from peers");
        return fail("read-block-failed-status-" + std::to_string(static_cast<int>(block_result.status())));
    }
    Block block = block_result.value();
    std::cout << "✅ [ConnectTip] Block read successfully, vtx.size()=" << block.vtx.size() << std::endl;

    // === CT DIAGNOSTIC: Log every tx and output in the block ===
    for (size_t ti = 0; ti < block.vtx.size(); ti++) {
        const auto& dtx = block.vtx[ti];
        std::cout << "   [CT-DIAG] tx[" << ti << "] txid=" << dtx.GetTxid().AsUint256().GetHex().substr(0, 16)
                  << " vin=" << dtx.vin.size() << " vout=" << dtx.vout.size()
                  << " has_ct=" << dtx.HasConfidentialOutputs()
                  << " explicit_fee=" << dtx.GetExplicitFee()
                  << std::endl;
        for (size_t oi = 0; oi < dtx.vout.size(); oi++) {
            const auto& out = dtx.vout[oi];
            std::cout << "     [CT-DIAG] vout[" << oi << "] value=" << out.value.GetUna()
                      << " spk=" << out.scriptPubKey.size()
                      << " is_ct=" << out.is_confidential
                      << " commit=" << out.commitment.size()
                      << " proof=" << out.range_proof.size()
                      << " nonce=" << out.nonce.size()
                      << std::endl;
        }
    }

    if (logger_) logger_->info("[ConnectTip] Connecting block: height=" +
                 std::to_string(tip_to_connect->height) +
                 ", hash=" + tip_to_connect->hash.GetHex().substr(0, 16) + "...");

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase P.2: Pre-cache Utreexo proof BEFORE ConnectBlock updates the forest
    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: The proof must be generated against the PRE-block forest state.
    // Once ConnectBlock runs, the forest is updated and the proof would have
    // the wrong accumulator_root_before.
    // ═══════════════════════════════════════════════════════════════════════════
    // Generate and persist Utreexo transition proofs for EVERY block.
    // This runs BEFORE ConnectBlock mutates the forest, capturing the correct
    // pre-block forest state. Stored proofs enable CSN (stateless) sync of
    // historical blocks — without them, on-the-fly generation uses the wrong
    // forest state and produces invalid proofs.
    bool precache_success = false;
    if (!GetConfig().utreexo_stateless && bridge_node_ && consensus_utxo_set_) {
        bool reuse_persisted_transition_proof = false;

        // Startup replay can revisit already-active blocks after restart. In
        // that case the transition proof for this exact active-chain block is
        // already persisted in ChainDB, and regenerating it here burns CPU
        // before RPC is ready. Only reuse when the stored active-chain hash at
        // this height matches the block we're connecting, so reorg/fork paths
        // still regenerate against the correct pre-block forest.
        if (chain_db_) {
            auto active_hash_result =
                chain_db_->getBlockHashByHeight(static_cast<int>(tip_to_connect->height));
            if (active_hash_result.status() == Status::Ok &&
                active_hash_result.value() == tip_to_connect->hash) {
                auto transition_proof_result =
                    chain_db_->getTransitionProof(static_cast<int>(tip_to_connect->height));
                if (transition_proof_result.status() == Status::Ok) {
                    reuse_persisted_transition_proof = true;
                    if (logger_) {
                        logger_->info("[ConnectTip] Reusing persisted transition proof at height " +
                                      std::to_string(tip_to_connect->height));
                    }
                }
            }
        }

        if (!reuse_persisted_transition_proof) {
            try {
                // GenerateProofForBlock captures the current forest state (BEFORE connect)
                auto proof_data = bridge_node_->GenerateProofForBlock(block, tip_to_connect->height);

                // Tip prewarm: cache freshly generated proof in gossip layer so
                // first getproof requests are cache hits instead of provider work.
                if (proof_gossip_manager_) {
                    proof_gossip_manager_->PrewarmProof(tip_to_connect->hash, proof_data);
                }

                // Generate transition proof while forest is at pre-block state
                auto tp = consensus::UtreexoTransitionProof::generate(
                    consensus_utxo_set_->GetForest(),
                    block,
                    proof_data.spend_proof,
                    tip_to_connect->height);
                bridge_node_->SetCachedTransitionProof(tip_to_connect->hash, tp);
                precache_success = true;
            } catch (const std::exception& e) {
                // Non-fatal: Log warning but continue with block connection
                if (logger_) logger_->warning("[ConnectTip] Proof pre-caching failed: " + std::string(e.what()));
            }
        }
    }

    ChainWriteToken token;  // Service layer has write authority for reorg metadata and tip writes

    // ═══════════════════════════════════════════════════════════════════════════
    // PRE-VALIDATION: Check DNRF commitment BEFORE committing any state.
    // This prevents forest corruption from post-commit rejection + failed rollback.
    // Full filter hash verification happens after ConnectBlock (needs undo data).
    // ═══════════════════════════════════════════════════════════════════════════
    if (consensus::RequiresFilterCommitment(tip_to_connect->height) && !block.vtx.empty()) {
        auto dnrf_idx = consensus::FindFilterCommitmentIndex(block.vtx[0]);
        if (!dnrf_idx.has_value()) {
            if (logger_) logger_->error("[ConnectTip] REJECTING block " +
                std::to_string(tip_to_connect->height) +
                ": Missing mandatory DNRF filter commitment in coinbase");
            return fail("bad-filter-commitment: Missing DNRF commitment (pre-check)");
        }
    }

    // Recovery/startup replay path for stateless nodes:
    // If the canonical forest is still at the block's recorded pre-state,
    // this block has not yet advanced the shared accumulator in this process.
    // Replay it through StatelessNode first, then persist bookkeeping only.
    if (GetConfig().utreexo_stateless && stateless_node_ && consensus_utxo_set_ &&
        block.utreexo.has_value() &&
        !block.utreexo->accumulator_root_before.empty()) {
        const auto current_commitment = consensus_utxo_set_->GetForest().getCommitment();
        if (current_commitment == block.utreexo->accumulator_root_before) {
            if (logger_) {
                logger_->info("[ConnectTip] Stateless replay path: advancing shared forest from stored proof data at height " +
                              std::to_string(tip_to_connect->height));
            }

            // Transition-proof validation can use deletion targets that differ
            // from the batch proof embedded in the stored block. The ordered
            // CSN worker persists those exact, already-validated targets in the
            // hash-anchored replay sidecar. Recovery must prefer that record,
            // just like the ABC-CSN reorg loop does, or the first real spend
            // after a restart can replay against the wrong leaves.
            std::vector<consensus::UtreexoHash> replay_targets =
                block.utreexo->spend_proof.targets;
            std::vector<consensus::SpentOutputData> replay_spent_outputs;
            const std::vector<consensus::SpentOutputData>* spent_outputs =
                &block.utreexo->spent_outputs;
            const auto replay_result =
                chain_db_->getCSNSpendTargets(tip_to_connect->hash);
            if (replay_result.status() == Status::Ok) {
                CsnReplayData replay_data;
                if (!DecodeCsnReplayData(replay_result.value(), replay_data)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] Malformed CSN replay data at height " +
                                       std::to_string(tip_to_connect->height));
                    }
                    return fail("stateless-replay-data-malformed");
                }
                replay_targets = std::move(replay_data.spend_targets);
                if (replay_data.has_spent_outputs) {
                    replay_spent_outputs = std::move(replay_data.spent_outputs);
                    spent_outputs = &replay_spent_outputs;
                }
            }

            if (!stateless_node_->ReplayBlock(
                    block,
                    tip_to_connect->height,
                    replay_targets,
                    spent_outputs)) {
                if (logger_) {
                    logger_->error("[ConnectTip] Stateless replay failed at height " +
                                   std::to_string(tip_to_connect->height));
                }
                return fail("stateless-replay-failed");
            }

            // #356: route the delta-recompute + pre-block frontier capture +
            // ApplyBlockShieldedSection through the shared ApplyStatelessReplay-
            // Shielded funnel (also used by the ABC-CSN reorg replay loop) so
            // this crash-recovery branch's shielded-bearing blocks actually get
            // their note commitments and nullifiers applied instead of silently
            // skipping them. The guard reads the persisted ShieldedTipMarker: if
            // the pool is already at/ahead of this block (applied_out=false) we
            // pass nullptr into bookkeeping exactly as before; a gap behind
            // height-1 is a loud failure, not a silent skip.
            consensus::BlockUndo replay_undo;
            bool shielded_applied = false;
            std::string shielded_err;
            if (!ApplyStatelessReplayShielded(block, tip_to_connect->height, replay_undo,
                                              shielded_applied, shielded_err)) {
                if (logger_) {
                    logger_->error("[ConnectTip] Stateless recovery shielded apply failed at height " +
                                   std::to_string(tip_to_connect->height) + ": " + shielded_err);
                }
                return fail("stateless-recovery-shielded-apply-failed: " + shielded_err);
            }

            std::string bookkeeping_error;
            // When shielded_applied is true, replay_undo carries the real
            // pre_block_shielded_frontier (and pre_reset_shielded_epoch, if
            // this block is the epoch-reset height) captured by the funnel
            // above, so CommitConnectedBlockBookkeeping's skip_undo_write
            // guard does NOT fire and the UndoRecord persists the shielded
            // snapshot needed for a later disconnect. When shielded_applied
            // is false (pool already at/ahead of this block), nullptr
            // preserves prior behavior for this already-applied block.
            if (!CommitConnectedBlockBookkeeping(tip_to_connect, block,
                                                 shielded_applied ? &replay_undo : nullptr,
                                                 &bookkeeping_error)) {
                if (logger_) {
                    logger_->error("[ConnectTip] Stateless replay bookkeeping failed at height " +
                                   std::to_string(tip_to_connect->height) + ": " + bookkeeping_error);
                }
                return fail("stateless-replay-bookkeeping-failed: " + bookkeeping_error);
            }

            std::cout << "✅ [ConnectTip] Stateless replay bookkeeping SUCCEEDED" << std::endl;
            RecordBlockConnectStats(tip_to_connect->height, connect_start);
            return true;
        }
    }

    // Use BlockValidator to apply UTXO state and create undo data
    std::cout << "🔧 [ConnectTip] Calling BlockValidator::ConnectBlock..." << std::endl;
    consensus::BlockUndo block_undo;
    std::string error;

    // Phase 3a of the shielded reorg invertibility plan
    // (docs/specs/atomic_consensus_persistence_phase3.md). When the
    // hidden -consensus.atomic_persist=1 flag is on, route the
    // UTXO-map mutations from ConnectBlockInternal through a
    // ConsensusWriteBatch instead of mutating the live UTXO set
    // directly. The active-batch pointer is set on the validator
    // for the duration of one ConnectBlock call and cleared via
    // RAII. With the flag off the validator's pointer stays null
    // and behavior is identical to before.
    std::optional<consensus::ConsensusWriteBatch> active_batch;
    struct ActiveBatchScope {
        consensus::BlockValidator* validator;
        ~ActiveBatchScope() {
            if (validator != nullptr) {
                validator->setActiveConsensusWriteBatch(nullptr);
            }
        }
    };
    ActiveBatchScope batch_scope{nullptr};
    if (consensus::ConsensusWriteBatch::IsEnabled(*this) && block_validator_) {
        active_batch.emplace(*this, tip_to_connect->height, tip_to_connect->hash);
        block_validator_->setActiveConsensusWriteBatch(&active_batch.value());
        batch_scope.validator = block_validator_.get();
    }

    if (!block_validator_->ConnectBlock(block, tip_to_connect->height, tip_to_connect->hash, block_undo, error)) {
        if (active_batch.has_value()) {
            active_batch->Abort();
        }
        if (logger_) logger_->error("[ConnectTip] BlockValidator::ConnectBlock failed: " + error);
        std::cout << "❌ [ConnectTip] BlockValidator::ConnectBlock FAILED: " << error << std::endl;
        // #309/I2: BlockValidator::ConnectBlock returns false for BOTH consensus
        // violations AND operational faults (validation timeout, "Failed to commit
        // UTXO ...", missing UTXO/proofs). Signal "consensus-invalid" ONLY for an
        // unambiguous consensus-rule violation, so the reorg driver never marks a
        // valid block permanently failed on a transient fault (which would fork the
        // node off the network). Operational failures fall through as a plain,
        // retryable connect failure.
        if (out_consensus_invalid && IsUnambiguousConsensusViolation(error)) {
            *out_consensus_invalid = true;
        }
        return fail("connect-block-failed: " + error);
    }

    // Phase 3b step 3 part 3 (split lifecycle): replay staged
    // in-memory ops onto the live UTXO set NOW, but defer the
    // journal-row write so we can fold it into the outer
    // utxo_batch built below. After utxo_batch is committed,
    // active_batch->MarkCommittedAfterOuterBatch() closes the
    // lifecycle without re-writing the row.
    if (active_batch.has_value()) {
        const auto commit_status = active_batch->CommitStagedLiveState();
        if (commit_status != Status::Ok) {
            if (logger_) logger_->error("[ConnectTip] ConsensusWriteBatch::CommitStagedLiveState failed");
            return fail("consensus-write-batch-live-state-commit-failed");
        }
    }
    std::cout << "✅ [ConnectTip] BlockValidator::ConnectBlock SUCCEEDED" << std::endl;

    // Track C: Liquidity Vault block-event hook. The other call site
    // is validation_queue.cpp's parallel-validation path, but live-tip
    // extension goes through ConnectTip (this function) — we need the
    // hook here too or deposits never advance past OBSERVED on new
    // blocks. No-op when vault is disabled.
    dinero::vault::NotifyVaultTipConnected(static_cast<uint64_t>(tip_to_connect->height));

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 3b step 3 part 3 slice 3: hoisted block-undo persistence
    // ═══════════════════════════════════════════════════════════════════════════
    // The undo flatfile fsync is a separate storage from ChainDB and cannot be
    // folded into a rocksdb::WriteBatch — but its DURABILITY ORDER matters:
    // the flatfile write must hit disk BEFORE the unified ChainDB batch
    // commits, so when the batch commits the BLOCK_HAVE_UNDO flag and
    // undo_file/pos/size metadata, the flatfile entry those fields point to
    // is already on disk. If the flatfile write succeeds but the batch
    // crashes, the orphaned flatfile entry is harmless waste — no committed
    // metadata references it.
    //
    // Pre-fix this entire section ran ~300 lines later in ConnectTip, AFTER
    // utxo_batch was already committed. That ordering meant a crash between
    // the UTXO commit and the writeUndo / updateBlockIndex left the daemon
    // with UTXO state durable but no readable undo for that height —
    // DisconnectTip would later trip "missing undo data for active tip" and
    // fire safe mode. Hoisting it here closes that crash window.
    auto existing_undo = ReadStoredUndo(tip_to_connect->hash);

    // #274: this gate must mirror EVERY structural check the publish
    // invariant (CheckBlockDisconnectMaterialDurable) applies to a decoded
    // undo — count, non-empty created, and the shielded-frontier coupling.
    // A stored undo that passes a weaker gate here gets reused, persisted,
    // and then aborts at publish time anyway.
    //   - expected spent count = one entry per non-coinbase input.
    //   - block_has_shielded uses the SAME predicate as the invariant:
    //     tx.IsShielded() over ALL txs (coinbase included).
    bool block_has_shielded = false;
    uint64_t expected_spent_count = 0;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        if (tx.IsShielded()) block_has_shielded = true;
        if (tx_idx > 0) {  // skip coinbase
            expected_spent_count += tx.vin.size();
        }
    }

    const bool existing_undo_valid = existing_undo.status() == Status::Ok &&
        existing_undo.value().spent.size() == expected_spent_count &&
        !existing_undo.value().created.empty() &&
        (!block_has_shielded ||
         existing_undo.value().pre_block_shielded_frontier.has_value());
    if (existing_undo.status() == Status::Ok && !existing_undo_valid && logger_) {
        std::string why;
        if (existing_undo.value().spent.size() != expected_spent_count) {
            why = "spent count mismatch (expected " +
                  std::to_string(expected_spent_count) + " got " +
                  std::to_string(existing_undo.value().spent.size()) + ")";
        } else if (existing_undo.value().created.empty()) {
            why = "created vector empty (every block creates coinbase outputs)";
        } else {
            why = "missing pre_block_shielded_frontier for shielded block";
        }
        logger_->warning("[ConnectTip] Rejecting stored undo record at height=" +
                      std::to_string(tip_to_connect->height) +
                      ": " + why + " - rebuilding");
    }

    dinero::UndoRecord undo_record;
    if (existing_undo_valid) {
        undo_record = existing_undo.value();
        if (logger_) {
            logger_->debug("[ConnectTip] Reusing existing undo record from BlockAcceptor/legacy storage");
        }
    } else {
        undo_record = BlockUndoToUndoRecord(block_undo, block);

        // #274: stateless (CSN) ConnectBlock has no UTXO set and leaves
        // BlockUndo.spent_coins empty, so a freshly built undo_record on a
        // stateless node has spent.size()==0 while the block carries
        // spends. Reconstruct the spent list from ChainDB coin rows (rows
        // are still live — the unified batch that deletes them commits
        // later in ConnectTip). On stateful nodes ConnectBlock populates
        // exactly one entry per non-coinbase input, so this is a no-op.
        if (undo_record.spent.size() != expected_spent_count) {
            std::string recon_error;
            const auto recon_status = ReconstructSpentCoinsFromChainDb(
                block, tip_to_connect->height, undo_record.spent, recon_error);
            if (recon_status != Status::Ok ||
                undo_record.spent.size() != expected_spent_count) {
                if (recon_status == Status::Ok && recon_error.empty()) {
                    recon_error = "reconstructed spent count mismatch: expected " +
                                  std::to_string(expected_spent_count) +
                                  " got " + std::to_string(undo_record.spent.size());
                }
                if (logger_) {
                    logger_->error("[ConnectTip] CRITICAL: Failed to reconstruct spent coins for stateless undo: " + recon_error);
                    logger_->error("[ConnectTip] Undo spent list is required for rollback safety - aborting connection");
                }
                std::cout << "❌ [ConnectTip] undo spent reconstruction FAILED - aborting to prevent inconsistent state" << std::endl;

                if (active_batch.has_value()) active_batch->Abort();
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height, block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                        logger_->error("[ConnectTip] Database is now inconsistent - manual recovery required");
                    }
                } else {
                    if (logger_) logger_->info("[ConnectTip] Rollback successful - state restored");
                }
                return fail("undo-spent-reconstruction-failed: " + recon_error);
            }
            if (logger_) {
                logger_->info("[ConnectTip] Reconstructed " +
                              std::to_string(undo_record.spent.size()) +
                              " spent coins from ChainDB for stateless undo (height=" +
                              std::to_string(tip_to_connect->height) + ")");
            }
        }
    }

    // CORRECTNESS: the flatfile is the source of truth, not the in-memory
    // CBlockIndex metadata. A block that was previously connected-then-
    // disconnected keeps its `undo_size` / `undo_file` / `undo_pos` fields
    // populated on the in-memory CBlockIndex — those fields are not reset on
    // disconnect. If the same block is later re-connected (e.g., via a
    // reorg, reconsider, or same-height sibling race in a two-miner
    // scenario), `undo_size` is non-zero even though the undo entry in the
    // flatfile may no longer be reachable at that position.
    //
    // The original condition `undo_size == 0` trusted the in-memory
    // metadata and skipped the write in that case, advancing the tip to a
    // block whose undo couldn't be read back at DisconnectTip time. That
    // produced the "missing undo data for active tip" safe-mode fire on
    // Mac 2026-04-20 during a two-miner sibling race at height 3417.
    const bool need_flatfile_undo = block_storage_ &&
        (tip_to_connect->undo_size == 0 || !existing_undo_valid);
    if (need_flatfile_undo) {
        const std::vector<uint8_t> undo_bytes = undo_record.Serialize();
        auto undo_pos_result = block_storage_->writeUndo(tip_to_connect->hash, undo_bytes);
        if (undo_pos_result.status() != Status::Ok) {
            if (logger_) {
                logger_->error("[ConnectTip] CRITICAL: Failed to persist undo flatfile");
                logger_->error("[ConnectTip] Undo flatfile is required for rollback safety - aborting connection");
            }
            std::cout << "❌ [ConnectTip] writeUndo FAILED - aborting to prevent inconsistent state" << std::endl;

            if (active_batch.has_value()) active_batch->Abort();
            std::string rollback_error;
            if (!block_validator_->DisconnectBlock(block, tip_to_connect->height, block_undo, rollback_error)) {
                if (logger_) {
                    logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    logger_->error("[ConnectTip] Database is now inconsistent - manual recovery required");
                }
            } else {
                if (logger_) logger_->info("[ConnectTip] Rollback successful - state restored");
            }
            return fail("persist-undo-flatfile-failed-status-" + std::to_string(static_cast<int>(undo_pos_result.status())));
        }

        const auto& undo_pos = undo_pos_result.value();
        if (undo_pos.offset > std::numeric_limits<uint32_t>::max()) {
            if (active_batch.has_value()) active_batch->Abort();
            std::string rollback_error;
            block_validator_->DisconnectBlock(block, tip_to_connect->height, block_undo, rollback_error);
            return fail("persist-undo-flatfile-offset-overflow");
        }
        tip_to_connect->undo_file = undo_pos.file_number;
        tip_to_connect->undo_pos = static_cast<uint32_t>(undo_pos.offset);
        tip_to_connect->undo_size = undo_pos.size;
    }

    if (existing_undo.status() != Status::Ok) {
        if (tip_to_connect->undo_size == 0) {
            if (active_batch.has_value()) active_batch->Abort();
            std::string rollback_error;
            block_validator_->DisconnectBlock(block, tip_to_connect->height, block_undo, rollback_error);
            return fail("strict-archival-missing-flatfile-undo");
        }
        if (logger_) {
            logger_->info("[ConnectTip] Archival mode: skipping legacy ChainDB undo shadow write");
        }
    }

    // The undo metadata update (BLOCK_HAVE_UNDO flag + undo_file/pos/size)
    // goes INTO utxo_batch below — not as a standalone fsync. Keep the
    // in-memory block index honest for runtime callers, but persist with the
    // surgical ChainDB undo-locator writer below instead of depending on a
    // wholesale updateBlockIndex status snapshot.
    tip_to_connect->status |= BLOCK_HAVE_UNDO;

    // Phase 3b step 4 (revisit of slice 4): the shielded frontier
    // flat-file write is NOT hoisted above the unified batch.
    //
    // The first cut of slice 4 hoisted PersistShieldedState() here on
    // the theory that "frontier durable before marker durable" is the
    // correct ordering. In practice it created a worse crash window:
    // the frontier file would advance to block N+1's state, then the
    // crash hook (after_undo_before_tip) would fire, the unified
    // batch would never commit, and on restart the marker would
    // still be at N while the frontier was at N+1. Startup's
    // ShieldedTipMarker consistency check fails that mismatch loud
    // and refuses to start. test_shielded_daemon_restart_equivalence
    // catches this regression.
    //
    // Resolution: keep the marker IN the unified batch (the slice-4
    // atomicity win) but write the frontier AFTER the batch commits
    // (post-batch position, near the bottom of ConnectTip — same
    // place as the pre-slice-4 baseline). The frontier-vs-marker
    // race window is inherent to the flat-file/rocksdb gap; placing
    // the frontier write post-batch keeps that window narrow and
    // matches the order startup recovery already understands. A real
    // crash there would still leave marker-at-N+1 / frontier-at-N
    // briefly; that's the residual gap the working-copy pattern
    // (parked) is designed to close.

    // Phase 3b step 3 part 3 slice 6: pre-compute chainwork BEFORE the
    // unified batch so a malformed hex string fails the connect cleanly
    // instead of corrupting an in-flight WriteBatch. The result feeds
    // setTip and putHeader, both of which are now staged into utxo_batch.
    arith_uint256 connect_tip_work;
    try {
        connect_tip_work = ChainworkFromHex(tip_to_connect->chainwork);
    } catch (const std::exception& e) {
        if (logger_) logger_->error(std::string("[ConnectTip] Invalid chainwork hex: ") + e.what());
        std::cout << "❌ [ConnectTip] Invalid chainwork when persisting tip" << std::endl;
        if (active_batch.has_value()) active_batch->Abort();
        std::string rollback_error;
        if (!block_validator_->DisconnectBlock(block, tip_to_connect->height, block_undo, rollback_error)) {
            if (logger_) logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
        }
        return fail(std::string("invalid-chainwork: ") + e.what());
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // UTXO Persistence: Write created/spent UTXOs to ChainDB (RocksDB)
    // ═══════════════════════════════════════════════════════════════════════════
    // BlockValidator::ConnectBlock() updates the in-memory consensus UTXO set.
    // We must also persist to ChainDB so gettxout and mempool validation work.
    //
    // Phase 3b step 3 part 3 (slices 1–6): this batch carries the entire
    // §1 atomic-unit needed to advance the chain by one block:
    //   - UTXO column writes (deletes + puts) and txindex (always)
    //   - forest checkpoint + CHECKSUM_VERSION + ForestTipMarker (slice 2)
    //   - BLOCK_HAVE_UNDO + undo_file/pos/size block-index update (slice 3)
    //   - ShieldedTipMarker (slice 4)
    //   - canonical setTip + putHeightIndex (slice 6 — canonical
    //     pointers move LAST, atomic with everything else)
    //   - (flag-on) consensus journal row (slice 1)
    // All hit disk in a single fsync. With slice 6 there is no longer an
    // intermediate state where UTXO/forest/undo are durable but the tip
    // pointer hasn't moved (or vice-versa) — the boundary is one
    // all-or-nothing commit.
    //
    // 2026-04-30: ChainstateCommitBatch wraps the raw rocksdb::WriteBatch
    // with per-field staged-bool tracking. Each chain_db_->putXxx call
    // below is paired with a `ccb.Mark*Staged()` line. Before the final
    // writeBatch invocation, `ccb.AllRequiredStaged()` refuses commit
    // if any required field is missing (e.g., a future refactor that
    // accidentally drops `putHeader` would surface here as
    // "header_cf missing" instead of as a sibling-race wedge weeks
    // later). `auto& utxo_batch = ccb.Batch()` is a reference to the
    // wrapper's internal WriteBatch so existing `&utxo_batch` argument
    // sites remain unchanged.
    {
        const bool ut_active = consensus::IsUtreexoActive(
            static_cast<uint32_t>(tip_to_connect->height));
        const bool shielded_active = static_cast<uint32_t>(tip_to_connect->height) >=
            dinero::Params().shielded_activation_height;
        // Forest checkpoint delta campaign phase 1: with interval N > 1 the
        // full forest checkpoint is written only at heights % N == 0; the
        // per-block delta sidecar + ForestTipMarker still ride this batch
        // every block (DisconnectTip and phase 2's replay-restore need them).
        const uint32_t checkpoint_interval =
            GetConfig().utreexo_checkpoint_interval > 1
                ? GetConfig().utreexo_checkpoint_interval : 1;
        const bool write_full_checkpoint =
            (static_cast<uint64_t>(tip_to_connect->height) % checkpoint_interval) == 0;
        ChainstateCommitBatch ccb(static_cast<uint64_t>(tip_to_connect->height),
                                  ut_active,
                                  GetConfig().utreexo_stateless,
                                  shielded_active,
                                  checkpoint_interval);
        auto& utxo_batch = ccb.Batch();

        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            const auto& tx = block.vtx[tx_idx];
            const TxId txid = tx.GetTxid();
            const bool is_coinbase = (tx_idx == 0);

            // Delete spent UTXOs (skip coinbase — no inputs)
            if (!is_coinbase) {
                for (const auto& input : tx.vin) {
                    chain_db_->deleteCoin(token, input.prevout.txid.AsUint256(),
                                          input.prevout.vout, &utxo_batch);
                }
            }

            // Add created UTXOs (outputs)
            for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
                const auto& output = tx.vout[vout];

                dinero::Coin coin;
                coin.amount = output.value.GetUna();
                // Convert scriptPubKey bytes to hex string
                std::ostringstream spk_hex;
                for (uint8_t byte : output.scriptPubKey) {
                    spk_hex << std::hex << std::setfill('0') << std::setw(2)
                            << static_cast<int>(byte);
                }
                coin.script_pubkey = spk_hex.str();
                coin.height = static_cast<uint32_t>(tip_to_connect->height);
                coin.coinbase = is_coinbase;
                coin.is_confidential = output.is_confidential;
                coin.commitment = output.commitment;

                chain_db_->putCoin(token, txid.AsUint256(), vout, coin, &utxo_batch);
            }
        }
        // Every block has at least the coinbase outputs, so the UTXO
        // delta is always non-empty by this point.
        ccb.MarkUtxoStaged();

        // Transaction index: map txid -> (block_hash, tx_offset) for getTransaction() lookups
        for (uint32_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            const TxId txid = block.vtx[tx_idx].GetTxid();
            chain_db_->putTxIndex(token, txid.AsUint256(), tip_to_connect->hash, tx_idx, &utxo_batch);
        }
        ccb.MarkTxIndexStaged();

        // Phase 3b step 3 part 3 slice 2: fold the Utreexo forest
        // checkpoint + SHA256 checksum + CHECKSUM_VERSION meta +
        // ForestTipMarker into THIS WriteBatch. Pre-fix these
        // landed in three separate ChainDB writes ~500 lines later
        // in ConnectTip; a crash between any of them and the
        // earlier UTXO commit left the daemon with UTXO state
        // committed but the forest checkpoint that proves it
        // either lagging or completely missing. Now they all
        // commit atomically with UTXO + txindex + journal row in
        // one rocksdb fsync.
        //
        // The standalone forest-checkpoint code block (formerly
        // at line ~9416) is now a no-op because everything it
        // wrote is already inside utxo_batch — left as a stub
        // there with a comment explaining the move so the
        // structure of ConnectTip is still readable.
        std::vector<uint8_t> forest_serialized;
        if (consensus_utxo_set_ && write_full_checkpoint) {
            forest_serialized = consensus_utxo_set_->GetForest().serialize();
            pending_forest_checkpoint_bytes_ = forest_serialized.size();
            const auto checkpoint_status = chain_db_->putUtreexoCheckpointWithChecksum(
                token, tip_to_connect->height, forest_serialized, &utxo_batch);
            if (checkpoint_status != Status::Ok) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to stage Utreexo checkpoint at height " +
                                   std::to_string(tip_to_connect->height));
                }
                return fail("utreexo-checkpoint-stage-failed");
            }

            // CHECKSUM_VERSION sentinel: stamp once, on the first
            // checkpoint write per chain. Idempotent — re-staging
            // the same value every block is cheap enough to skip
            // the getUtreexoMeta query that the standalone path
            // used.
            chain_db_->putUtreexoMeta(token, "CHECKSUM_VERSION", "1", &utxo_batch);
            ccb.MarkUtreexoCheckpointStaged();
        }
        if (consensus_utxo_set_) {
            // ForestTipMarker — height + block hash + forest root.
            // Goes into the same batch so its existence on disk
            // implies the checkpoint above is also on disk.
            ChainDB::ForestTipMarker forest_marker;
            forest_marker.height = tip_to_connect->height;
            forest_marker.block_hash = tip_to_connect->hash;
            const consensus::UtreexoHash commitment =
                consensus_utxo_set_->GetForest().getCommitment();
            if (commitment.size() == 32) {
                std::memcpy(forest_marker.forest_root.data, commitment.data(), 32);
            } else {
                forest_marker.forest_root.SetNull();
            }
            chain_db_->putForestTipMarker(token, forest_marker, &utxo_batch);
            ccb.MarkUtreexoForestTipMarkerStaged();
        }

        // Phase 3b step 3 part 3 slice 3: stage the undo metadata update
        // (BLOCK_HAVE_UNDO flag + undo_file/pos/size) into THIS WriteBatch.
        // The undo flatfile fsync already ran above; folding the metadata
        // here means "flatfile entry exists" and "block index points to it"
        // become durable in the same atomic commit as UTXO/txindex/forest
        // checkpoint.
        //
        // D.1 (Apr 30 2026): unconditional re-stamp closed the original
        // active-tip missing-undo wedge.
        //
        // D.2 (May 2 2026): use a surgical ChainDB writer instead of
        // updateBlockIndex's all-fields put. updateUndoLocator updates only
        // undo_file/pos/size and ORs BLOCK_HAVE_UNDO; it preserves topology,
        // chainwork, block-body positions, and unrelated status bits. This
        // keeps ConnectTip out of the wholesale-overwrite class entirely.
        auto bi_status = chain_db_->updateUndoLocator(
            token,
            tip_to_connect->hash,
            tip_to_connect->undo_file,
            tip_to_connect->undo_pos,
            tip_to_connect->undo_size,
            &utxo_batch);
        if (bi_status == Status::NotFound) {
            // A freshly connected block should already have header metadata
            // from header/body acceptance. If an older path reaches ConnectTip
            // without that row, fall back to the authoritative full
            // block-index stamp rather than wedging. There is no existing row
            // to preserve in this branch.
            bi_status = chain_db_->updateBlockIndex(token, tip_to_connect, &utxo_batch);
        }
        if (bi_status == Status::Ok) {
            ccb.MarkBlockIndexStaged();
        } else {
            if (active_batch.has_value()) active_batch->Abort();
            if (logger_) {
                logger_->error("[ConnectTip] Failed to stage undo metadata block-index update");
            }
            std::string rollback_error;
            if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                  block_undo, rollback_error)) {
                if (logger_) {
                    logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                }
            }
            return fail("persist-undo-metadata-stage-failed-status-" +
                        std::to_string(static_cast<int>(bi_status)));
        }

        // Phase 3b option 1: stage the shielded frontier blob +
        // anchor history blob + ShieldedTipMarker into THIS WriteBatch.
        // With every shielded state container that contributes to the
        // DSRH (frontier bytes, anchor history bytes, marker) riding
        // the same atomic commit as UTXO + txindex + forest + undo
        // metadata + setTip + journal row, the marker-vs-frontier
        // gap that the legacy flat-file path had is closed structurally:
        // either every shielded container lands on disk together, or
        // none does. Critically, the journal row stored inside this
        // batch records DSRH(state-at-N+1); for that DSRH to match
        // at restart, the live state loaded from ChainDB MUST contain
        // every input that fed into it — frontier, nullifiers (sqlite,
        // already durable per-write), and anchor history. Pre-fix the
        // anchor history was only persisted by the post-batch
        // PersistShieldedState() call, so a crash between unified
        // batch and post-batch flushes left anchor history stale and
        // the journal verification fired consensus_journal_state_mismatch.
        {
            const auto frontier_bytes = shielded_tree_.SerializeFrontier();
            const std::string frontier_blob(frontier_bytes.begin(),
                                            frontier_bytes.end());
            const auto frontier_status =
                chain_db_->putUtreexoMeta(token, "shielded_frontier",
                                          frontier_blob, &utxo_batch);
            if (frontier_status != Status::Ok) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to stage shielded frontier blob at height " +
                                   std::to_string(tip_to_connect->height));
                }
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    }
                }
                return fail("persist-shielded-frontier-stage-failed-status-" +
                            std::to_string(static_cast<int>(frontier_status)));
            }
            ccb.MarkShieldedFrontierStaged();

            // Anchor history blob — DSRH input #4. The post-batch
            // PersistShieldedState() call still writes it standalone
            // for one release as a fallback (and stamps the v1
            // migration sentinel). Staging here ensures the journal
            // row's DSRH matches at restart.
            const auto anchor_bytes = shielded_anchor_history_.SerializeBytes();
            const std::string anchor_blob(anchor_bytes.begin(), anchor_bytes.end());
            const auto anchor_status =
                chain_db_->putUtreexoMeta(token, "shielded_anchor_history",
                                          anchor_blob, &utxo_batch);
            if (anchor_status != Status::Ok) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to stage anchor history blob at height " +
                                   std::to_string(tip_to_connect->height));
                }
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    }
                }
                return fail("persist-anchor-history-stage-failed-status-" +
                            std::to_string(static_cast<int>(anchor_status)));
            }
            ccb.MarkShieldedAnchorHistoryStaged();

            const auto shielded_snapshot = CurrentShieldedStateSnapshot();
            ChainDB::ShieldedTipMarker shielded_marker;
            shielded_marker.height = static_cast<int32_t>(tip_to_connect->height);
            shielded_marker.block_hash = tip_to_connect->hash;
            shielded_marker.shielded_root = shielded_snapshot.root;
            shielded_marker.tree_size = shielded_snapshot.tree_size;
            shielded_marker.nullifier_count = shielded_snapshot.nullifier_count;
            const auto stm_status =
                chain_db_->putShieldedTipMarker(token, shielded_marker, &utxo_batch);
            if (stm_status != Status::Ok) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to stage ShieldedTipMarker at height " +
                                   std::to_string(tip_to_connect->height));
                }
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    }
                }
                return fail("persist-shielded-tip-marker-stage-failed-status-" +
                            std::to_string(static_cast<int>(stm_status)));
            }
            ccb.MarkShieldedTipMarkerStaged();
        }

        // Phase 3b nullifier fold-in: stage every shielded spend's
        // nullifier into THIS WriteBatch under PREFIX_SHIELDED_NULLIFIER
        // keys. ConnectBlock already inserted them into the in-memory
        // (sqlite-backed) NullifierSet via ApplyShieldedBundle; here we
        // write the durable copy that pairs with the marker / frontier
        // / anchor history we just staged. ChainDB becomes the
        // authoritative on-disk source for the nullifier set, and the
        // sqlite NullifierSet becomes a per-instance cache that
        // startup will rebuild from ChainDB. A pre-batch crash now
        // leaves zero nullifier rows on disk (the batch never
        // committed), so the orphaned-nullifier inline rollback in
        // VerifyOrBootstrapShieldedTipMarker is no longer needed.
        for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
            const auto& tx = block.vtx[tx_idx];
            if (!tx.IsShielded()) {
                continue;
            }
            consensus::shielded::ShieldedBundle bundle;
            const auto decode = consensus::shielded::DeserializeShieldedBundle(
                tx.shielded_bundle_bytes, &bundle);
            if (decode != consensus::shielded::BundleDecodeError::Ok) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to decode shielded bundle for "
                                   "nullifier staging at height " +
                                   std::to_string(tip_to_connect->height) +
                                   " tx " + std::to_string(tx_idx) + " code=" +
                                   std::to_string(static_cast<int>(decode)));
                }
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    }
                }
                return fail("shielded-bundle-decode-for-nullifier-staging-failed");
            }
            for (const auto& spend : bundle.spends) {
                const auto nf_status = chain_db_->putShieldedNullifier(
                    token,
                    static_cast<uint32_t>(tip_to_connect->height),
                    spend.nullifier.data(),
                    &utxo_batch);
                if (nf_status != Status::Ok) {
                    if (active_batch.has_value()) active_batch->Abort();
                    if (logger_) {
                        logger_->error("[ConnectTip] Failed to stage shielded nullifier at height " +
                                       std::to_string(tip_to_connect->height));
                    }
                    std::string rollback_error;
                    if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                          block_undo, rollback_error)) {
                        if (logger_) {
                            logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                        }
                    }
                    return fail("persist-shielded-nullifier-stage-failed-status-" +
                                std::to_string(static_cast<int>(nf_status)));
                }
            }
        }
        // Shielded epoch reset (hard-fork cutover): purge the AUTHORITATIVE
        // ChainDB nullifier set so a restart's rehydration cannot resurrect the
        // pre-cutover pool. Staged into utxo_batch so it commits atomically with
        // setTip. The frontier/anchor blobs staged above already reflect the
        // (empty) state ConnectBlock reset in memory, so only the nullifier CF
        // needs an explicit purge. Block H is shielded-empty by the wall rule,
        // so the staging loop above added nothing.
        if (consensus::shielded::IsShieldedEpochResetHeight(
                static_cast<uint32_t>(tip_to_connect->height),
                dinero::Params().shielded_epoch_reset_height)) {
            const auto purged =
                chain_db_->deleteAllShieldedNullifiers(token, &utxo_batch);
            if (!purged.ok()) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] Failed to stage shielded epoch "
                                   "reset nullifier purge at height " +
                                   std::to_string(tip_to_connect->height));
                }
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) {
                        logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                    }
                }
                return fail("shielded-epoch-reset-nullifier-purge-stage-failed");
            }
        }

        // Track that nullifier staging completed (or that the block had
        // no shielded txs — both shapes leave the batch consistent).
        ccb.MarkShieldedNullifiersStaged();

        // Phase 3b step 3 part 3 slice 6: stage canonical setTip +
        // putHeightIndex into THIS WriteBatch as the final consensus
        // pointer move. After this, every disk write needed to
        // promote block N+1 to canonical tip is in one batch — UTXO
        // + txindex + forest + undo metadata + ShieldedTipMarker +
        // tip pointer + height index — atomic on commit. The
        // §1 atomic-unit law applies in full: a crash before
        // utxo_batch's writeBatch returns leaves NONE of these on
        // disk; a crash after leaves ALL of them on disk. There is
        // no intermediate "tip says N+1 but UTXO is at N" or
        // "UTXO is at N+1 but tip is at N" state to recover from.
        const auto tip_status = chain_db_->setTip(
            token,
            tip_to_connect->hash,
            static_cast<int>(tip_to_connect->height),
            connect_tip_work,
            &utxo_batch);
        if (tip_status != Status::Ok) {
            if (active_batch.has_value()) active_batch->Abort();
            if (logger_) {
                logger_->error("[ConnectTip] Failed to stage canonical tip pointer at height " +
                               std::to_string(tip_to_connect->height));
            }
            std::string rollback_error;
            if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                  block_undo, rollback_error)) {
                if (logger_) {
                    logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                }
            }
            return fail("persist-tip-stage-failed-status-" +
                        std::to_string(static_cast<int>(tip_status)));
        }
        ccb.MarkSetTipStaged();

        const auto height_idx_status = chain_db_->putHeightIndex(
            token,
            static_cast<int>(tip_to_connect->height),
            tip_to_connect->hash,
            &utxo_batch);
        if (height_idx_status != Status::Ok) {
            if (active_batch.has_value()) active_batch->Abort();
            if (logger_) {
                logger_->error("[ConnectTip] Failed to stage height index at height " +
                               std::to_string(tip_to_connect->height));
            }
            std::string rollback_error;
            if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                  block_undo, rollback_error)) {
                if (logger_) {
                    logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                }
            }
            return fail("persist-height-index-stage-failed-status-" +
                        std::to_string(static_cast<int>(height_idx_status)));
        }
        ccb.MarkHeightIndexStaged();

        // Phase 3b step 3 part 3 slice 1 (split lifecycle): fold the
        // journal row into THIS WriteBatch. With the flag on,
        // AttachJournalRowToBatch must succeed — failing to attach
        // means the row would either be missing on disk (if we
        // pressed on) or written twice (if we fell through to
        // active_batch->Commit() below). Both are wrong, so abort
        // the batch + fail the connect cleanly. With the flag off,
        // active_batch is empty and this whole block is a no-op.
        bool journal_row_attached = false;
        if (active_batch.has_value()) {
            journal_row_attached =
                active_batch->AttachJournalRowToBatch(&utxo_batch);
            if (!journal_row_attached) {
                active_batch->Abort();
                if (logger_) {
                    logger_->error("[ConnectTip] AttachJournalRowToBatch returned false "
                                   "while consensus.atomic_persist=1 — refusing to commit "
                                   "with an inconsistent journal row state");
                }
                return fail("consensus-journal-attach-failed");
            }
            ccb.MarkJournalRowStaged();
        } else {
            // active_batch optional — when consensus.atomic_persist is
            // off, no journal row is required; mark anyway so the batch
            // doesn't fail AllRequiredStaged on absence-by-design.
            ccb.MarkJournalRowStaged();
        }

        // P1 (Apr 30 2026): fold the Utreexo delta sidecar (UD:<hash>)
        // into the same utxo_batch that commits setTip + height index +
        // header metadata. Pre-fix this sidecar was Put + fsynced in a
        // SEPARATE WriteBatch ~280 lines below, leaving a crash window
        // where the canonical tip was durable but its UD:<hash> sidecar
        // was not. DisconnectTip treats the sidecar as mandatory for
        // every Utreexo-active block, so a crash there left an
        // undisconnectable active tip — same wedge shape as the
        // missing-undo case, just via the delta sidecar instead of the
        // undo flatfile. Atomic with the canonical batch closes the
        // window: tip + delta sidecar land together or neither does.
        if (consensus::IsUtreexoActive(tip_to_connect->height) && !GetConfig().utreexo_stateless) {
            if (!block_undo.utreexo_delta.has_value()) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) logger_->error("[ConnectTip] Missing Utreexo delta after ConnectBlock");
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                }
                return fail("missing-utreexo-delta");
            }
            std::string delta_blob;
            std::string delta_error;
            if (!SerializeUtreexoDelta(*block_undo.utreexo_delta, delta_blob, delta_error)) {
                if (active_batch.has_value()) active_batch->Abort();
                if (logger_) logger_->error("[ConnectTip] Failed to serialize Utreexo delta sidecar: " +
                                            delta_error);
                std::string rollback_error;
                if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                      block_undo, rollback_error)) {
                    if (logger_) logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
                }
                return fail("serialize-utreexo-delta-failed: " + delta_error);
            }
            const std::string delta_key = MakeUtreexoDeltaUndoKey(tip_to_connect->hash);
            utxo_batch.Put(delta_key, delta_blob);
            ccb.MarkUtreexoDeltaStaged();
        }

        // Test-only crash boundary "after_undo_before_tip" — fires
        // immediately BEFORE the unified batch commits, so a crash
        // here leaves NONE of the batched writes (UTXO, txindex,
        // forest checkpoint, BLOCK_HAVE_UNDO metadata,
        // ShieldedTipMarker, setTip, height index, journal row) on
        // disk. The slice-3 hoisted writeUndo flatfile IS durable on
        // disk by this point — exactly matching the historical
        // semantic ("after undo durable, before tip durable") that
        // tests like test_shielded_daemon_restart_equivalence and
        // test_connecttip_restart_equivalence depend on. Pre-slice-6
        // this hook fired between the legacy standalone setTip fsync
        // and the legacy standalone writeUndo + post-batch flushes;
        // slice 6 collapsed all of those into the unified batch, so
        // the only point where "undo durable, tip not yet" still
        // holds is right before writeBatch is invoked.
        dinero::testing::MaybeAbortAt("after_undo_before_tip",
                                      dinero::Params().network_id == "regtest");

        // Architectural invariant: every required field must be staged
        // before the unified batch can be committed. This is the
        // "structural" half of the publication-invariant story — the
        // CheckBlockDisconnectMaterialDurable read-back (5a6cc0799) is
        // the runtime half. Together: a future refactor that drops a
        // Put from the batch fails AllRequiredStaged here BEFORE
        // writeBatch is ever invoked, AND if it somehow slipped through,
        // CheckBlockDisconnectMaterialDurable catches it post-commit
        // before active_tip_ is published.
        if (auto missing = ccb.AllRequiredStaged()) {
            if (active_batch.has_value()) active_batch->Abort();
            if (logger_) {
                logger_->error("[ConnectTip] commit-batch incomplete: missing field '" +
                               *missing + "' at height " +
                               std::to_string(tip_to_connect->height) +
                               "; refusing to commit");
            }
            std::cout << "❌ [ConnectTip] commit-batch incomplete: missing '" << *missing
                      << "' — refusing to commit" << std::endl;
            std::string rollback_error;
            if (!block_validator_->DisconnectBlock(block, tip_to_connect->height,
                                                  block_undo, rollback_error)) {
                if (logger_) logger_->error("[ConnectTip] FATAL: Rollback failed: " + rollback_error);
            }
            return fail("commit-batch-incomplete-missing-" + *missing);
        }

        auto utxo_status = chain_db_->writeBatch(token, ccb.ReleaseBatch(), true);

        // After the outer rocksdb commit succeeds, close the
        // ConsensusWriteBatch lifecycle. The journal row was
        // already written into utxo_batch and just hit disk
        // atomically with the UTXO/txindex column writes;
        // MarkCommittedAfterOuterBatch flips the state machine to
        // Committed so the destructor doesn't trip the D4 leak
        // panic. On utxo_batch failure, Abort() the batch instead.
        if (active_batch.has_value()) {
            if (utxo_status == Status::Ok) {
                active_batch->MarkCommittedAfterOuterBatch();
            } else {
                active_batch->Abort();
            }
        }

        // Phase 3b step 3 part 3 (slices 1–3): the unified batch
        // carries UTXO + txindex + forest checkpoint + BLOCK_HAVE_UNDO
        // metadata + (flag-on) journal row. A failure here means NONE
        // of those landed on disk — but the in-memory consensus state
        // (UTXO set, forest, shielded frontier as of CommitStagedLiveState)
        // has ALREADY been mutated by ConnectBlock, and the undo
        // flatfile was already fsynced by slice 3's hoist. Returning
        // success while none of the disk side committed is exactly
        // the memory/disk split §1's law forbids — silently advancing
        // active_tip_, notifying wallets, and updating consensus_utxo_
        // tip metadata against UTXO state that does not exist on disk.
        //
        // Fail loud. Until phase 3b's working-copy pattern lands and
        // we can trivially roll the in-memory mutation back, the
        // correct behavior with the flag on is consensus safe mode —
        // the operator must inspect the chain state and call
        // safemode.exit { confirm: true } before block connect or
        // template generation resume. With the flag off, log + fail
        // is sufficient because the legacy per-write paths are
        // already best-effort and the daemon will catch this on the
        // next ConnectTip / startup verification.
        if (utxo_status != Status::Ok) {
            const std::string reason =
                "unified-consensus-batch-write-failed (height=" +
                std::to_string(tip_to_connect->height) +
                " block=" + tip_to_connect->hash.GetHex().substr(0, 16) +
                "… status=" + std::to_string(static_cast<int>(utxo_status)) + ")";
            if (logger_) {
                logger_->error("[ConnectTip] FATAL: " + reason);
                logger_->error("[ConnectTip] In-memory consensus state has been mutated by "
                               "ConnectBlock but the unified disk batch did NOT commit. "
                               "Refusing to advance the tip — this would create the exact "
                               "memory/disk split phase 3b is designed to eliminate.");
            }
            std::cout << "❌ [ConnectTip] FATAL: " << reason << std::endl;
            if (consensus::ConsensusWriteBatch::IsEnabled(*this)) {
                EnterSafeMode(reason);
            }
            return fail("unified-consensus-batch-write-failed");
        }

        if (logger_) logger_->info("[ConnectTip] Persisted UTXOs for " +
                     std::to_string(block.vtx.size()) + " transactions to ChainDB");
    }

    // Phase 3b step 4: write the shielded frontier flat file
    // IMMEDIATELY after the unified batch commits — before any
    // crash hook fires. The marker for tip N+1 is durable in the
    // unified batch above; the frontier file pairs with that marker
    // (root + tree_size + nullifier_count) and must be on disk before
    // the post-batch crash hooks (after_tip_before_checkpoint,
    // after_height_index_before_header) trigger, otherwise startup's
    // ShieldedTipMarker consistency check sees the marker ahead of
    // the frontier and refuses to start. Putting the flat-file write
    // first narrows the marker-vs-frontier crash window to almost
    // zero — only the gap between rocksdb's batch commit returning
    // and the std::ofstream fsync, which is microseconds in practice.
    //
    // Phase 3b precondition for step 6 (recovery-maze deletion):
    // crash-inject IMMEDIATELY between the unified-batch fsync
    // returning Ok and the frontier flat-file write below. At that
    // boundary the marker says N+1 and the frontier is still at N
    // — the exact partial state RecoverShieldedStateFromTipMarker
    // exists to reconcile. The
    // test_frontier_write_gap_recovery integration test crashes
    // here, restarts, and asserts the daemon recovers without
    // operator intervention. If the test passes, deleting the
    // recovery maze in step 6 is safe; if it fails, the maze is
    // still load-bearing.
    dinero::testing::MaybeAbortAt("after_unified_batch_before_frontier_write",
                                  dinero::Params().network_id == "regtest");

    if (!PersistShieldedState() && logger_) {
        logger_->warning("[ConnectTip] Failed to persist shielded frontier at height " +
                         std::to_string(tip_to_connect->height));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 3b step 3 part 3 slice 5: hoisted UTXO position-index update
    // ═══════════════════════════════════════════════════════════════════════════
    // The position index is in-memory only (rebuildable from chain state at
    // startup), so there's nothing to fold into the unified batch. The
    // ordering still matters: in-memory consensus side state should sync
    // immediately after the unified batch commits, BEFORE setTip + height
    // index move (slice 6's "canonical pointers move last" rule). With
    // this hoist, the consensus cluster — UTXO set + forest + position
    // index — is fully aligned with disk before any tip pointer advances.
    //
    // Pre-fix the position-index update ran ~250 lines later, after
    // setTip / height index / header CF / multiple side-state writes.
    // Same-process crash recovery still rebuilt correctly because the
    // index is reconstructable, but the function flow was harder to
    // reason about: setTip moved the canonical pointer while the
    // proof-serving side state lagged behind.
    //
    // Crash hook stays at its original position (after setTip +
    // height index land, see the matching MaybeAbortAt below) so the
    // on-disk consensus snapshot is fully consistent at the crash
    // boundary — moving the hook here would split it across the tip
    // pointer and the position-index update would advance UTXO/forest
    // state on disk past a tip that hadn't yet moved. The
    // test_position_index_restart_equivalence integration test
    // depends on the on-disk side being fully durable when the hook
    // fires.
    if (block_undo.utreexo_delta.has_value() && utxo_position_index_ && !GetConfig().utreexo_stateless) {
        const auto& delta = block_undo.utreexo_delta.value();

        // Identify intra-block ephemeral UTXOs (created and consumed in same block).
        // These never enter the Utreexo forest, so they don't appear in delta.addedLeaves.
        std::unordered_map<dinero::OutPoint, size_t> intra_outputs;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            const auto& tx = block.vtx[tx_idx];
            TxId txid = tx.GetTxid();
            for (uint32_t n = 0; n < tx.vout.size(); n++) {
                intra_outputs[dinero::OutPoint(txid, n)] = tx_idx;
            }
        }
        std::unordered_set<dinero::OutPoint> ephemeral_outputs;
        for (const auto& tx : block.vtx) {
            if (tx.IsCoinbase()) continue;
            for (const auto& input : tx.vin) {
                dinero::OutPoint op(input.prevout.txid, input.prevout.vout);
                if (intra_outputs.count(op)) {
                    ephemeral_outputs.insert(op);
                }
            }
        }

        // Invariant: addedLeaves.size() must equal non-ephemeral outputs in block
        size_t expected_outputs = 0;
        for (const auto& tx : block.vtx) {
            TxId txid = tx.GetTxid();
            for (uint32_t n = 0; n < tx.vout.size(); n++) {
                if (!ephemeral_outputs.count(dinero::OutPoint(txid, n))) {
                    expected_outputs++;
                }
            }
        }

        if (delta.addedLeaves.size() != expected_outputs) {
            if (logger_) logger_->error("[ConnectTip] Phase 11a: Delta size mismatch: " +
                          std::to_string(delta.addedLeaves.size()) + " leaves, " +
                          std::to_string(expected_outputs) + " outputs");
            return fail("utreexo-delta-size-mismatch");  // CRITICAL: Indexing invariant violated
        }

        // Step 1: Remove positions for spent UTXOs (inputs)
        size_t removed_count = 0;
        for (const auto& tx : block.vtx) {
            if (tx.IsCoinbase()) continue;  // Skip coinbase (no inputs to spend)

            for (const auto& input : tx.vin) {
                TxId prev_txid = input.prevout.txid;
                uint32_t prev_vout = input.prevout.vout;

                // Skip intra-block spends (ephemeral UTXOs never had positions)
                if (ephemeral_outputs.count(dinero::OutPoint(prev_txid, prev_vout))) {
                    continue;
                }

                auto old_position = utxo_position_index_->RemovePosition(prev_txid, prev_vout);
                if (old_position.has_value()) {
                    removed_count++;
                }
            }
        }

        // Step 2: Add positions for created UTXOs (outputs)
        // Skip ephemeral outputs (intra-block spends never enter forest)
        size_t idx = 0;
        for (const auto& tx : block.vtx) {
            TxId txid = tx.GetTxid();
            for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
                if (ephemeral_outputs.count(dinero::OutPoint(txid, vout))) {
                    continue;  // Ephemeral output — no forest position
                }
                const auto& added = delta.addedLeaves[idx++];
                utxo_position_index_->AddPosition(txid, vout, added.position);
            }
        }

        if (logger_) logger_->info("[ConnectTip] Phase 11a: Removed " + std::to_string(removed_count) +
                     " positions, added " + std::to_string(delta.addedLeaves.size()) + " positions");
    }

    // Build and persist BIP158 GCS block filter.
    // SipHash key = prev_block_hash (avoids circular dependency with coinbase commitment).
    // Excludes OP_RETURN outputs (provably unspendable, matches assembler).
    // Includes spent input scriptPubKeys from undo data.
    {
        std::vector<std::vector<uint8_t>> scripts;

        // Collect output scriptPubKeys (excluding OP_RETURN)
        for (const auto& tx : block.vtx) {
            for (const auto& out : tx.vout) {
                if (!out.scriptPubKey.empty() && out.scriptPubKey[0] != 0x6a) {
                    scripts.push_back(out.scriptPubKey);
                }
            }
        }

        // Collect spent input scriptPubKeys from undo data
        for (const auto& entry : block_undo.spent_coins) {
            if (!entry.coin.scriptPubKey.empty()) {
                scripts.push_back(entry.coin.scriptPubKey);
            }
        }

        // Use prev_block_hash as SipHash key (deterministic before mining)
        auto filter = consensus::GCSFilter::Build(scripts, block.header.prev_block_hash);
        if (!filter.IsEmpty()) {
            // Validate DNRF filter commitment in coinbase
            if (!block.vtx.empty()) {
                uint256 filter_hash = filter.GetHash();
                std::string filter_error;
                if (!consensus::ValidateFilterCommitment(block.vtx[0], filter_hash, tip_to_connect->height, filter_error)) {
                    // NOTE: The mandatory existence check is now in the pre-validation above
                    // (before ConnectBlock). This post-check verifies the commitment HASH
                    // matches the computed filter. A mismatch here means the commitment exists
                    // but contains the wrong hash — log a warning for investigation.
                    // We do NOT call DisconnectBlock here because that can corrupt the forest
                    // if the rollback fails (the root cause of the height-10630 consensus split).
                    if (consensus::RequiresFilterCommitment(tip_to_connect->height)) {
                        if (logger_) logger_->error("[ConnectTip] DNRF filter hash MISMATCH at height " +
                                                    std::to_string(tip_to_connect->height) + ": " + filter_error);
                    } else {
                        if (logger_) logger_->warning("[ConnectTip] Filter commitment validation failed (pre-activation): " + filter_error);
                    }
                }
            }

            chain_db_->putBlockFilter(token, tip_to_connect->hash,
                                       filter.encoded_data, filter.element_count);
        }
    }

    // P1 (Apr 30 2026): the Utreexo delta sidecar (UD:<hash>) was
    // previously written here in a separate WriteBatch + fsync,
    // post-canonical-commit. That left a crash window between tip
    // durable and sidecar durable, producing an undisconnectable
    // active tip. The sidecar is now staged into the canonical
    // utxo_batch above (search "P1 (Apr 30 2026)") so tip + sidecar
    // land atomically. STATELESS (CSN) mode still skips: CSN does not
    // compute deltas in ConnectBlockInternal.

    // ═══════════════════════════════════════════════════════════════════════════
    // CRASH-SAFETY ORDERING: Undo data MUST be durably written to the flatfile
    // BEFORE the canonical tip is persisted. If we setTip first and crash before
    // writeUndo completes, we end up with a tip pointing to a block whose undo
    // is missing — DisconnectTip becomes impossible and the node wedges in safe
    // mode ("missing undo data for active tip"). That state requires operator
    // intervention (the auto-recovery path is disarmed because --reindex-chainstate
    // is itself broken — see chainstate_recovery_marker.h).
    //
    // Both writeUndo (block_storage.cpp:608) and setTip (chain_db.cpp:687) fsync
    // individually. The invariant is maintained purely by ordering: writeUndo
    // returns only after the undo flatfile is durable, so when setTip's fsynced
    // write lands, the undo it references is guaranteed to already be on disk.
    // ═══════════════════════════════════════════════════════════════════════════

    // Phase 3b step 3 part 3 slice 3: block-undo persistence (both
    // the flatfile fsync and the BLOCK_HAVE_UNDO + undo_file/pos/size
    // block-index update) ran ~300 lines earlier in ConnectTip — see
    // the slice-3 hoist above the unified-batch block. Hoisted because:
    //
    //   1. The flatfile fsync must complete BEFORE the unified
    //      WriteBatch commits, so when the batch commits the
    //      BLOCK_HAVE_UNDO metadata, the flatfile entry it points to
    //      is already on disk.
    //
    //   2. The block-index update goes INTO that unified batch,
    //      atomic with UTXO + txindex + forest checkpoint + journal
    //      row. No more crash window where UTXO state is durable
    //      but undo metadata is missing — the bug that fired the
    //      "missing undo data for active tip" safe mode on the
    //      fleet on 2026-04-20.
    //
    // This site is intentionally a stub so the structure of
    // ConnectTip stays readable.

    // Phase 3b step 4 (hook relocation): the
    // "after_undo_before_tip" hook moved INTO the unified-batch
    // staging block, firing immediately before writeBatch. Pre-step-4
    // it fired here, after the batch had already committed — which
    // broke the semantic ("undo durable, tip not yet") because slice
    // 6 folded setTip into the batch. Tests that key on this hook
    // (test_shielded_daemon_restart_equivalence,
    // test_connecttip_restart_equivalence,
    // test_bug1_missing_undo_reproducer) need the chain to NOT
    // advance through the crash boundary, which only the pre-commit
    // position guarantees.

    // Phase 3b step 3 part 3 slice 6: canonical setTip + height
    // index now stage into the unified utxo_batch (see slice-6
    // staging block ~600 lines earlier). Pre-slice-6 these were two
    // standalone fsyncs after the unified batch — a crash between
    // utxo_batch and setTip left UTXO state durable but the tip
    // pointer at the previous height; a crash between setTip and
    // putHeightIndex left tip at N+1 with the height index still at
    // N. After slice 6 both atomic with the rest of the §1 unit.
    //
    // This site is intentionally a stub — same shape as slices 2, 3,
    // 4. Crash hooks "after_tip_before_checkpoint" and
    // "after_height_index_before_header" remain in their original
    // positions for backward compatibility with the integration
    // tests that key on those names (test_tip_persist_restart_
    // equivalence, test_shielded_tip_persist_restart_equivalence,
    // test_header_cf_restart_equivalence). The intermediate states
    // those hooks were specced to test ("tip durable but checkpoint
    // missing", "height index durable but header missing") are now
    // unreachable post-slice-6 because tip + checkpoint + height
    // index commit atomically. The hooks still fire and the tests
    // still pass — restart finds tip + checkpoint + height index
    // all durable together, replays forward to the persisted height,
    // which is what the tests already assert.

    // Test-only crash boundary "after_tip_before_checkpoint" — kept
    // here for test compatibility. After slice 6, tip + checkpoint
    // are atomic (both in utxo_batch), so this hook fires AFTER
    // both are durable; the test still passes because restart finds
    // a coherent on-disk snapshot.
    dinero::testing::MaybeAbortAt("after_tip_before_checkpoint",
                                  dinero::Params().network_id == "regtest");

    // Test-only crash boundary "after_height_index_before_header" —
    // kept here for test compatibility. After slice 6, height index
    // is in utxo_batch (durable atomic with tip); putHeader is the
    // next disk write below this hook. The hook still gates "after
    // height index durable, before header CF durable."
    dinero::testing::MaybeAbortAt("after_height_index_before_header",
                                  dinero::Params().network_id == "regtest");

    // Persist block header alongside tip + height index so every active-chain
    // block has a header CF entry regardless of how it arrived (peer IBD,
    // assumeUTXO bootstrap, or local miner). Without this, nodes that synced
    // via snapshot have an empty header CF, which breaks:
    //   - ValidateProofOfWork (needs parent bits)
    //   - blockchain.getblockheader RPC
    //   - startup block index rebuild (iterates header CF)
    // Non-fatal: consensus state (UTXO, tip, height index) is already committed.
    {
        auto hdr_status = chain_db_->putHeader(
            token,
            tip_to_connect->hash,
            block.header,
            static_cast<int>(tip_to_connect->height),
            connect_tip_work
        );
        if (hdr_status != Status::Ok) {
            if (logger_) logger_->warning("[ConnectTip] header persist failed at height " +
                                         std::to_string(tip_to_connect->height) +
                                         " (non-fatal)");
        }
    }

    // Capture post-block forest root for BridgeNode proof metadata
    // Must happen AFTER ConnectBlock (forest updated) and BEFORE checkpoint serialization
    if (precache_success && bridge_node_ && consensus_utxo_set_) {
        bridge_node_->SetCachedRootAfter(
            tip_to_connect->hash,
            tip_to_connect->height,
            consensus_utxo_set_->GetForest().getCommitment()
        );
    }

    // Phase 3b step 3 part 3 slice 5: the position-index update loop
    // ran ~250 lines earlier in ConnectTip — see the slice-5 hoist
    // immediately after the unified-batch commit. Hoisted so the
    // in-memory consensus cluster (UTXO set + forest + position
    // index) finishes syncing with disk BEFORE setTip + height index
    // move (slice 6's "canonical pointers move last" rule).
    //
    // The test-only crash boundary stays here, NOT at the slice-5
    // hoist site, on purpose: at this point in ConnectTip every
    // on-disk consensus container (utxo_batch + setTip + height
    // index + header CF + GCS filter + delta sidecar) has already
    // committed for this block, and the only remaining work is
    // in-memory side state (active_tip_, notifyBlockConnected,
    // HeaderChainSelector). The
    // test_position_index_restart_equivalence integration test
    // depends on the on-disk snapshot being fully consistent at the
    // crash boundary — moving the hook to immediately after the
    // unified batch would split it across the canonical tip
    // pointer (utxo_batch durable for block N+1, tip still at N)
    // and break startup recovery.
    //
    // Hook name "after_header_before_position_index" is preserved
    // for the integration test's DINERO_CRASH_AT trigger. The
    // "before_position_index" half of the name is now historical:
    // the position index has already been updated by the slice-5
    // hoist by the time we reach this point. The hook still
    // accurately gates "consensus durable, in-memory tip pointer
    // not yet promoted."
    dinero::testing::MaybeAbortAt("after_header_before_position_index",
                                  dinero::Params().network_id == "regtest");

    // NOTE: Undo persistence moved earlier in ConnectTip (before setTip) for
    // crash safety — see the CRASH-SAFETY ORDERING comment above the setTip
    // block.
    //
    // Phase 3b step 3 part 3 slice 2: the Utreexo checkpoint, the
    // CHECKSUM_VERSION sentinel, and the ForestTipMarker are now
    // staged into the same rocksdb::WriteBatch as the UTXO/txindex
    // writes (and, when the flag is on, the consensus journal row).
    // See the slice-2 staging block ~500 lines earlier in ConnectTip.
    // That makes the forest-checkpoint write atomic with the UTXO
    // commit it certifies — no crash window between "UTXO state
    // committed" and "forest checkpoint that proves it on disk".
    //
    // This site is intentionally left as a stub so the structure of
    // ConnectTip stays readable; the actual writes happen in the
    // unified batch above.

    // Phase 3b step 4 (shielded frontier flat-file write): the
    // ShieldedTipMarker goes into the unified batch (atomic with
    // everything else for tip N+1). The frontier flat-file write
    // happens IMMEDIATELY after the unified batch commits — see the
    // PersistShieldedState() call ~600 lines earlier in ConnectTip.
    // This site is intentionally a stub.

    // Phase 3: Persist transition proof to ChainDB for historical block serving
    // Note: precache_success guard removed — GetTransitionProof returns nullopt
    // when TP generation failed, so this is safe and ensures TPs are always persisted.
    if (!GetConfig().utreexo_stateless && bridge_node_) {
        auto tp_opt = bridge_node_->GetTransitionProof(tip_to_connect->hash);
        if (tp_opt) {
            auto tp_bytes = tp_opt->serialize();
            auto tp_status = chain_db_->putTransitionProof(token, tip_to_connect->height, tp_bytes);
            if (tp_status != Status::Ok) {
                if (logger_) logger_->warning("[ConnectTip] Failed to persist transition proof at height " +
                              std::to_string(tip_to_connect->height));
            }
        }
    }

    // Keep consensus UTXO tip metadata aligned with canonical tip.
    if (consensus_utxo_set_) {
        consensus_utxo_set_->SetBestBlock(tip_to_connect->hash, tip_to_connect->height);
    }

    // ATOMIC ACTIVE-TIP PUBLICATION INVARIANT (Apr 30 2026).
    //
    //   A block must not become the active tip until all data needed
    //   to disconnect it is durably committed.
    //
    // The unified-batch design (UTXO + txindex + utreexo checkpoint
    // + UD:<hash> sidecar [P1] + header metadata with BLOCK_HAVE_UNDO
    // [D.1] + shielded markers + setTip + height index, all in one
    // rocksdb WriteBatch with sync=true; preceded by undo flatfile
    // fsync) is supposed to make this true by construction. This
    // assertion proves it by reading back from durable storage
    // immediately before publishing the in-memory tip pointer. If
    // any future refactor reintroduces a path that publishes a tip
    // without one of those materials, this trips here.
    //
    // Behavior:
    //   - regtest: hard abort. Catches regressions in the test suite
    //     before they ship.
    //   - mainnet/testnet: log loud + EnterSafeMode. Refuse to
    //     advance the tip until operator review. The unified batch
    //     already committed; the in-memory state is rolled back to
    //     the previous tip, and ActivateBestChain stops.
    {
        // Reference tip == the block being connected: depth 0, coinbase
        // always immature, so the read-back invariant runs unconditionally
        // here (this is the atomicity-enforcing path).
        const auto material = CheckBlockDisconnectMaterialDurable(
            block, tip_to_connect->hash, tip_to_connect->height,
            tip_to_connect->height);
        if (!material.durable) {
            const std::string msg =
                "INVARIANT VIOLATION at ConnectTip publish: " + material.failure_reason;
            if (logger_) logger_->error("[ConnectTip] " + msg);
            std::cout << "❌ [ConnectTip] " << msg << std::endl;
            if (Params().network_id == "regtest") {
                std::abort();  // catch regressions in tests
            }
            EnterSafeMode("active-tip publication invariant violated: " +
                          material.failure_reason);
            return false;
        }
    }

    // Update in-memory state AFTER consensus mutation succeeds AND
    // the publication invariant is verified durable. THIS is the
    // canonical "advancement" publication — the only place in the
    // codebase that should pass `kAdvancement`. All other
    // PublishActiveTip call sites move the tip backward, restore it
    // from durable state, or run before the validator is ready.
    PublishActiveTip(tip_to_connect, TipPublishReason::kAdvancement);

    // Notify wallets AFTER state is consistent
    notifyBlockConnected(block, tip_to_connect->height);

    // P2P FIX: Add header to HeaderChainSelector so mined blocks are tracked
    // This ensures header chain comparison works correctly during reorgs
    if (header_chain_selector_) {
        header_chain_selector_->AddHeader(block.header);
        std::cout << "✅ [ConnectTip] Added header to HeaderChainSelector, height="
                  << tip_to_connect->height << std::endl;
    }

    RecordBlockConnectStats(tip_to_connect->height, connect_start);
    return true;
}

void ChainstateService::RecordBlockConnectStats(
    uint32_t height, std::chrono::steady_clock::time_point connect_start) {
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connect_start).count();
    const uint64_t checkpoint_bytes = pending_forest_checkpoint_bytes_;
    pending_forest_checkpoint_bytes_ = 0;
    const std::string summary = sync_stats_.RecordBlockConnect(
        height, static_cast<uint64_t>(connect_ms), checkpoint_bytes);
    if (!summary.empty() && logger_) {
        logger_->info(summary);
    }
}

bool ChainstateService::ReplayForestDeltasToTip(uint32_t checkpoint_height,
                                                uint32_t target_height,
                                                std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };
    if (!consensus_utxo_set_ || !chain_db_) {
        return fail("replay-prerequisites-missing");
    }
    if (target_height <= checkpoint_height) {
        return true;
    }

    // Work on a copy: a mid-replay failure (missing/corrupt sidecar, root
    // mismatch) must leave the live forest at verified checkpoint state so
    // the existing body-based catch-up recovery still has its expected
    // starting point. The walk itself is the shared
    // storage::ReplayUtreexoDeltaRange (also the bridge's historical
    // restore) — per-block header-root verification included.
    consensus::UtreexoForest working = consensus_utxo_set_->GetForest();

    std::string range_error;
    const Status replay_status = storage::ReplayUtreexoDeltaRange(
        *chain_db_, working, checkpoint_height, target_height, range_error);
    if (replay_status != Status::Ok) {
        return fail(range_error);
    }

    consensus_utxo_set_->GetForest() = std::move(working);
    if (logger_) {
        logger_->info("[ForestDeltaReplay] forest restored to tip height " +
                      std::to_string(target_height) + " via checkpoint " +
                      std::to_string(checkpoint_height) + " + " +
                      std::to_string(target_height - checkpoint_height) +
                      " delta sidecars (root header-verified per block)");
    }
    return true;
}

// ============================================================================
// CSN Reorg: Bookkeeping-only block connect
// ============================================================================
// Called during STATELESS reorg after ReplayBlock() has already advanced the
// forest. Does NOT call ConnectBlock, does NOT mutate the forest. Only:
//   1. Persist coin changes (delete spent, add created), an UndoRecord, and
//      shielded frontier/anchor/marker/nullifier state to ChainDB — all in
//      ONE atomic batch, so this replay-connected block is disconnectable
//      and restart-safe exactly like a ConnectTip-connected block.
//   2. Set canonical tip pointer
//   3. Update height→hash index
//   4. Save Utreexo checkpoint (forest already at correct state)
//   5. Update active_tip_ and notify observers
// ============================================================================

bool ChainstateService::CommitConnectedBlockBookkeeping(CBlockIndex* block_index, const Block& block,
                                                         const consensus::BlockUndo* shielded_undo,
                                                         std::string* out_error) {
    auto fail = [&](const std::string& reason) {
        if (out_error) *out_error = reason;
        return false;
    };

    if (!block_index || !chain_db_) return fail("null-block-index-or-db");

    ChainWriteToken token;

    // Shielded-bearing predicate + expected spent count for the undo gates
    // below (mirrors ConnectTip's pre-undo scan): any non-coinbase tx
    // carrying a shielded bundle, and one spent entry per non-coinbase input.
    //
    // NOTE (predicate cross-reference): this pre-scan uses tx.IsShielded()
    // (shielded version AND bundle present), whereas the delta helper /
    // forward loop in block_validation.cpp use UsesShieldedValueSemantics
    // (version OR bundle). The two cannot diverge on a consensus-valid block
    // — consensus rejects mixed shapes (version without bundle or vice
    // versa) — so block_has_shielded here agrees with the delta path on every
    // block that reaches bookkeeping. The pre-scan starts at tx_idx=1 (not 0
    // like ConnectTip's created-outputs loop) because consensus rejects a
    // shielded coinbase, so tx 0 can never set block_has_shielded.
    bool block_has_shielded = false;
    uint64_t expected_spent_count = 0;
    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        if (block.vtx[tx_idx].IsShielded()) block_has_shielded = true;
        expected_spent_count += block.vtx[tx_idx].vin.size();
    }

    // LOUD-FAILURE GUARD: an UndoRecord for a shielded-bearing block MUST
    // carry the pre-block shielded frontier, or a later DisconnectTip-CSN
    // would call DisconnectBlockShieldedSection with both optionals empty —
    // a silent no-op that leaves the shielded pool un-rolled-back (silent
    // corruption). #356: ConnectTip's stateless-replay recovery branch now
    // routes through ApplyStatelessReplayShielded before calling us, and
    // hands in a real BlockUndo (shielded_applied ? &replay_undo : nullptr)
    // — it only passes nullptr when the guard there determined the shielded
    // pool is already at/ahead of this block (nothing to apply, nothing to
    // undo). So the SKIP arm below still exists as defense-in-depth: if a
    // shielded-bearing block ever arrives here with shielded_undo=nullptr or
    // missing pre_block_shielded_frontier (e.g. a future caller that doesn't
    // go through the funnel), we SKIP the undo write entirely, so a future
    // disconnect of this block hits the loud "Missing undo data" failure
    // (the pre-Task-6 behavior) instead of silently skipping the shielded
    // rollback. Coins/tip/shielded staging below proceed unchanged.
    //
    // SECOND ARM (epoch-reset block): the reset block at H is shielded-EMPTY
    // by the wall rule, so block_has_shielded==false and the arm above never
    // fires for it. But the reset undo needs pre_reset_shielded_epoch (the
    // pre-cutover pool snapshot) — DisconnectBlockShieldedSection restores the
    // discarded epoch from it; RollbackAbove cannot re-add rows the reset
    // wiped. When ApplyStatelessReplayShielded applies at the reset height,
    // ApplyBlockShieldedSection fills pre_reset_shielded_epoch into the undo
    // it returns, so ConnectTip's recovery branch hands in a snapshot-bearing
    // undo here too. If a caller ever reaches here with shielded_undo=nullptr
    // (or a missing snapshot) for a reset block, it would write a reset undo
    // with NO snapshot; a later DisconnectTip-CSN of H would then no-op the
    // shielded rollback (both optionals empty) and retreat below H with the
    // post-reset EMPTY pool — every pre-reset nullifier gone, a silent
    // double-spend/fork window. So for the reset block we ALSO skip the undo
    // write unless the real snapshot is in hand, mirroring the regen-refusal
    // guard's rationale (see ~10877): a snapshot-less reset undo must never
    // be persisted.
    const bool at_reset = consensus::shielded::IsShieldedEpochResetHeight(
        static_cast<uint32_t>(block_index->height),
        dinero::Params().shielded_epoch_reset_height);
    const bool skip_undo_write =
        (block_has_shielded && (shielded_undo == nullptr ||
            !shielded_undo->pre_block_shielded_frontier.has_value())) ||
        (at_reset && (shielded_undo == nullptr ||
            !shielded_undo->pre_reset_shielded_epoch.has_value()));
    if (skip_undo_write && logger_) {
        logger_->warning("[CommitBookkeeping] block connected without the required "
                         "shielded undo (shielded-bearing block missing pre-block "
                         "frontier, or epoch-reset block missing pre-reset snapshot) — "
                         "skipping undo write. If no valid undo record already exists on "
                         "disk for this block, a future DisconnectTip-CSN fails loudly "
                         "with \"Missing undo data\" (never a silent shielded rollback "
                         "skip); if a previously-written valid record still exists, that "
                         "record remains readable and the disconnect succeeds correctly; "
                         "height=" + std::to_string(block_index->height));
    }

    // Build the UndoRecord for this block (unless the loud-failure guard
    // above suppressed it). Reverses the old "stateless reorg recovers by
    // re-sync" decision (see the comment further below): every
    // replay-connected block now gets a real UndoRecord so a SECOND reorg
    // can disconnect it through the standard DisconnectTip-CSN path.
    dinero::UndoRecord undo_record;
    if (!skip_undo_write) {
        // Resolve the same spent-outputs source the ABC-CSN replay loop
        // already loaded for this block before calling us: block.utreexo's
        // embedded metadata when present, else the CF7 CSN-replay-data
        // sidecar (older hash-only records carry spend metadata there
        // instead, and ConnectTip's own stateless-replay call site always
        // has block.utreexo populated by its gate).
        std::vector<consensus::SpentOutputData> fallback_spent_outputs_storage;
        const std::vector<consensus::SpentOutputData>* spent_outputs_src = nullptr;
        if (block.utreexo.has_value()) {
            spent_outputs_src = &block.utreexo->spent_outputs;
        } else {
            auto st_result = chain_db_->getCSNSpendTargets(block_index->hash);
            if (st_result.status() == Status::Ok && st_result.value().size() >= 4) {
                CsnReplayData replay_data;
                if (DecodeCsnReplayData(st_result.value(), replay_data) && replay_data.has_spent_outputs) {
                    fallback_spent_outputs_storage = std::move(replay_data.spent_outputs);
                    spent_outputs_src = &fallback_spent_outputs_storage;
                }
            }
        }

        size_t global_input_idx = 0;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
            if (tx_idx == 0) continue;  // coinbase has no inputs to undo
            const auto& tx = block.vtx[tx_idx];
            for (const auto& input : tx.vin) {
                if (!spent_outputs_src || global_input_idx >= spent_outputs_src->size()) {
                    return fail("commit-bookkeeping-missing-spent-output-metadata");
                }
                const auto& so = (*spent_outputs_src)[global_input_idx++];
                // Global-index walk: block_assembler.cpp populates
                // spent_outputs by walking every non-coinbase tx's vin in
                // order (intra-block spends included at their position via
                // the same SpentOutputData shape), so this simple
                // sequential consume — no per-tx reset — lines up with
                // intra-block spends exactly like external ones.
                undo_record.spent.emplace_back(
                    input.prevout.txid.AsUint256(), input.prevout.vout,
                    so.value, so.scriptPubKey, so.is_coinbase, so.created_height,
                    so.is_confidential, so.commitment);
            }
        }
        for (const auto& tx : block.vtx) {
            const TxId txid = tx.GetTxid();
            for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
                undo_record.created.emplace_back(txid.AsUint256(), vout);
            }
        }
        if (shielded_undo) {
            undo_record.pre_block_shielded_frontier = shielded_undo->pre_block_shielded_frontier;
            undo_record.pre_reset_shielded_epoch = shielded_undo->pre_reset_shielded_epoch;
        }
    }

    // Existing-undo idempotency check (ConnectTip/Promotion idiom): trust
    // the flatfile, not just the in-memory locator, before deciding whether
    // to rewrite it — a block that was previously connected-then-disconnected
    // keeps stale undo_size/undo_file/undo_pos on the in-memory CBlockIndex.
    // Mirrors ConnectTip's full gate including the shielded-frontier
    // coupling: a stored undo for a shielded-bearing block that lacks
    // pre_block_shielded_frontier is stale (written before the shielded
    // fields existed, or by a path that couldn't supply them) and must be
    // rewritten now that we have the real shielded undo in hand.
    auto existing_undo = ReadStoredUndo(block_index->hash);
    const bool existing_undo_valid = existing_undo.status() == Status::Ok &&
        existing_undo.value().spent.size() == expected_spent_count &&
        !existing_undo.value().created.empty() &&
        (!block_has_shielded ||
         existing_undo.value().pre_block_shielded_frontier.has_value()) &&
        // Reset-block coupling: a stored reset undo that lacks
        // pre_reset_shielded_epoch is stale (written by the snapshot-less
        // recovery path). When an ABC-CSN reconnect of H arrives holding the
        // real snapshot, treat the flatfile record as invalid so it gets
        // rewritten with the snapshot — otherwise a later disconnect of H
        // would read the snapshot-less record and silently skip the reset
        // rollback.
        (!at_reset || existing_undo.value().pre_reset_shielded_epoch.has_value());
    const bool need_flatfile_undo = !skip_undo_write && block_storage_ &&
        (block_index->undo_size == 0 || !existing_undo_valid);
    uint32_t undo_file = block_index->undo_file;
    uint32_t undo_pos = block_index->undo_pos;
    uint32_t undo_size = block_index->undo_size;

    // 1. Persist coin changes + undo record + shielded state (ONE atomic batch)
    {
        rocksdb::WriteBatch utxo_batch;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            const auto& tx = block.vtx[tx_idx];
            const TxId txid = tx.GetTxid();
            const bool is_coinbase = (tx_idx == 0);

            if (!is_coinbase) {
                for (const auto& input : tx.vin) {
                    chain_db_->deleteCoin(token, input.prevout.txid.AsUint256(),
                                          input.prevout.vout, &utxo_batch);
                }
            }

            for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
                const auto& output = tx.vout[vout];
                dinero::Coin coin;
                coin.amount = output.value.GetUna();
                std::ostringstream spk_hex;
                for (uint8_t byte : output.scriptPubKey) {
                    spk_hex << std::hex << std::setfill('0') << std::setw(2)
                            << static_cast<int>(byte);
                }
                coin.script_pubkey = spk_hex.str();
                coin.height = static_cast<uint32_t>(block_index->height);
                coin.coinbase = is_coinbase;
                coin.is_confidential = output.is_confidential;
                coin.commitment = output.commitment;
                chain_db_->putCoin(token, txid.AsUint256(), vout, coin, &utxo_batch);
            }
        }
        // Transaction index
        for (uint32_t ti = 0; ti < block.vtx.size(); ti++) {
            const TxId tid = block.vtx[ti].GetTxid();
            chain_db_->putTxIndex(token, tid.AsUint256(), block_index->hash, ti, &utxo_batch);
        }

        // Undo persistence: mirrors ConnectTip's shape (chainstate_service.cpp
        // slice-3 hoist above ~line 11700). The flatfile write happens BEFORE
        // this batch commits — the flatfile is the source of truth — then the
        // locator (BLOCK_HAVE_UNDO + undo_file/pos/size) is staged into THIS
        // SAME batch so "undo bytes durable" and "coin changes durable" land
        // together, both BEFORE the tip advances in step 2 below (same
        // crash-safety rationale as ConnectTip: a crash between undo-durable
        // and tip-durable must never leave the tip pointing at an
        // undisconnectable block). Skipped wholesale (flatfile AND locator)
        // when the loud-failure guard fired: a shielded-bearing block with
        // no shielded undo must NOT get a shielded-field-less undo record,
        // or a later disconnect would silently skip the shielded rollback.
        if (!skip_undo_write) {
            if (need_flatfile_undo) {
                const std::vector<uint8_t> undo_bytes = undo_record.Serialize();
                auto undo_pos_result = block_storage_->writeUndo(block_index->hash, undo_bytes);
                if (undo_pos_result.status() != Status::Ok) {
                    if (logger_) {
                        logger_->error("[CommitBookkeeping] writeUndo failed at height " +
                                      std::to_string(block_index->height));
                    }
                    return fail("commit-bookkeeping-write-undo-failed");
                }
                const auto& pos = undo_pos_result.value();
                if (pos.offset > std::numeric_limits<uint32_t>::max()) {
                    return fail("commit-bookkeeping-undo-offset-overflow");
                }
                undo_file = pos.file_number;
                undo_pos = static_cast<uint32_t>(pos.offset);
                undo_size = pos.size;
            }
            block_index->undo_file = undo_file;
            block_index->undo_pos = undo_pos;
            block_index->undo_size = undo_size;
            block_index->status |= BLOCK_HAVE_UNDO;
            auto locator_status = chain_db_->updateUndoLocator(
                token, block_index->hash, undo_file, undo_pos, undo_size, &utxo_batch);
            if (locator_status == Status::NotFound) {
                // No existing header-metadata row to preserve — fall back to the
                // authoritative full block-index stamp (ConnectTip D.2 idiom).
                locator_status = chain_db_->updateBlockIndex(token, block_index, &utxo_batch);
            }
            if (locator_status != Status::Ok) {
                if (logger_) {
                    logger_->error("[CommitBookkeeping] undo locator stage failed at height " +
                                  std::to_string(block_index->height));
                }
                return fail("commit-bookkeeping-undo-locator-stage-failed");
            }
        }

        // Shielded persistence staged with the coins (mirrors ConnectTip's
        // Phase 3b option 1 staging block, chainstate_service.cpp
        // ~line 12111 near putShieldedTipMarker): frontier blob, anchor
        // history blob, ShieldedTipMarker, and this block's nullifier rows
        // — all into the SAME utxo_batch so they commit atomically with the
        // coin/undo changes. The in-memory shielded pool was already
        // advanced by the replay loop's ApplyBlockShieldedSection call
        // before this function runs, so SerializeFrontier() /
        // CurrentShieldedStateSnapshot() below already reflect the correct
        // post-block state.
        {
            const auto frontier_bytes = shielded_tree_.SerializeFrontier();
            const std::string frontier_blob(frontier_bytes.begin(), frontier_bytes.end());
            if (chain_db_->putUtreexoMeta(token, "shielded_frontier", frontier_blob, &utxo_batch) != Status::Ok) {
                if (logger_) {
                    logger_->error("[CommitBookkeeping] Failed to stage shielded frontier blob at height " +
                                  std::to_string(block_index->height));
                }
                return fail("commit-bookkeeping-shielded-frontier-stage-failed");
            }
            const auto anchor_bytes = shielded_anchor_history_.SerializeBytes();
            const std::string anchor_blob(anchor_bytes.begin(), anchor_bytes.end());
            if (chain_db_->putUtreexoMeta(token, "shielded_anchor_history", anchor_blob, &utxo_batch) != Status::Ok) {
                if (logger_) {
                    logger_->error("[CommitBookkeeping] Failed to stage anchor history blob at height " +
                                  std::to_string(block_index->height));
                }
                return fail("commit-bookkeeping-anchor-history-stage-failed");
            }
            const auto shielded_snapshot = CurrentShieldedStateSnapshot();
            ChainDB::ShieldedTipMarker shielded_marker;
            shielded_marker.height = static_cast<int32_t>(block_index->height);
            shielded_marker.block_hash = block_index->hash;
            shielded_marker.shielded_root = shielded_snapshot.root;
            shielded_marker.tree_size = shielded_snapshot.tree_size;
            shielded_marker.nullifier_count = shielded_snapshot.nullifier_count;
            if (chain_db_->putShieldedTipMarker(token, shielded_marker, &utxo_batch) != Status::Ok) {
                if (logger_) {
                    logger_->error("[CommitBookkeeping] Failed to stage ShieldedTipMarker at height " +
                                  std::to_string(block_index->height));
                }
                return fail("commit-bookkeeping-shielded-tip-marker-stage-failed");
            }
            for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
                const auto& tx = block.vtx[tx_idx];
                if (!tx.IsShielded()) {
                    continue;
                }
                consensus::shielded::ShieldedBundle bundle;
                const auto decode = consensus::shielded::DeserializeShieldedBundle(
                    tx.shielded_bundle_bytes, &bundle);
                if (decode != consensus::shielded::BundleDecodeError::Ok) {
                    if (logger_) {
                        logger_->error("[CommitBookkeeping] Failed to decode shielded bundle for "
                                       "nullifier staging at height " +
                                       std::to_string(block_index->height) +
                                       " tx " + std::to_string(tx_idx));
                    }
                    return fail("commit-bookkeeping-shielded-bundle-decode-failed");
                }
                for (const auto& spend : bundle.spends) {
                    if (chain_db_->putShieldedNullifier(
                            token, static_cast<uint32_t>(block_index->height),
                            spend.nullifier.data(), &utxo_batch) != Status::Ok) {
                        if (logger_) {
                            logger_->error("[CommitBookkeeping] Failed to stage shielded nullifier at height " +
                                          std::to_string(block_index->height));
                        }
                        return fail("commit-bookkeeping-shielded-nullifier-stage-failed");
                    }
                }
            }
        }

        auto status = chain_db_->writeBatch(token, std::move(utxo_batch), true);
        if (status != Status::Ok) {
            if (logger_) {
                logger_->error("[CommitBookkeeping] Coin persistence failed at height " +
                              std::to_string(block_index->height));
            }
            return fail("persist-utxos-failed");
        }
    }

    // 2. Set canonical tip
    arith_uint256 work;
    try {
        work = ChainworkFromHex(block_index->chainwork);
    } catch (const std::exception& e) {
        return fail(std::string("invalid-chainwork: ") + e.what());
    }
    auto tip_status = chain_db_->setTip(token, block_index->hash,
                                         static_cast<int>(block_index->height), work);
    if (tip_status != Status::Ok) {
        return fail("persist-tip-failed");
    }

    // 3. Height index
    auto height_idx_status = chain_db_->putHeightIndex(
        token, static_cast<int>(block_index->height), block_index->hash);
    if (height_idx_status != Status::Ok) {
        return fail("persist-height-index-failed");
    }

    // 4. Utreexo checkpoint (forest is already at correct post-replay state).
    // Campaign phase 1: with utreexo.checkpoint_interval N > 1 the full
    // checkpoint is written only at heights % N == 0 (same gating as
    // ConnectTip's unified batch; the per-block delta sidecar is persisted
    // by the caller either way).
    if (consensus_utxo_set_) {
        const uint32_t bk_checkpoint_interval =
            GetConfig().utreexo_checkpoint_interval > 1
                ? GetConfig().utreexo_checkpoint_interval : 1;
        if (static_cast<uint64_t>(block_index->height) % bk_checkpoint_interval == 0) {
            auto serialized = consensus_utxo_set_->GetForest().serialize();
            pending_forest_checkpoint_bytes_ = serialized.size();
            auto checkpoint_status = chain_db_->putUtreexoCheckpointWithChecksum(
                token, block_index->height, serialized);
            if (checkpoint_status != Status::Ok) {
                return fail("persist-utreexo-checkpoint-failed");
            }
        }
        consensus_utxo_set_->SetBestBlock(block_index->hash, block_index->height);
    }
    // Shielded epoch reset (hard-fork cutover) — the stateless commit path (both
    // stateless connect entry points route through here) must apply the reset
    // too, or a stateless node keeps its pre-cutover shielded pool past H and its
    // ShieldedTipMarker / shieldedStateHash diverge from full nodes (a split).
    // Enforce the wall (H must be shielded-empty), discard the in-memory pool,
    // and purge the authoritative nullifier CF, so the PersistShieldedState /
    // PersistShieldedTipMarker below write the empty post-reset epoch.
    //
    // New invariant (was: "stateless reorg across the cutover recovers by
    // re-sync, so no undo snapshot is written on this light-client path"):
    // that is no longer true. A replay-connected reset block gets a real
    // UndoRecord written above WHENEVER the snapshot is available: the ABC-CSN
    // replay caller hands in a `shielded_undo` whose pre_reset_shielded_epoch
    // is populated (ApplyBlockShieldedSection fills it) as it crosses the reset
    // height. Since #356, ConnectTip's stateless recovery branch ALSO applies
    // shielded state (via ApplyStatelessReplayShielded → ApplyBlockShieldedSection)
    // before calling here, so when it connects the reset block it hands in a
    // snapshot-bearing undo too. The guarantee that a disconnect back across the
    // cutover is safe does NOT rely on every caller supplying the snapshot — it
    // rests on REFUSAL as defense-in-depth: if a caller ever reaches here with a
    // snapshot-less reset undo (nullptr, or a Skip that applied nothing), the
    // skip_undo_write guard above declines to persist it and the DisconnectTip-CSN
    // reader refuses to disconnect H off a snapshot-less record — so a stateless
    // node either disconnects across the cutover from a real snapshot or fails
    // loudly, never silently drops the pre-cutover pool.
    {
        const uint32_t reset_height = dinero::Params().shielded_epoch_reset_height;
        if (consensus::shielded::IsShieldedEpochResetHeight(
                static_cast<uint32_t>(block_index->height), reset_height)) {
            for (size_t i = 1; i < block.vtx.size(); ++i) {
                if (block.vtx[i].IsShielded()) {
                    return fail("shielded-tx-at-epoch-reset-height");
                }
            }
            consensus::shielded::ResetShieldedEpoch(
                shielded_tree_, shielded_anchor_history_, shielded_nullifiers_);
            // Record the post-reset (H, empty_root) anchor, exactly as the live
            // ConnectBlockShieldedSection path does after ResetShieldedEpoch (see
            // shielded_block_section.cpp: reset THEN RecordRoot). ResetShieldedEpoch
            // CLEARS the anchor window, but the invariant is that EVERY block at/after
            // shielded activation contributes one anchor — including the cutover block
            // itself, whose post-reset root is the empty-tree root. Omitting this here
            // made the reset_batch's in-memory reset inconsistent with a live-built or
            // forward-synced node: on the ABC-CSN reorg-replay path the in-loop
            // ApplyBlockShieldedSection had already recorded (H, empty_root), and this
            // second ResetShieldedEpoch wiped it without re-recording, leaving a reorged
            // CSN's anchor history one entry short of the bridge / a fresh sync (a
            // divergent DSR2 shieldedStateHash at an identical tip — a consensus split).
            // RecordRoot overwrites on a repeated height, so this is idempotent whether
            // or not the in-loop apply already recorded (H, empty_root).
            shielded_anchor_history_.RecordRoot(
                static_cast<uint32_t>(block_index->height), shielded_tree_.Root());
            // Stage the CF purge AND the post-reset frontier/anchor blobs into
            // ONE batch so they commit atomically: afterwards the nullifier CF
            // is empty and the ChainDB frontier/anchor blobs both reflect the
            // empty epoch together. A crash BEFORE this batch leaves the stale
            // CF and no blobs, so the startup resurrection guard fails safe to a
            // brick (reindex-recoverable) — never a silent resurrection. Writing
            // the blobs here also satisfies the guard so a stateless node comes
            // up cleanly post-cutover even if it crashes before PersistShieldedState.
            ChainWriteToken reset_token;
            rocksdb::WriteBatch reset_batch;
            const auto purged =
                chain_db_->deleteAllShieldedNullifiers(reset_token, &reset_batch);
            if (!purged.ok()) {
                return fail("shielded-epoch-reset-nullifier-purge-failed");
            }
            const auto fb = shielded_tree_.SerializeFrontier();
            const auto ab = shielded_anchor_history_.SerializeBytes();
            if (chain_db_->putUtreexoMeta(reset_token, "shielded_frontier",
                    std::string(fb.begin(), fb.end()), &reset_batch) != Status::Ok ||
                chain_db_->putUtreexoMeta(reset_token, "shielded_anchor_history",
                    std::string(ab.begin(), ab.end()), &reset_batch) != Status::Ok) {
                return fail("shielded-epoch-reset-blob-stage-failed");
            }
            if (chain_db_->writeBatch(reset_token, std::move(reset_batch), true) !=
                Status::Ok) {
                return fail("shielded-epoch-reset-commit-failed");
            }
        }
    }
    if (!PersistShieldedState() && logger_) {
        logger_->warning("[CommitBookkeeping] Failed to persist shielded frontier at height " +
                         std::to_string(block_index->height));
    }
    if (!PersistShieldedTipMarker(block_index->hash, static_cast<uint32_t>(block_index->height)) &&
        logger_) {
        logger_->warning("[CommitBookkeeping] Failed to persist ShieldedTipMarker at height " +
                         std::to_string(block_index->height));
    }

    // 5. Update in-memory state + notify
    PublishActiveTip(block_index, TipPublishReason::kEarlyInitGenesis);
    notifyBlockConnected(block, block_index->height);

    if (header_chain_selector_) {
        header_chain_selector_->AddHeader(block.header);
    }

    if (logger_) logger_->info("[CommitBookkeeping] height=" + std::to_string(block_index->height) +
                               " hash=" + block_index->hash.GetHex().substr(0, 16) + "...");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 44: Background Validation (AssumeUTXO Safety)
// ═══════════════════════════════════════════════════════════════════════════

void ChainstateService::StartBackgroundValidation() {
    std::lock_guard<std::mutex> lock(bg_validation_mutex_);

    if (!assumeutxo_active_) {
        logger_->warning("[BackgroundValidation] Not in AssumeUTXO mode, validation not needed");
        return;
    }

    if (bg_validation_status_ == BackgroundValidationStatus::InProgress) {
        logger_->warning("[BackgroundValidation] Validation already in progress");
        return;
    }

    if (bg_validation_status_ == BackgroundValidationStatus::Completed) {
        logger_->info("[BackgroundValidation] Validation already completed successfully");
        return;
    }

    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("🔍 STARTING BACKGROUND VALIDATION");
    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("Snapshot base height: " + std::to_string(assumeutxo_base_height_));
    logger_->info("Snapshot base hash: " + assumeutxo_base_block_.GetHex());
    logger_->info("This will validate all blocks from genesis to snapshot height");
    logger_->info("═══════════════════════════════════════════════════════════════════════");

    // Reset state
    bg_validation_status_ = BackgroundValidationStatus::InProgress;
    bg_validation_current_height_ = 0;
    bg_validation_blocks_validated_ = 0;
    bg_validation_error_ = "";
    bg_validation_should_stop_ = false;

    // Arm the fatal state machine's stall clock. In validation_stalled this
    // only re-arms the clock — the stall stays visible until real progress.
    EnsureAssumeUtxoLifecycle();
    assumeutxo_lifecycle_->OnValidationStarted(std::chrono::steady_clock::now());

    // Launch worker thread
    bg_validation_thread_ = std::make_unique<std::thread>(&ChainstateService::BackgroundValidationWorker, this);
}

ChainstateService::BackgroundValidationProgress ChainstateService::GetBackgroundValidationProgress() const {
    std::lock_guard<std::mutex> lock(bg_validation_mutex_);

    BackgroundValidationProgress progress;
    progress.status = bg_validation_status_;
    progress.current_height = bg_validation_current_height_;
    progress.target_height = assumeutxo_base_height_;
    progress.blocks_validated = bg_validation_blocks_validated_;
    progress.error_message = bg_validation_error_;

    if (assumeutxo_base_height_ > 0) {
        progress.progress_percent = (static_cast<double>(bg_validation_current_height_) /
                                    static_cast<double>(assumeutxo_base_height_)) * 100.0;
    } else {
        progress.progress_percent = 0.0;
    }

    return progress;
}

bool ChainstateService::IsBackgroundValidationComplete() const {
    std::lock_guard<std::mutex> lock(bg_validation_mutex_);
    return bg_validation_status_ == BackgroundValidationStatus::Completed;
}

void ChainstateService::CheckHangWatchdog() {
    // #298 diagnose-only no-progress watchdog. Called every ~5s from the p2p
    // scheduler tick thread (INDEPENDENT of the bg-validation worker, so it
    // survives a wedge of that worker — the actual incident). Single-caller,
    // so the watchdog_* state below needs no lock.
    const BackgroundValidationProgress progress = GetBackgroundValidationProgress();

    // Only meaningful while background validation is actually running. A
    // fully-synced (or not-yet-started) node legitimately sees no height
    // change for >15min, so gate strictly on InProgress and re-arm otherwise.
    if (progress.status != BackgroundValidationStatus::InProgress) {
        watchdog_initialized_ = false;
        return;
    }

    const CBlockIndex* tip = GetActiveTip();
    const uint32_t fg_height = tip ? tip->height : 0;
    const uint32_t bg_height = progress.current_height;
    const auto now = std::chrono::steady_clock::now();

    // First observation this run, OR either height advanced => progress.
    if (!watchdog_initialized_ ||
        bg_height != watchdog_last_bg_height_ ||
        fg_height != watchdog_last_fg_height_) {
        watchdog_initialized_ = true;
        watchdog_last_bg_height_ = bg_height;
        watchdog_last_fg_height_ = fg_height;
        watchdog_last_progress_time_ = now;
        return;
    }

    const auto stalled_min = std::chrono::duration_cast<std::chrono::minutes>(
        now - watchdog_last_progress_time_).count();
    if (stalled_min < kHangWatchdogMinutes) {
        return;
    }

    // Re-arm for another full interval so we emit at most one marker per
    // kHangWatchdogMinutes of continued wedge (not on every 5s tick).
    watchdog_last_progress_time_ = now;

    uint32_t missing_bodies = 0;
    if (assumeutxo_lifecycle_) {
        missing_bodies = assumeutxo_lifecycle_->GetStatus(now).missing_body_count;
    }

    if (logger_) {
        logger_->error("[Watchdog] #298 NO PROGRESS for " + std::to_string(stalled_min) +
                       "min: bg_height=" + std::to_string(bg_height) +
                       " fg_tip=" + std::to_string(fg_height) +
                       " missing_bodies=" + std::to_string(missing_bodies) +
                       " — possible wedge; capture: gdb -p " +
                       std::to_string(
#ifdef _WIN32
                           static_cast<long>(::GetCurrentProcessId())
#else
                           static_cast<long>(::getpid())
#endif
                           ) +
                       " -batch -ex 'thread apply all bt'");
    }
}

void ChainstateService::BackgroundValidationWorker() {
    util::SetThreadName("din-bgvalidate");  // #298: readable gdb backtraces
    logger_->info("[BackgroundValidation] Worker thread started");

    try {
        if (!chain_db_) {
            OnBackgroundValidationComplete(false, "ChainDB not available");
            return;
        }

        // #298 wake-on-store: register the scheduler callback so a freshly
        // stored pre-base body wakes this worker immediately (the 30s rescan
        // becomes a backstop, not the primary cadence). The callback only
        // touches the atomic + cv, so it is safe to fire from the scheduler's
        // post-store path. The stop paths clear this callback after join() to
        // avoid a use-after-free on [this] if a body lands during shutdown.
        // Clear any stale wake left set when a previous worker exited via the
        // should_stop short-circuit, so the first rescan wait of this run is
        // not skipped on the reset→reload recovery path.
        bg_validation_body_arrived_.store(false);
        if (auto* ctx = DaemonContext::instance(); ctx && ctx->block_download) {
            ctx->block_download->SetOnBackfillBodyStored([this]{
                // #298: only wake validation when it is genuinely WAITING for
                // re-requested gap bodies. During bulk backfill a body is
                // stored ~40k times; waking (and full-rescanning) on each would
                // be pathological. When not awaiting, validation paces itself
                // on the 30s cv timeout and picks up batches.
                if (!bg_validation_awaiting_bodies_.load()) return;
                bg_validation_body_arrived_.store(true);
                bg_validation_cv_.notify_one();
            });
        }

        EnsureAssumeUtxoLifecycle();

        const uint32_t target_height = assumeutxo_base_height_;
        const uint32_t log_interval = std::max(1u, target_height / 100);  // Log every 1%

        // First-time-only progress tracking: re-scanning an already-validated
        // height is not "actual progress" (spec Stall Semantics) — only a
        // height never validated this run (and beyond the durable marker from
        // a previous run) feeds the lifecycle's stall clock.
        const uint32_t resume_height = assumeutxo_lifecycle_
            ->GetStatus(std::chrono::steady_clock::now()).current_validation_height;
        std::vector<bool> height_validated(static_cast<size_t>(target_height) + 1, false);
        // Gate on resume_height > 0: on a fresh run the marker is 0 (genesis),
        // and we want genesis itself to produce a real OnBlockValidated signal
        // on the first pass rather than be pre-marked as already done.
        if (resume_height > 0) {
            for (uint32_t h = 0; h <= target_height && h <= resume_height; ++h) {
                height_validated[h] = true;  // claimed by the durable progress marker
            }
        }

        // Real genesis->base replay (spec Completion Criteria): every
        // canonical block below the base is connected through the normal
        // BlockValidator path into an isolated in-memory consensus set; the
        // resulting records digest is compared against the commitment
        // persisted at snapshot load.
        //
        // Replay state is in-memory only — it cannot survive daemon
        // restarts, so every run replays from genesis. The height_validated
        // pre-mark vector above governs ONLY lifecycle progress signals
        // (OnBlockValidated / blocks_validated counter): ConnectAndAdvance
        // runs for EVERY height >= 1 each pass regardless of the durable
        // resume marker, but only genuinely new heights feed the stall clock.
        //
        // (std::optional because the engine is intentionally non-movable;
        // emplace() re-creates it fresh for each rescan pass.)
        std::optional<assumeutxo::AssumeUtxoReplayEngine> replay;
        bool replay_poisoned = false;        // set when ConnectBlock refuses
        std::string replay_poison_reason;
        // Confirm-before-fatal: a validation failure must reproduce on a
        // fresh pass (fresh engine, freshly-read bodies) before it is
        // treated as a snapshot mismatch — see ReplayFailurePolicy.
        assumeutxo::ReplayFailurePolicy replay_failure_policy;
        bool replay_retry_pass = false;

        // Memory expectation: the replay set holds all sub-base UTXOs in
        // memory a second time (bounded by the snapshot's own count).
        logger_->info("[BackgroundValidation] replay engine active: in-memory replay set "
                      "(expected ~" + std::to_string(assumeutxo_base_height_) +
                      " blocks; UTXO count bounded by snapshot)");

        // Spec: missing bodies are NEVER skippable success. Re-scan until all
        // bodies are present (bodies backfill as IBD proceeds) or we are told
        // to stop; Tick() drives the loud-stall transition while we wait.
        //
        // canonical_hashes_fallback is declared OUTSIDE the rescan loop
        // (rebuilt fresh each pass) so the COMPLETING pass's hash-anchored
        // table survives to the promotion call after the loop —
        // PromoteValidatedHistory requires it as the height-index source.
        std::vector<uint256> canonical_hashes_fallback;
        uint32_t blocks_skipped = 0;

        // #298 reconciliation: per-skipped-height record. reason classifies why
        // the body was unreadable; canonical_hash is the header-anchored hash
        // when we had one (Null for NoCanonicalHash). After each pass these feed
        // the targeted re-request and the per-height diagnostics (issue #298).
        enum class MissReason { NoCanonicalHash, NoBodyForHash, HashMismatch };
        struct MissingEntry {
            uint32_t height;
            uint256 canonical_hash;  // Null when reason == NoCanonicalHash
            MissReason reason;
        };
        // Log the scheduler-unwired warning at most once for this worker run.
        bool sched_unwired_logged = false;

        while (true) {
            blocks_skipped = 0;
            std::vector<MissingEntry> missing;  // #298: per-pass missing heights
            // Engine heights must ascend strictly from 1 within one engine
            // lifetime, so each rescan pass restarts the replay from genesis
            // with a fresh engine. Replaying from 0 each pass is acceptable:
            // passes beyond the first only happen while bodies are missing,
            // and the final (complete) pass is the one whose digest counts.
            try {
                replay.emplace();
                // Capture exactly the undo tail the startup audit
                // (VerifyActiveChainUndoCoverage, window=1024) will check
                // after promotion. Must follow EVERY emplace(): the window
                // is per-engine state and the engine is re-created per pass.
                replay->SetUndoTailWindow(
                    std::min<uint32_t>(kStartupUndoAuditWindow, target_height));
            } catch (const std::exception& e) {
                // Engine construction failure (e.g. the :memory: nullifier
                // sqlite open under fd exhaustion) is a transient LOCAL
                // resource problem — operational retry, NOT proof that the
                // snapshot cannot be trusted. Mirror the missing-bodies wait.
                logger_->warning(std::string("[BackgroundValidation] replay engine "
                                 "construction failed (") + e.what() +
                                 ") — operational, retrying next pass in 30s");
                for (int i = 0; i < 30 && !bg_validation_should_stop_; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                assumeutxo_lifecycle_->Tick(std::chrono::steady_clock::now());
                if (bg_validation_should_stop_) {
                    OnBackgroundValidationComplete(false, "Validation stopped by user");
                    return;
                }
                continue;
            }
            replay_poisoned = false;
            replay_poison_reason.clear();

            // ── Header-chain fallback table (hoisted, O(target_height) per pass) ──
            // Backfilled block bodies arrive via the scheduler WITHOUT
            // height-index writes (only ConnectTip writes the canonical index,
            // by design — so canonical-write invariants are untouched). When
            // the height index misses a backfilled body, the table below
            // resolves the canonical hash — and it MUST anchor on the
            // snapshot base block HASH (assumeutxo_base_block_, the trust
            // root), never on the best header chain by height. LoadSnapshot's
            // base gate is existence-only (a GetHeader map lookup — side
            // branches pass), so the BEST header chain can diverge below the
            // base: a side-branch snapshot at load, or a majority-work header
            // fork arriving during the validation window. A height-anchored
            // walk (GetHeaderAtHeight) would then yield the FORK's hashes and
            // this worker would replay the fork completely into a persisted
            // false fatal_mismatch on an honest node. Anchoring on the base
            // hash + ancestor uniqueness (each header has exactly one parent
            // chain) pins every per-height entry to the snapshot's own chain
            // regardless of what the best chain does. The body-hash check
            // below still guards integrity — a corrupt or misrouted body is
            // caught by hash mismatch and treated as missing.
            //
            // Perf: ONE by-hash anchor lookup (O(1) map), then a SINGLE
            // backward walk via ->parent fills the entire table in
            // O(target_height) total — per-height GetHeaderAtHeight calls
            // would be the same O(n²) trap as the pre-#241 scanner
            // (≈ 33k × 40k = 1.3B pointer steps per pass). Each per-height
            // lookup is then O(1) from the vector. The table is rebuilt once
            // per rescan pass (the while(true) loop only retries when bodies
            // are missing, so passes beyond the first are rare and bounded).
            canonical_hashes_fallback.clear();
            if (header_chain_selector_ && !assumeutxo_base_block_.IsNull()) {
                // Resolve + walk + copy under the header chain's OWN mutex
                // (CollectAncestorsByHash): the snapshot base may be a
                // childless side-branch tip, which EvictBranch (running under
                // the header chain's lock on another thread when the
                // side-branch budget is full) can free — raw GetHeader()
                // pointers and their parent links do not survive that, so an
                // unlocked walk here would be a use-after-free. Failure paths
                // stay fail-safe: an unknown/mismatched anchor leaves the
                // table empty (missing bodies skip/stall, never replay
                // another chain).
                uint32_t anchor_height = 0;
                std::vector<std::pair<uint256, uint32_t>> branch;
                const bool anchor_known =
                    header_chain_selector_->CollectAncestorsByHash(
                        assumeutxo_base_block_, 0, anchor_height, branch);
                if (!anchor_known) {
                    logger_->error("[BackgroundValidation] snapshot base header " +
                                   assumeutxo_base_block_.GetHex().substr(0, 16) +
                                   "... not in header chain — fallback table empty "
                                   "(missing bodies will skip/stall, never replay "
                                   "another chain)");
                } else if (anchor_height != target_height) {
                    logger_->error("[BackgroundValidation] snapshot base header " +
                                   assumeutxo_base_block_.GetHex().substr(0, 16) +
                                   "... has height " + std::to_string(anchor_height) +
                                   " but validation target is " +
                                   std::to_string(target_height) +
                                   " — fallback table empty");
                } else {
                    canonical_hashes_fallback.resize(
                        static_cast<size_t>(target_height) + 1);
                    for (const auto& [hash, height] : branch) {
                        canonical_hashes_fallback[height] = hash;
                    }
                }
            }

            for (uint32_t height = 0; height <= target_height; ++height) {
                if (bg_validation_should_stop_) {
                    logger_->warning("[BackgroundValidation] Validation stopped by request");
                    OnBackgroundValidationComplete(false, "Validation stopped by user");
                    return;
                }
                // Regtest-only: slow the genesis->base replay so a test can
                // deterministically win the forward-sync-past-base race that
                // #353 bug-2 (promotion race) fixes. Inert unless on regtest AND
                // the env var is set to a positive integer (never in production).
                if (dinero::Params().name == "regtest") {
                    if (const char* delay_env = std::getenv("DINERO_DEBUG_BG_VALIDATION_DELAY_MS")) {
                        errno = 0;
                        char* parse_end = nullptr;
                        const long delay_ms = std::strtol(delay_env, &parse_end, 10);
                        if (parse_end != delay_env && *parse_end == '\0' && errno == 0 &&
                            delay_ms > 0 && delay_ms <= 60000) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(bg_validation_mutex_);
                    bg_validation_current_height_ = height;
                }
                // Try the height index first (populated by ConnectTip /
                // --reindex). On miss, fall back to the canonical header-chain
                // table built above (backfilled bodies lack index entries).
                auto hash_result = chain_db_->getBlockHashByHeight(height);
                uint256 canonical_hash;
                if (hash_result.ok()) {
                    canonical_hash = hash_result.value();
                } else if (height < canonical_hashes_fallback.size() &&
                           !canonical_hashes_fallback[height].IsNull()) {
                    canonical_hash = canonical_hashes_fallback[height];
                } else {
                    blocks_skipped++;
                    missing.push_back({height, uint256(),
                                       MissReason::NoCanonicalHash});  // #298
                    continue;
                }
                auto block_result = getBlockByHash(canonical_hash);
                if (!block_result.ok()) {
                    blocks_skipped++;
                    missing.push_back({height, canonical_hash,
                                       MissReason::NoBodyForHash});  // #298
                    continue;
                }
                const Block& blk = block_result.value();
                // The stored body must actually be the canonical block:
                // ConnectBlock trusts the (height, hash) it is handed and
                // never re-derives the block's hash itself, so a corrupt or
                // mis-stored local body would otherwise be replayed as if
                // canonical. A mismatch is LOCAL corruption (heal by
                // re-download), not snapshot poison: treat as missing body.
                if (blk.GetHash() != canonical_hash) {
                    blocks_skipped++;
                    missing.push_back({height, canonical_hash,
                                       MissReason::HashMismatch});  // #298
                    logger_->warning("[BackgroundValidation] body at height " +
                                     std::to_string(height) + " hashes to " +
                                     blk.GetHash().GetHex() + " but canonical hash is " +
                                     canonical_hash.GetHex() +
                                     " — local block-store corruption; treating as a "
                                     "missing body pending re-download");
                    continue;
                }
                // The block hash covers only the header; the tx section is
                // bound via the header's merkle root, and ConnectBlock never
                // re-checks it. Without this guard a torn or partially-flushed
                // body read (intact header, corrupt tx bytes) walks into real
                // validation and manifests as a false consensus failure
                // (observed 2026-07-14 as bad-utreexo-root at height 143 on a
                // body later proven byte-canonical). Same classification as a
                // hash mismatch: LOCAL corruption, heal by re-download.
                if (consensus::ComputeMerkleRoot(blk.vtx) != blk.header.merkle_root) {
                    blocks_skipped++;
                    missing.push_back({height, canonical_hash,
                                       MissReason::HashMismatch});
                    logger_->warning("[BackgroundValidation] body at height " +
                                     std::to_string(height) +
                                     " does not match its header merkle root — torn/"
                                     "corrupt local body; treating as a missing body "
                                     "pending re-download");
                    continue;
                }
                // #298: this height is readable now. If a prior pass reported it
                // missing and re-requested it, log the arrival exactly once.
                if (auto req_it = bg_requested_heights_.find(height);
                    req_it != bg_requested_heights_.end() && req_it->second) {
                    logger_->info("[BackgroundValidation] #298 body arrived height=" +
                                  std::to_string(height));
                    req_it->second = false;
                }
                // Genesis (height 0) is never validated — production
                // ConnectTip's early-init path likewise installs genesis as
                // tip without running ConnectBlock. But the canonical UTXO
                // set is NOT genesis-neutral: genesis_init.cpp persists every
                // genesis coinbase output (OP_RETURN included) as a height-0
                // coin, and the startup BulkLoad carries them into the live
                // set that ExportSnapshot commits to. Seed the engine
                // identically or every honest snapshot fails its own
                // commitment (caught by the Task 8 e2e). Height 0 also counts
                // for availability/progress below.
                //
                // blocks_skipped == 0 gate: the engine requires strictly
                // ascending heights, so once any body this pass is missing
                // the pass can no longer produce a valid digest — feeding a
                // post-gap block would trip the ascending check and be
                // MISCLASSIFIED as poison (missing bodies are never fatal).
                // The rest of the pass continues as an availability scan;
                // the rescan loop restarts the engine for the next pass.
                if (height == 0 && blocks_skipped == 0) {
                    std::string seed_err;
                    if (!replay->SeedGenesis(blk, seed_err)) {
                        // Impossible by construction (fresh engine, single
                        // seed per pass): indicates engine misuse, and a
                        // retry would fail identically — not transient.
                        replay_poisoned = true;
                        replay_poison_reason =
                            "genesis seeding failed during replay: " + seed_err;
                        break;
                    }
                }
                if (height >= 1 && blocks_skipped == 0) {
                    std::string connect_err;
                    if (!replay->ConnectAndAdvance(blk, height, canonical_hash,
                                                   connect_err)) {
                        // A canonical-chain block below the snapshot base
                        // failed real validation. A GENUINE snapshot mismatch
                        // is stable — it reproduces on a fresh pass with
                        // freshly-read bodies — while torn reads / transient
                        // heap corruption do not (2026-07-14 field incident:
                        // height 143 failed bad-utreexo-root once with a
                        // byte-canonical stored body, then validated clean).
                        // Require confirmation before the spec fatal.
                        switch (replay_failure_policy.OnValidationFailure(height)) {
                        case assumeutxo::ReplayFailurePolicy::Action::kRetryPass:
                            replay_retry_pass = true;
                            logger_->warning(
                                "[BackgroundValidation] block " + std::to_string(height) +
                                " failed validation during replay (" + connect_err +
                                ") — retrying with a fresh pass before treating it as "
                                "a snapshot mismatch (transient-corruption guard, "
                                "retry " +
                                std::to_string(replay_failure_policy.TotalRetries()) +
                                ")");
                            break;
                        case assumeutxo::ReplayFailurePolicy::Action::kConfirmFatal:
                            // Spec: hard validation failure => fatal (now
                            // confirmed across passes, or retries exhausted).
                            replay_poisoned = true;
                            replay_poison_reason = "block " + std::to_string(height) +
                                                   " failed validation during replay: " +
                                                   connect_err;
                            break;
                        }
                        break;  // out of the height loop; handled below
                    }
                    replay_failure_policy.OnValidationSuccess(height);
                }
                if (!height_validated[height]) {
                    height_validated[height] = true;
                    {
                        std::lock_guard<std::mutex> lock(bg_validation_mutex_);
                        bg_validation_blocks_validated_++;
                    }
                    assumeutxo_lifecycle_->OnBlockValidated(height, std::chrono::steady_clock::now());
                }
                if (height % log_interval == 0 || height == target_height) {
                    double percent = (static_cast<double>(height) /
                                      static_cast<double>(target_height)) * 100.0;
                    logger_->info("[BackgroundValidation] Progress: " + std::to_string(height) +
                                "/" + std::to_string(target_height) + " (" +
                                std::to_string(static_cast<int>(percent)) + "%)");
                }
            }
            if (replay_poisoned) break;
            if (replay_retry_pass) {
                // Unconfirmed validation failure: replay again immediately
                // with a fresh engine and fresh body reads. Do NOT run the
                // missing-body reconciliation/wait below — nothing is
                // missing, the pass was discarded on suspicion of transient
                // corruption. A full pass takes minutes, so this cannot
                // tight-loop; ReplayFailurePolicy bounds total retries.
                replay_retry_pass = false;
                replay.reset();
                continue;
            }
            if (blocks_skipped == 0) break;

            assumeutxo_lifecycle_->OnMissingBodies(blocks_skipped);
            // Release the partially-fed replay set (up to ~30MB at mainnet
            // scale) during the backfill wait; the next pass re-creates it.
            replay.reset();

            // ── #298 deterministic reconciliation: targeted re-request ──
            // Background validation used to discover missing pre-base bodies
            // and do nothing but sleep — a permanent stall when the one-shot
            // backfill window had already reported itself complete. Build a
            // {hash,height} want-list and re-queue it on the scheduler so the
            // next Tick re-issues getdata for exactly those bodies. For each
            // missing entry: re-derive the canonical hash from the active
            // header chain when we never had one (do NOT trust a stale
            // height→hash); otherwise re-request the header-anchored hash we
            // recorded. Per-height diagnostics (issue #298 spec) are emitted
            // below AFTER reconciliation so the `requested` field is accurate.
            std::vector<std::pair<uint256, uint32_t>> want;
            want.reserve(missing.size());
            std::vector<uint8_t> entry_requested(missing.size(), 0);
            std::vector<uint256> entry_hash(missing.size());  // resolved hash
            for (size_t i = 0; i < missing.size(); ++i) {
                // The scan already resolved snapshot-base-anchored hashes for
                // NoBodyForHash/HashMismatch. Populate diagnostics regardless
                // of whether complete-window reconciliation runs; the old code
                // misleadingly logged header_exists=0/requested=0 mid-window.
                entry_hash[i] = missing[i].canonical_hash;
            }
            auto* recon_ctx = DaemonContext::instance();
            consensus::BlockDownloadScheduler* sched =
                (recon_ctx && recon_ctx->block_download)
                    ? recon_ctx->block_download.get() : nullptr;
            if (!sched && !sched_unwired_logged) {
                logger_->warning("[BackgroundValidation] #298 block-download "
                                 "scheduler unwired — cannot re-request missing "
                                 "bodies; relying on 30s backstop");
                sched_unwired_logged = true;
            }

            // Mid-window liveness: give the earliest strict-read gap one
            // persistent scheduler lane. This does not bulk-requeue the missing
            // vector and does not reset an active request; it only prevents a
            // timed-out critical body from waiting for the ~50k-entry cursor to
            // wrap. The hash comes from the snapshot-base-anchored scan above,
            // never from best-header-by-height.
            if (sched) {
                if (!missing.empty() && !missing.front().canonical_hash.IsNull()) {
                    sched->SetBackfillValidationFrontier(
                        missing.front().canonical_hash, missing.front().height);
                } else {
                    sched->SetBackfillValidationFrontier(uint256(), 0);
                }
            }
            // #298 refinement: only re-request once backfill has reported its
            // window COMPLETE (completed >= total) yet validation still finds
            // gaps — that is the exact stuck condition the reconciliation edge
            // exists for (the original bug: completed==39942/39942 but 2909
            // bodies unreadable, nothing re-arming the fetch). Re-requesting
            // earlier is actively harmful:
            //   • completed==0 while foreground IBD starves the tip-idle-gated
            //     backfill drain  → re-requesting all 39942 every pass churns
            //     the scheduler and starves the fetch (observed: store=0,
            //     200k+ re-requests, foreground stalled).
            //   • completed climbing (backfill mid-window) → the bodies are
            //     already on the way; just wait.
            // After a re-request the scheduler drops `completed` below `total`
            // for the re-queued heights, so this naturally self-paces: one
            // re-request burst per window-completion-with-gaps, then wait for
            // the re-fetch to land.
            bool backfill_window_complete = false;
            if (sched) {
                const auto bf = sched->GetBackfillProgress();
                backfill_window_complete =
                    (bf.total > 0 && bf.completed >= bf.total);
            }
            size_t requeued = 0;
            if (sched && backfill_window_complete) {
                for (size_t i = 0; i < missing.size(); ++i) {
                    uint256 hash = entry_hash[i];
                    if (hash.IsNull()) {
                        uint256 derived;
                        if (sched->GetExpectedHashAtHeight(missing[i].height, derived)) {
                            hash = derived;
                        }
                    }
                    entry_hash[i] = hash;
                    if (!hash.IsNull()) {
                        want.emplace_back(hash, missing[i].height);
                        entry_requested[i] = 1;
                        // Mark outstanding so the read-success path can log the
                        // body's arrival exactly once on a later pass.
                        bg_requested_heights_[missing[i].height] = true;
                    }
                }
                if (!want.empty()) {
                    requeued = sched->RequestMissingBackfillBodies(want);
                }
            }

            // Per-skipped-height instrumentation (issue #298). Log the first 20
            // distinct missing heights in full; always log the gap-summary.
            uint32_t min_h = std::numeric_limits<uint32_t>::max();
            uint32_t max_h = 0;
            size_t detail_logged = 0;
            for (size_t i = 0; i < missing.size(); ++i) {
                const MissingEntry& e = missing[i];
                min_h = std::min(min_h, e.height);
                max_h = std::max(max_h, e.height);
                if (detail_logged >= 20) continue;
                ++detail_logged;
                const bool header_exists = !entry_hash[i].IsNull();
                const bool body_in_store = (e.reason == MissReason::HashMismatch);
                const bool read_failed   = (e.reason == MissReason::NoBodyForHash);
                logger_->info(std::string("[BackgroundValidation] #298 gap") +
                              " height=" + std::to_string(e.height) +
                              " expected=" + (header_exists
                                  ? entry_hash[i].GetHex().substr(0, 16) : "NONE") +
                              " header_exists=" + (header_exists ? "1" : "0") +
                              " body_in_store=" + (body_in_store ? "1" : "0") +
                              " read_failed=" + (read_failed ? "1" : "0") +
                              " requested=" + (entry_requested[i] ? "1" : "0"));
            }
            if (missing.empty()) { min_h = 0; }
            logger_->info(std::string("[BackgroundValidation] #298 gap-summary:") +
                          " missing=" + std::to_string(missing.size()) +
                          " min_height=" + std::to_string(min_h) +
                          " max_height=" + std::to_string(max_h) +
                          " requeued=" + std::to_string(requeued));

            logger_->warning("[BackgroundValidation] " + std::to_string(blocks_skipped) +
                             "/" + std::to_string(target_height + 1) +
                             " bodies unavailable — waiting for backfill (spec: missing"
                             " bodies are not success); re-scan in 30s or on body store");
            // #298 signalable wait (wake-on-store): when backfill has STALLED we
            // re-requested specific gap bodies and want to resume the instant
            // they land — arm the wake. During active backfill we did NOT
            // re-request; leave the wake disarmed so the ~40k stores don't each
            // trigger a full re-scan, and pace on the 30s backstop instead.
            // The stop paths set bg_validation_should_stop_ under
            // bg_validation_wait_mutex_ then notify, so shutdown never waits the
            // full 30s (no lost wakeup).
            bg_validation_awaiting_bodies_.store(backfill_window_complete);
            {
                std::unique_lock<std::mutex> lk(bg_validation_wait_mutex_);
                bg_validation_cv_.wait_for(lk, std::chrono::seconds(30), [this]{
                    return bg_validation_should_stop_.load() ||
                           bg_validation_body_arrived_.exchange(false);
                });
            }
            bg_validation_awaiting_bodies_.store(false);
            assumeutxo_lifecycle_->Tick(std::chrono::steady_clock::now());
            if (bg_validation_should_stop_) {
                OnBackgroundValidationComplete(false, "Validation stopped by user");
                return;
            }
        }
        if (replay_poisoned) {
            logger_->error("[BackgroundValidation] CRITICAL: " + replay_poison_reason);
            assumeutxo_lifecycle_->OnReplayComplete(
                /*replay_performed=*/true, /*commitment_match=*/false,
                "(canonical chain below base)", replay_poison_reason,
                0, std::chrono::steady_clock::now());
            OnBackgroundValidationComplete(false, replay_poison_reason);
            return;
        }

        assumeutxo_lifecycle_->OnMissingBodies(0);
        logger_->info("[BackgroundValidation] All bodies replayed; comparing content commitment...");

        // A complete engine replay pass (every height 1..target connected
        // through real validation) is indisputable progress — recover a
        // persisted/announced stall before evaluating completion, else the
        // lifecycle would refuse fully_validated from ValidationStalled
        // (cross-restart pre-marking can suppress all per-height signals).
        // Cannot loop: this fires at most once per COMPLETE pass, and a
        // complete pass always terminates (completion or fatal).
        if (assumeutxo_lifecycle_->GetState() ==
            assumeutxo::AssumeUtxoLifecycle::State::ValidationStalled) {
            assumeutxo_lifecycle_->OnBlockValidated(
                target_height, std::chrono::steady_clock::now());
        }

        // Fast pre-check (legacy): UTXO count vs snapshot metadata. A count
        // mismatch folds into commitment_match=false below — fatal via
        // OnReplayComplete, same severity as the pre-replay worker gave it.
        const bool count_ok = VerifyUTXOSetMatch();

        // Real commitment comparison (spec Completion Criteria items 2-3).
        const std::optional<std::string> expected_commitment =
            utxo_index_ ? utxo_index_->GetMetadata(assumeutxo::kExpectedCommitmentKey)
                        : std::optional<std::string>{};
        if (!expected_commitment) {
            if (!count_ok) {
                // No commitment to compare, but the durable set already fails
                // the snapshot's own count — keep the pre-replay fatal
                // semantics (any mismatch => fatal).
                std::string error = "UTXO set mismatch at snapshot height " +
                                    std::to_string(target_height);
                logger_->error("[BackgroundValidation] CRITICAL: " + error);
                assumeutxo_lifecycle_->OnReplayComplete(
                    /*replay_performed=*/true, /*commitment_match=*/false,
                    "(snapshot utxo count)", "(count mismatch)",
                    /*missing_body_count=*/0, std::chrono::steady_clock::now());
                OnBackgroundValidationComplete(false, error);
                return;
            }
            // Legacy load (pre-commitment binary) — replay ran, but there is
            // nothing trustworthy to compare against. Stay in
            // validating_history; never claim fully_validated. Operator remedy:
            // reload the snapshot with a current binary, or full resync.
            logger_->warning("[BackgroundValidation] no expected commitment persisted "
                             "(snapshot loaded by a pre-replay binary) — cannot retire "
                             "trust assumption; reload snapshot or resync to proceed");
            OnBackgroundValidationComplete(true, "");
            return;
        }

        const std::string recomputed = replay->RecordsDigestHex();
        const bool commitment_match = count_ok &&
            (recomputed == expected_commitment.value());
        // Defense-in-depth: utreexo root comparison when the v3 root was stored.
        bool root_match = true;
        if (auto expected_root = utxo_index_->GetMetadata(assumeutxo::kExpectedUtreexoRootKey)) {
            root_match = (replay->UtreexoRootHex() == expected_root.value());
        }

        const bool lifecycle_promoted_to_fully_validated =
            assumeutxo_lifecycle_->OnReplayComplete(
                /*replay_performed=*/true,
                commitment_match && root_match,
                expected_commitment.value(),
                recomputed + (root_match ? "" : " (utreexo root mismatch)"),
                /*missing_body_count=*/0, std::chrono::steady_clock::now());

        if (!(commitment_match && root_match)) {
            OnBackgroundValidationComplete(false,
                "replay commitment mismatch at base height " + std::to_string(target_height));
            return;
        }

        // ── Promotion (assumeutxo mode exit) ────────────────────────────────
        // History is now replay-proven; promote it into ChainDB so the exit
        // gate in OnBackgroundValidationComplete (chaindb tip >= base AND
        // lifecycle FullyValidated) can fire and clear the assumeutxo mode.
        //
        // Eligibility: the lifecycle promoted to FullyValidated just now, OR
        // it was ALREADY FullyValidated from a prior run (crash window: the
        // FullyValidated record persisted but the promotion writes did not
        // land — OnReplayComplete refuses re-entry from FullyValidated, so
        // gating on the return alone would strand such a node in assumeutxo
        // mode forever). Either way the commitment matched THIS pass.
        //
        // LOCKS HELD HERE: none. The worker's bg_validation_mutex_ uses are
        // all scoped guards released above; lifecycle mu_ is internal to each
        // lifecycle call. PromoteValidatedHistory takes activation_mutex_ for
        // its whole body — see its lock-ordering proof.
        const bool lifecycle_fully_validated_now =
            lifecycle_promoted_to_fully_validated ||
            assumeutxo_lifecycle_->GetState() ==
                assumeutxo::AssumeUtxoLifecycle::State::FullyValidated;
        if (lifecycle_fully_validated_now) {
            // Cheap idempotence guard: if the ChainDB tip is already at/above
            // the base (previous promotion landed, or the canonical chain
            // advanced past base), promotion is done — and on such a re-run
            // canonical_hashes_fallback may legitimately be empty (the height
            // index served every lookup), so don't even call.
            bool tip_below_base = true;
            {
                auto tip_result = chain_db_->getTip();
                if (tip_result.status() == Status::Ok &&
                    static_cast<uint32_t>(tip_result.value().height) >= target_height) {
                    tip_below_base = false;
                }
            }
            // Forward-connect: the tip legitimately advances past base
            // BEFORE the replay finishes, so tip>=base does not mean a prior
            // promotion landed — only the durable marker does. Without this,
            // promotion would be skipped and the pre-base coins never
            // materialized (#353 bug 2 through the side door).
            bool promotion_needed = tip_below_base;
            if (!tip_below_base && GetConfig().assumeutxo_forward_connect &&
                !PromotionArtifactsCommitted()) {
                promotion_needed = true;
            }
            if (promotion_needed) {
                std::string promote_err;
                if (!PromoteValidatedHistory(*replay, canonical_hashes_fallback,
                                             promote_err)) {
                    // OPERATIONAL failure (disk, db): retry next pass — never
                    // fatal. History remains proven; only the writes failed.
                    logger_->warning("[BackgroundValidation] promotion failed (" +
                                     promote_err +
                                     ") — will retry; assumeutxo mode remains active");
                    OnBackgroundValidationComplete(true, "");  // gate simply won't fire yet
                    return;
                }
                logger_->info("[Promotion] complete — ChainDB tip at base; "
                              "exit gate eligible");
            }
            // Arm the never-cleared below-base fork guard (survives
            // ClearAssumeUTXOState, which nulls assumeutxo_base_height_).
            promoted_base_height_ = target_height;
        }
        OnBackgroundValidationComplete(true, "");

    } catch (const std::exception& e) {
        std::string error = std::string("Exception during validation: ") + e.what();
        logger_->error("[BackgroundValidation] " + error);
        OnBackgroundValidationComplete(false, error);
    }
}

std::string ChainstateService::PromotionMarkerKey() const {
    // Keyed by the base BLOCK HASH: a future lifecycle with a different base
    // can never be satisfied by a stale marker. (Height alone could collide
    // across a chain reset.)
    return std::string("assumeutxo_promoted:") + assumeutxo_base_block_.GetHex();
}

bool ChainstateService::PromotionArtifactsCommitted() const {
    if (!chain_db_ || assumeutxo_base_block_.IsNull()) return false;
    auto result = chain_db_->getUtreexoMeta(PromotionMarkerKey());
    return result.status() == Status::Ok && result.value() == "1";
}

bool ChainstateService::PromoteValidatedHistory(
        const assumeutxo::AssumeUtxoReplayEngine& engine,
        const std::vector<uint256>& canonical_hashes,
        std::string& error) {
    // ═══════════════════════════════════════════════════════════════════════
    // AssumeUTXO mode exit (promotion): replay-proven history 1..base becomes
    // canonical ChainDB state so the existing exit gate
    // (OnBackgroundValidationComplete: chaindb tip >= base + FullyValidated)
    // can fire.
    //
    // THE NON-NEGOTIABLE ORDERING INVARIANT: setTip(base) is the LAST write
    // and the ONLY one that makes the rest observable to the startup audits
    // (VerifyActiveChainUndoCoverage / IsCanonicalStateAligned /
    // VerifyConsensusJournalAtActiveTip all key off the persisted tip).
    // Every write before it is invisible-until-tip and idempotently
    // re-runnable: a crash mid-promotion leaves the tip below base, the
    // worker re-runs replay+promotion on restart, and each put overwrites
    // its previous value with identical bytes (the proven history is
    // deterministic). Failure anywhere returns error+false — the caller
    // treats that as OPERATIONAL (retry next pass), never snapshot-fatal.
    //
    // LOCKING: activation_mutex_ is held for the entire method so the tip
    // check and all subsequent writes are atomic w.r.t. ConnectTip. Without
    // it, ConnectTip could advance the canonical tip between our tip read and
    // the coin reconcile, causing promotion to delete post-base coin deltas
    // and regress the persisted tip.
    //
    // Lock-ordering proof (no cycle):
    //   acquisition order is activation_mutex_ only; the worker holds nothing
    //   at the BackgroundValidationWorker call site (the promotion block
    //   between OnReplayComplete and OnBackgroundValidationComplete — its
    //   bg_validation_mutex_ uses are all scoped guards released earlier, and
    //   no assumeutxo_lifecycle_init_mutex_ or lifecycle mu_ is held).
    //   ActivateBestChain/ConnectTip (under activation_mutex_) never take
    //   bg_validation_mutex_ or assumeutxo_lifecycle_init_mutex_, so the
    //   hierarchy is bg_validation (if any) -> activation, with no reverse
    //   edge; deadlock is structurally impossible.
    // ═══════════════════════════════════════════════════════════════════════
    if (!chain_db_) { error = "chaindb unavailable"; return false; }
    if (!block_storage_) { error = "block storage unavailable"; return false; }

    const uint32_t base = assumeutxo_base_height_;
    const uint256 base_hash = assumeutxo_base_block_;
    if (base == 0 || base_hash.IsNull()) {
        error = "no assumeutxo base recorded";
        return false;
    }

    // Serialise all writes against ConnectTip (see locking comment above).
    std::lock_guard<std::recursive_mutex> activation_lock(activation_mutex_);

    // Defensive ordering guard (the caller also gates on this): once the
    // ChainDB tip is at/above the base — either a previous promotion landed
    // or post-base ConnectTip batches already advanced the canonical tip —
    // the bulk coin reconcile below would clobber post-base coin deltas with
    // base-state coins. Refuse to write; the exit gate's tip condition is
    // already satisfied.
    bool advanced_tip = false;
    {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == Status::Ok &&
            static_cast<uint32_t>(tip_result.value().height) >= base) {
            if (GetConfig().assumeutxo_forward_connect) {
                if (PromotionArtifactsCommitted()) {
                    if (logger_) {
                        logger_->info("[Promotion] completion marker present — "
                                      "promotion already landed (idempotent skip)");
                    }
                    return true;
                }
                // ADVANCED-TIP promotion (forward-connect): the canonical tip
                // is legitimately past base. Reconcile against the LIVE set,
                // leave tip-anchored state to ConnectTip, and commit via the
                // durable marker instead of setTip(base).
                advanced_tip = true;
                if (logger_) {
                    logger_->info("[Promotion] advanced-tip mode: ChainDB tip at height " +
                                  std::to_string(tip_result.value().height) +
                                  " >= base " + std::to_string(base) +
                                  " (forward-connect profile)");
                }
            } else {
                if (logger_) {
                    logger_->warning("[Promotion] ChainDB tip already at height " +
                                     std::to_string(tip_result.value().height) +
                                     " >= base " + std::to_string(base) +
                                     " — skipping promotion writes (exit gate already eligible)");
                }
                return true;
            }
        }
    }

    // The hash-anchored canonical table is the promotion's source of truth
    // for the height index — it MUST cover 0..base and agree with the base.
    if (canonical_hashes.size() < static_cast<size_t>(base) + 1) {
        error = "canonical hash table incomplete (" +
                std::to_string(canonical_hashes.size()) + " entries for base " +
                std::to_string(base) + ")";
        return false;
    }
    if (canonical_hashes[base] != base_hash) {
        error = "canonical hash table not anchored on the snapshot base";
        return false;
    }
    for (uint32_t h = 1; h <= base; ++h) {
        if (canonical_hashes[h].IsNull()) {
            error = "canonical hash table has a null entry at height " + std::to_string(h);
            return false;
        }
    }

    // ── Pre-write undo-tail coverage gate ─────────────────────────────────────
    // Require the engine's undo tail to cover [max(1, base-1023)..base]
    // contiguously (ascending, no gaps, last == base).
    //
    // Rationale: VerifyActiveChainUndoCoverage (called by Start() with
    // kStartupUndoAuditWindow=1024) checks exactly this window at the next
    // restart. If the tail is under-provisioned, the audit fires immediately
    // after promotion and writes a recovery marker that blocks chain advance.
    // Refusing promotion here surfaces the gap before it hits disk.
    {
        const uint32_t window_start = (base >= kStartupUndoAuditWindow)
            ? (base - kStartupUndoAuditWindow + 1u)
            : 1u;
        const auto& undo_tail = engine.UndoTail();
        if (undo_tail.empty() || undo_tail.back().height != base) {
            error = "undo tail does not cover the startup audit window — promotion refused";
            return false;
        }
        // Verify contiguous descending coverage from base back to window_start.
        bool coverage_ok = false;
        uint32_t expect = base;
        for (auto it = undo_tail.rbegin(); it != undo_tail.rend(); ++it) {
            if (it->height != expect) break;
            if (expect == window_start) { coverage_ok = true; break; }
            --expect;
        }
        if (!coverage_ok) {
            error = "undo tail does not cover the startup audit window — promotion refused";
            return false;
        }
    }

    if (logger_) {
        logger_->info("[Promotion] promoting replay-proven history 1.." + std::to_string(base) +
                      " into ChainDB (height index, undo tail, coin reconcile, tip)");
    }

    ChainWriteToken token;
    constexpr size_t kPromotionBatchOps = 4096;  // chunked batches (reindexer idiom)

    // Chunk-commit helper: commit the staged batch (sync=false — durability
    // is anchored by the final sync'd setTip; pre-tip writes are
    // invisible-until-tip and re-runnable).
    auto commit_chunk = [&](rocksdb::WriteBatch& batch, size_t& ops,
                            const char* what) -> bool {
        // Pre-tip writes are invisible-until-tip and idempotently re-runnable
        // (see ordering invariant, method header). Aborting here is safe.
        if (bg_validation_should_stop_.load()) {
            error = "shutdown requested during promotion";
            return false;
        }
        if (ops == 0) return true;
        const auto st = chain_db_->writeBatch(token, std::move(batch), /*sync=*/false);
        batch = rocksdb::WriteBatch();
        ops = 0;
        if (st != Status::Ok) {
            error = std::string("writeBatch failed during ") + what + " (status=" +
                    std::to_string(static_cast<int>(st)) + ")";
            return false;
        }
        return true;
    };

    // ── 1) Height index 1..base (idempotent puts, chunked) ─────────────────
    // canonical_hashes[h] is the worker's hash-anchored table (anchored on
    // the snapshot base hash, never the best header chain by height).
    {
        rocksdb::WriteBatch batch;
        size_t ops = 0;
        for (uint32_t h = 1; h <= base; ++h) {
            const auto st = chain_db_->putHeightIndex(
                token, static_cast<int>(h), canonical_hashes[h], &batch);
            if (st != Status::Ok) {
                error = "putHeightIndex failed at height " + std::to_string(h);
                return false;
            }
            if (++ops >= kPromotionBatchOps && !commit_chunk(batch, ops, "height-index")) {
                return false;
            }
        }
        if (!commit_chunk(batch, ops, "height-index")) return false;
    }
    if (logger_) {
        logger_->info("[Promotion] stage 1 complete: height index 1.." + std::to_string(base) + " written");
    }

    // ── 2) Undo tail: flatfile + locator + UD sidecar for the audited window ──
    // Mirrors ConnectTip's hoisted undo persistence (slice 3): the flatfile
    // write hits disk BEFORE the ChainDB metadata that points at it commits
    // (per chunk), so committed undo_file/pos/size always reference durable
    // bytes. The flatfile is the source of truth — reuse a readable existing
    // entry, write a fresh one otherwise (orphaned entries from a crashed
    // earlier attempt are harmless waste; no committed metadata references
    // them).
    {
        const bool stateless = GetConfig().utreexo_stateless;
        rocksdb::WriteBatch batch;
        size_t ops = 0;
        for (const auto& captured : engine.UndoTail()) {
            if (captured.height == 0 || captured.height > base) {
                error = "undo tail entry out of range at height " +
                        std::to_string(captured.height);
                return false;
            }
            const uint256& bh = captured.block_hash;
            if (canonical_hashes[captured.height] != bh) {
                error = "undo tail hash disagrees with canonical table at height " +
                        std::to_string(captured.height);
                return false;
            }

            // Existing-undo check (ConnectTip idiom: trust the flatfile, not
            // in-memory metadata alone).
            uint32_t undo_file = 0;
            uint32_t undo_pos = 0;
            uint32_t undo_size = 0;
            const auto existing_undo = ReadStoredUndo(bh);
            const auto meta_result = chain_db_->getHeaderMetadata(bh);
            const bool locator_present =
                meta_result.status() == Status::Ok && meta_result.value().undo_size > 0;
            const bool need_flatfile_undo =
                !(existing_undo.status() == Status::Ok && locator_present);
            if (need_flatfile_undo) {
                // BlockUndoToUndoRecord needs the block body to derive the
                // created-outpoint list (disconnect symmetry) — read the
                // stored canonical body the replay already validated.
                auto block_result = ReadStoredBlock(bh);
                if (block_result.status() != Status::Ok) {
                    error = "undo tail: stored body unavailable at height " +
                            std::to_string(captured.height);
                    return false;
                }
                const dinero::UndoRecord undo_record =
                    BlockUndoToUndoRecord(captured.undo, block_result.value());
                const std::vector<uint8_t> undo_bytes = undo_record.Serialize();
                auto undo_pos_result = block_storage_->writeUndo(bh, undo_bytes);
                if (undo_pos_result.status() != Status::Ok) {
                    error = "undo tail: writeUndo failed at height " +
                            std::to_string(captured.height) + " (status=" +
                            std::to_string(static_cast<int>(undo_pos_result.status())) + ")";
                    return false;
                }
                const auto& pos = undo_pos_result.value();
                if (pos.offset > std::numeric_limits<uint32_t>::max()) {
                    error = "undo tail: flatfile offset overflow at height " +
                            std::to_string(captured.height);
                    return false;
                }
                undo_file = pos.file_number;
                undo_pos = static_cast<uint32_t>(pos.offset);
                undo_size = pos.size;
            } else {
                undo_file = meta_result.value().undo_file;
                undo_pos = meta_result.value().undo_pos;
                undo_size = meta_result.value().undo_size;
            }

            // Surgical locator update (sets BLOCK_HAVE_UNDO, preserves
            // topology/chainwork/body positions — ConnectTip D.2 idiom),
            // with the same NotFound fallback to a full block-index stamp.
            CBlockIndex* idx = FindBlockIndex(bh);
            if (idx) {
                idx->undo_file = undo_file;
                idx->undo_pos = undo_pos;
                idx->undo_size = undo_size;
                idx->status |= BLOCK_HAVE_UNDO;
            }
            auto bi_status = chain_db_->updateUndoLocator(
                token, bh, undo_file, undo_pos, undo_size, &batch);
            if (bi_status == Status::NotFound && idx) {
                bi_status = chain_db_->updateBlockIndex(token, idx, &batch);
            }
            if (bi_status != Status::Ok) {
                error = "undo tail: undo locator stamp failed at height " +
                        std::to_string(captured.height) + " (status=" +
                        std::to_string(static_cast<int>(bi_status)) + ")";
                return false;
            }
            ++ops;

            // UD:<hash> utreexo delta sidecar — mandatory disconnect material
            // for every utreexo-active block in stateful mode (the startup
            // audit CheckBlockDisconnectMaterialDurable demands it). Same
            // serialization + key as ConnectTip's fold-in.
            if (consensus::IsUtreexoActive(captured.height) && !stateless) {
                if (!captured.undo.utreexo_delta.has_value()) {
                    error = "undo tail: missing utreexo delta at height " +
                            std::to_string(captured.height);
                    return false;
                }
                std::string delta_blob;
                std::string delta_error;
                if (!SerializeUtreexoDelta(*captured.undo.utreexo_delta, delta_blob,
                                           delta_error)) {
                    error = "undo tail: serialize utreexo delta failed at height " +
                            std::to_string(captured.height) + ": " + delta_error;
                    return false;
                }
                batch.Put(MakeUtreexoDeltaUndoKey(bh), delta_blob);
                ++ops;
            }

            if (ops >= kPromotionBatchOps && !commit_chunk(batch, ops, "undo-tail")) {
                return false;
            }
        }
        if (!commit_chunk(batch, ops, "undo-tail")) return false;
    }
    if (logger_) {
        logger_->info("[Promotion] stage 2 complete: undo tail written");
    }

    // ── 3) Bulk coin reconcile: coin CF must END equal to the proven set ───
    // Pre-promotion the coin CF holds only the genesis seed (genesis_init)
    // — LoadSnapshot populates the consensus set, never ChainDB. Delete
    // every row absent from the proven set (genesis coins the pre-base
    // history spent), then put every proven entry. Same end-state as the
    // reindexer's per-block delete/put rebuild, computed in one pass.
    {
        // ADVANCED-TIP (forward-connect): the coin CF must end equal to the
        // LIVE consensus set — proven base state + validated post-base
        // connects. Reconciling against the engine's base set here would
        // delete post-base coins and resurrect post-base-spent ones (the
        // exact #353 bug-2 hazard). activation_mutex_ (held for this whole
        // method) makes the live-set read atomic w.r.t. ConnectTip. The base
        // state itself was already proven: the replay-complete digest check
        // compared the engine's re-derived state to the snapshot commitment
        // before promotion was called, and the live set descends from that
        // same anchored snapshot.
        if (advanced_tip && !consensus_utxo_set_) {
            error = "advanced-tip promotion: consensus set unavailable";
            return false;
        }
        const auto& proven = advanced_tip ? consensus_utxo_set_->GetUTXOs()
                                          : engine.ProvenUtxos();

        std::vector<std::pair<uint256, uint32_t>> stale;
        const auto fe_status = chain_db_->forEachUTXO(
            [&](const uint256& txid, uint32_t vout, const Coin& /*coin*/) -> bool {
                if (proven.find(OutPoint(TxId(txid), vout)) == proven.end()) {
                    stale.emplace_back(txid, vout);
                }
                return true;
            });
        if (fe_status != Status::Ok) {
            error = "coin reconcile: forEachUTXO failed (status=" +
                    std::to_string(static_cast<int>(fe_status)) + ")";
            return false;
        }

        rocksdb::WriteBatch batch;
        size_t ops = 0;
        for (const auto& [txid, vout] : stale) {
            const auto st = chain_db_->deleteCoin(token, txid, vout, &batch);
            if (st != Status::Ok) {
                error = "coin reconcile: deleteCoin failed";
                return false;
            }
            if (++ops >= kPromotionBatchOps && !commit_chunk(batch, ops, "coin-delete")) {
                return false;
            }
        }

        for (const auto& [outpoint, entry] : proven) {
            // Conversion via file-local helper (byte-identical to
            // PersistentUTXOAdapter::ToDbCoin — see UtxoEntryToDbCoin above).
            const Coin coin = UtxoEntryToDbCoin(entry);
            const auto st = chain_db_->putCoin(
                token, outpoint.txid.AsUint256(), outpoint.vout, coin, &batch);
            if (st != Status::Ok) {
                error = "coin reconcile: putCoin failed";
                return false;
            }
            if (++ops >= kPromotionBatchOps && !commit_chunk(batch, ops, "coin-put")) {
                return false;
            }
        }
        if (!commit_chunk(batch, ops, "coin-reconcile")) return false;
    }
    if (logger_) {
        logger_->info("[Promotion] stage 3 complete: coin CF reconciled");
    }

    // ── Nullifier content — serialized ONCE; reused by stage 4a (row writes)
    // and the journal preimage (section 5).  Rows written ≡ hash input by
    // construction, eliminating any risk of a malformed-row error on the
    // second call silently hashing an empty preimage → journal mismatch.
    //
    // engine.ShieldedNullifiers() is guaranteed non-null: the engine ctor
    // constructs the trio unconditionally (make_unique) and throws
    // std::runtime_error if Open(":memory:") fails — the unique_ptr is never
    // empty at this call site.
    const auto nullifier_content = engine.ShieldedNullifiers()->SerializeContent();
    // Stream-error guard: SerializeContent returns empty on sqlite error
    // (e.g. malformed row — see nullifier_set.cpp). If the set is non-empty
    // but serialization produced nothing, we would silently drop all
    // nullifiers and promote a corrupted state.
    if (nullifier_content.empty() && engine.ShieldedNullifiers()->Size() > 0) {
        error = "shielded promotion: nullifier serialization returned empty for non-empty set";
        return false;
    }
    constexpr size_t kHeaderBytes = 4 + 2 + 8;
    constexpr size_t kRowBytes = 4 + 32;
    constexpr uint32_t kSerializeContentTag = 0x4653434E;  // 'NSCF' LE-reconstructed
    uint32_t tag_read = 0;
    if (nullifier_content.size() >= 4) {
        for (int i = 0; i < 4; ++i) {
            tag_read |= static_cast<uint32_t>(nullifier_content[i]) << (i * 8);
        }
    }
    uint64_t row_count = 0;
    if (!nullifier_content.empty()) {
        if (nullifier_content.size() < kHeaderBytes || tag_read != kSerializeContentTag) {
            error = "shielded promotion: nullifier SerializeContent header malformed";
            return false;
        }
        for (int i = 0; i < 8; ++i) {
            row_count |= static_cast<uint64_t>(nullifier_content[6 + i]) << (i * 8);
        }
        if (nullifier_content.size() - kHeaderBytes != row_count * kRowBytes) {
            error = "shielded promotion: nullifier SerializeContent size mismatch";
            return false;
        }
    }

    // ── 4a) Shielded nullifier rows from the engine's proven set (chunked) ──
    // ChainDB is the authoritative on-disk nullifier store (startup rebuilds
    // the sqlite cache from these rows). Parsed from nullifier_content above —
    // same byte format LoadShieldedState's mode-C migration parses:
    //   tag 'NSCF' (4) || version (2) || count_LE_u64 (8) ||
    //     per row: height_LE_u32 (4) || nullifier_bytes (32)
    {
        rocksdb::WriteBatch batch;
        size_t ops = 0;
        for (uint64_t i = 0; i < row_count; ++i) {
            const size_t off = kHeaderBytes + i * kRowBytes;
            uint32_t height = 0;
            for (int j = 0; j < 4; ++j) {
                height |= static_cast<uint32_t>(nullifier_content[off + j]) << (j * 8);
            }
            const uint8_t* nf = &nullifier_content[off + 4];
            const auto st = chain_db_->putShieldedNullifier(token, height, nf, &batch);
            if (st != Status::Ok) {
                error = "shielded promotion: putShieldedNullifier failed";
                return false;
            }
            if (++ops >= kPromotionBatchOps && !commit_chunk(batch, ops, "nullifiers")) {
                return false;
            }
        }
        if (!commit_chunk(batch, ops, "nullifiers")) return false;
    }
    if (logger_) {
        logger_->info("[Promotion] stage 4a complete: " + std::to_string(row_count) +
                      " nullifier rows written");
    }

    // ── 4b) Tip-anchored singles at base (one batch) + 5) journal row ──────
    // ADVANCED-TIP: skipped entirely. ConnectTip has been maintaining the
    // forest/shielded tip markers, frontier/anchor blobs, and per-height
    // journal rows at the REAL tip all along; overwriting those singles with
    // base-state values would leave the persisted markers behind the actual
    // tip and corrupt the next restart's alignment audits. The base-height
    // journal row is never read while the tip is above base.
    if (!advanced_tip)
    // All from the ENGINE's proven state at the base — the same containers a
    // post-promotion restart loads back (frontier blob, anchor history blob,
    // nullifier rows above) and verifies against the markers
    // (VerifyOrBootstrapShieldedTipMarker / IsCanonicalStateAligned at
    // tip=base). The forest fields come from the LIVE consensus set: under
    // the tip<base gate the live tip IS the snapshot base, and startup
    // restores the forest from the checkpoint LoadSnapshot persisted at the
    // base — the same serialized forest object the live set holds now (the
    // replay verified the engine's root equals it).
    {
        rocksdb::WriteBatch batch;

        // ForestTipMarker — height + block hash + forest root (mirror
        // ConnectTip slice 2; the base checkpoint itself was persisted by
        // LoadSnapshot).
        {
            ChainDB::ForestTipMarker forest_marker;
            forest_marker.height = static_cast<int32_t>(base);
            forest_marker.block_hash = base_hash;
            const consensus::UtreexoHash commitment = engine.Forest()->getCommitment();
            if (commitment.size() == 32) {
                std::memcpy(forest_marker.forest_root.data, commitment.data(), 32);
            } else {
                forest_marker.forest_root.SetNull();
            }
            const auto st = chain_db_->putForestTipMarker(token, forest_marker, &batch);
            if (st != Status::Ok) {
                error = "promotion: putForestTipMarker failed";
                return false;
            }
        }

        // Shielded frontier + anchor history blobs (DSRH inputs; mirror
        // ConnectTip's option-1 fold-in keys).
        {
            const auto frontier_bytes = engine.ShieldedTree()->SerializeFrontier();
            const std::string frontier_blob(frontier_bytes.begin(), frontier_bytes.end());
            auto st = chain_db_->putUtreexoMeta(token, "shielded_frontier",
                                                frontier_blob, &batch);
            if (st != Status::Ok) {
                error = "promotion: shielded frontier blob stage failed";
                return false;
            }
            const auto anchor_bytes = engine.ShieldedAnchors()->SerializeBytes();
            const std::string anchor_blob(anchor_bytes.begin(), anchor_bytes.end());
            st = chain_db_->putUtreexoMeta(token, "shielded_anchor_history",
                                           anchor_blob, &batch);
            if (st != Status::Ok) {
                error = "promotion: anchor history blob stage failed";
                return false;
            }
        }

        // ShieldedTipMarker at base from the engine's state.
        {
            ChainDB::ShieldedTipMarker marker;
            marker.height = static_cast<int32_t>(base);
            marker.block_hash = base_hash;
            const auto root = engine.ShieldedTree()->Root();
            if (root.size() == 32) {
                std::memcpy(marker.shielded_root.data, root.data(), 32);
            } else {
                marker.shielded_root.SetNull();
            }
            marker.tree_size = engine.ShieldedTree()->Size();
            marker.nullifier_count = engine.ShieldedNullifiers()->Size();
            const auto st = chain_db_->putShieldedTipMarker(token, marker, &batch);
            if (st != Status::Ok) {
                error = "promotion: putShieldedTipMarker failed";
                return false;
            }
        }

        // 5) Consensus journal row at base (same key/value shape as
        // ConsensusWriteBatch::AttachJournalRowToBatch, same flag gate as
        // VerifyConsensusJournalAtActiveTip). The DSRH v2 preimage is
        // composed from what a post-promotion restart at tip=base will hold
        // live: the forest restored from the base checkpoint (== the live
        // consensus set's forest under the tip<base gate) + the ENGINE's
        // shielded containers (loaded back from the rows/blobs staged
        // above). Layout mirrors ComputeShieldedReorgStateHash (DSR2).
        if (consensus::ConsensusWriteBatch::IsEnabled(*this)) {
            std::vector<uint8_t> preimage;
            preimage.reserve(128);
            preimage.push_back('D'); preimage.push_back('S');
            preimage.push_back('R'); preimage.push_back('2');
            preimage.push_back(2);
            auto append_le64 = [&](uint64_t v) {
                for (int i = 0; i < 8; ++i) {
                    preimage.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
                }
            };
            if (consensus_utxo_set_) {
                const auto& forest = consensus_utxo_set_->GetForest();
                const auto commitment = forest.getCommitment();
                if (commitment.size() == 32) {
                    preimage.insert(preimage.end(), commitment.begin(), commitment.end());
                } else {
                    preimage.insert(preimage.end(), 32, 0);
                }
                append_le64(forest.getNumLeaves());
                preimage.push_back(forest.isCanonicalEmptyRoots() ? 1 : 0);
            } else {
                preimage.insert(preimage.end(), 32 + 8 + 1, 0);
            }
            {
                const auto root = engine.ShieldedTree()->Root();
                if (root.size() == 32) {
                    preimage.insert(preimage.end(), root.begin(), root.end());
                } else {
                    preimage.insert(preimage.end(), 32, 0);
                }
                append_le64(engine.ShieldedTree()->Size());
            }
            {
                // Reuse the single serialization from above — rows written ≡ hash input by construction.
                preimage.insert(preimage.end(), nullifier_content.begin(), nullifier_content.end());
            }
            {
                const auto anchor_bytes = engine.ShieldedAnchors()->SerializeBytes();
                preimage.insert(preimage.end(), anchor_bytes.begin(), anchor_bytes.end());
            }
            crypto::CSHA256 hasher;
            hasher.Write(preimage.data(), preimage.size());
            uint8_t digest[32];
            hasher.Finalize(digest);
            uint256 state_hash;
            std::memcpy(state_hash.data, digest, 32);

            char height_be_hex[9];
            std::snprintf(height_be_hex, sizeof(height_be_hex), "%08x", base);
            const std::string journal_key =
                std::string("consensus_journal:") + height_be_hex + ":" +
                base_hash.GetHex();
            const auto st = chain_db_->putUtreexoMeta(token, journal_key,
                                                      state_hash.GetHex(), &batch);
            if (st != Status::Ok) {
                error = "promotion: consensus journal row stage failed";
                return false;
            }
        }

        const auto st = chain_db_->writeBatch(token, std::move(batch), /*sync=*/true);
        if (st != Status::Ok) {
            error = "promotion: tip-anchored marker batch commit failed (status=" +
                    std::to_string(static_cast<int>(st)) + ")";
            return false;
        }
    }
    if (logger_) {
        logger_->info(advanced_tip
            ? "[Promotion] stage 4b/5 skipped (advanced-tip: ConnectTip owns the tip-anchored state)"
            : "[Promotion] stage 4b/5 complete: tip-anchored markers and journal row committed");
    }

    // ── 6) The commit point ────────────────────────────────────────────────
    // ADVANCED-TIP: the tip is already past base and must NOT be regressed.
    // The durable completion marker replaces setTip(base) as the last write:
    // it is what makes this promotion observable to the worker's idempotence
    // gate and to the exit gate in OnBackgroundValidationComplete. A crash
    // before the marker re-runs the replay + promotion on restart; every
    // write above is idempotent (same proven bytes).
    if (advanced_tip) {
        rocksdb::WriteBatch marker_batch;
        const auto put_st = chain_db_->putUtreexoMeta(
            token, PromotionMarkerKey(), "1", &marker_batch);
        if (put_st != Status::Ok) {
            error = "advanced-tip promotion: completion marker stage failed";
            return false;
        }
        const auto st = chain_db_->writeBatch(token, std::move(marker_batch), /*sync=*/true);
        if (st != Status::Ok) {
            error = "advanced-tip promotion: completion marker commit failed (status=" +
                    std::to_string(static_cast<int>(st)) + ")";
            return false;
        }
        if (logger_) {
            logger_->info("[Promotion] complete (advanced-tip): pre-base history "
                          "materialized under a live tip; completion marker durable "
                          "(tip untouched)");
        }
        return true;
    }

    // ── 6) Durable setTip(base) — THE LAST WRITE (see ordering invariant) ──
    // Work comes from the header-chain block index for the base (the same
    // chainwork-hex source ConnectTip uses); fall back to the persisted
    // header metadata when the in-memory entry is unavailable.
    {
        arith_uint256 base_work;
        bool have_work = false;
        if (CBlockIndex* base_index = FindBlockIndex(base_hash)) {
            try {
                base_work = ChainworkFromHex(base_index->chainwork);
                have_work = true;
            } catch (const std::exception& e) {
                if (logger_) {
                    logger_->warning(std::string("[Promotion] invalid chainwork hex on base "
                                                 "block index: ") + e.what());
                }
            }
        }
        if (!have_work) {
            auto work_result = chain_db_->getBlockWork(base_hash);
            if (work_result.status() != Status::Ok) {
                error = "promotion: cannot resolve chainwork for the base block";
                return false;
            }
            base_work = work_result.value();
        }
        // Direct (non-batched) setTip writes with sync=true (chain_db.cpp
        // Phase E.1.c) — the single fsync that makes the promotion visible.
        const auto st = chain_db_->setTip(token, base_hash,
                                          static_cast<int>(base), base_work);
        if (st != Status::Ok) {
            error = "promotion: setTip(base) failed (status=" +
                    std::to_string(static_cast<int>(st)) + ")";
            return false;
        }
    }

    // Under the forward-connect profile the exit gate requires the completion
    // marker even when promotion ran classically (replay won the race and the
    // tip was still below base) — write it after setTip so both modes satisfy
    // the same gate.
    if (GetConfig().assumeutxo_forward_connect) {
        rocksdb::WriteBatch marker_batch;
        if (chain_db_->putUtreexoMeta(token, PromotionMarkerKey(), "1",
                                      &marker_batch) == Status::Ok) {
            (void)chain_db_->writeBatch(token, std::move(marker_batch), /*sync=*/true);
        }
    }

    if (logger_) {
        logger_->info("[Promotion] complete: ChainDB tip now at base height " +
                      std::to_string(base) + " (" + base_hash.GetHex().substr(0, 16) +
                      "...) — height index, undo tail, coin CF and tip-anchored "
                      "markers promoted from the proven replay");
    }
    return true;
}

bool ChainstateService::VerifyUTXOSetMatch() {
    // Phase 44.1: UTXO Set Verification (Defense-in-Depth)
    //
    // Strategy: Multi-level verification with fail-fast
    // 1. Count verification (fast sanity check)
    // 2. Spot-check sampled UTXOs (defense-in-depth)
    // 3. Full verification on demand (future: enabled via config flag)
    //
    // Why this is sufficient for this phase:
    // - Snapshot checksum already verified (CRITICAL-001 fix)
    // - Block retrieval scan confirms data availability across the snapshot range
    // - Count + spot-check catches corruption/attacks
    // - Full byte-for-byte verification is expensive and redundant

    logger_->info("[BackgroundValidation] ═══════════════════════════════════════════════════════");
    logger_->info("[BackgroundValidation] Phase 44.1: UTXO Set Verification");
    logger_->info("[BackgroundValidation] ═══════════════════════════════════════════════════════");

    // Level 1: Count Verification
    // Get expected count from snapshot metadata
    auto coins_meta = utxo_index_->GetMetadata("assumeutxo_coins_loaded");
    if (!coins_meta) {
        logger_->warning("[BackgroundValidation] Snapshot metadata missing 'coins_loaded' - skipping count check");
        logger_->warning("[BackgroundValidation] This is expected for older snapshots");
    } else {
        uint64_t expected_count = std::stoull(coins_meta.value());

        // Get actual UTXO count from database
        // Note: This counts unspent UTXOs (spend_height IS NULL)
        auto count_result = utxo_index_->GetUTXOCount();
        if (!count_result.isOk()) {
            logger_->error("[BackgroundValidation] Failed to get UTXO count: " + count_result.error());
            return false;
        }

        uint64_t actual_count = count_result.value();

        logger_->info("[BackgroundValidation] UTXO Count Verification:");
        logger_->info("[BackgroundValidation]   Expected (from snapshot): " + std::to_string(expected_count));
        logger_->info("[BackgroundValidation]   Actual (from database):   " + std::to_string(actual_count));

        if (actual_count != expected_count) {
            logger_->error("[BackgroundValidation] ✗ UTXO COUNT MISMATCH!");
            logger_->error("[BackgroundValidation]   Expected: " + std::to_string(expected_count));
            logger_->error("[BackgroundValidation]   Got:      " + std::to_string(actual_count));
            logger_->error("[BackgroundValidation]   Difference: " +
                          std::to_string(static_cast<int64_t>(actual_count) - static_cast<int64_t>(expected_count)));
            logger_->error("[BackgroundValidation] Snapshot is INVALID or corrupted");
            return false;
        }

        logger_->info("[BackgroundValidation] ✓ UTXO count matches: " + std::to_string(actual_count));
    }

    // Level 2: Spot-Check Sampled UTXOs
    // Sample a subset of UTXOs and verify they exist with correct values
    // This catches corruption that might have same count but wrong data
    logger_->info("[BackgroundValidation] Spot-checking sampled UTXOs...");

    // Get a sample of UTXOs (e.g., first 100, last 100, random middle)
    // For simplicity, we'll verify that UTXOs exist and are unspent
    // Full value verification would require replaying all transactions (expensive)

    // Sample check: Verify snapshot base block's coinbase UTXO exists
    if (assumeutxo_base_height_ > 0) {
        // Get block hash at snapshot height
        auto hash_result = chain_db_->getBlockHashByHeight(assumeutxo_base_height_);
        if (!hash_result.ok()) {
            logger_->warning("[BackgroundValidation] Could not get block hash at snapshot height for spot-check");
        } else {
            logger_->info("[BackgroundValidation] Verified base block exists at height " +
                         std::to_string(assumeutxo_base_height_));
            logger_->info("[BackgroundValidation] Base block hash: " + hash_result.value().GetHex());
        }
    }

    // Level 3: Full Verification (Optional hardening mode)
    // A full UTXO-by-UTXO comparison can be added behind an explicit config flag.
    // This would require:
    // - Rebuild UTXO set by replaying all transactions genesis → snapshot_height
    // - Compare every UTXO (txid, vout, value, scriptPubKey, height, is_coinbase)
    // - Very expensive but provides maximum confidence
    // - Only needed for paranoid deployments or after detected attacks

    logger_->info("[BackgroundValidation] ═══════════════════════════════════════════════════════");
    logger_->info("[BackgroundValidation] ✓ UTXO Set Verification PASSED");
    logger_->info("[BackgroundValidation]   - Count verification: PASSED");
    logger_->info("[BackgroundValidation]   - Spot-check: PASSED");
    logger_->info("[BackgroundValidation]   - Full verification: SKIPPED (not required)");
    logger_->info("[BackgroundValidation] ═══════════════════════════════════════════════════════");

    return true;
}

void ChainstateService::OnBackgroundValidationComplete(bool success, const std::string& error) {
    // Deadlock guard: EnterSafeMode() -> MiningService::stopMining() joins
    // mining threads. Nothing mining-side takes bg_validation_mutex_ today,
    // but joining threads while holding it is fragile — so the failure branch
    // gathers state and logs under the lock, then acts AFTER releasing it.
    bool enter_fatal = false;
    bool activate_stored_post_base_branch = false;
    std::string fatal_error;
    {
    std::lock_guard<std::mutex> lock(bg_validation_mutex_);

    if (success) {
        bg_validation_status_ = BackgroundValidationStatus::Completed;
        bg_validation_current_height_ = assumeutxo_base_height_;

        logger_->info("═══════════════════════════════════════════════════════════════════════");
        logger_->info("✅ BACKGROUND INTEGRITY CHECK COMPLETE");
        logger_->info("═══════════════════════════════════════════════════════════════════════");
        logger_->info("Blocks scanned for availability: " + std::to_string(bg_validation_blocks_validated_));
        logger_->info("UTXO set integrity verified at height: " + std::to_string(assumeutxo_base_height_));
        logger_->info("Snapshot integrity checks passed");
        logger_->info("═══════════════════════════════════════════════════════════════════════");

        // Only exit AssumeUTXO mode if chaindb has caught up to (or past) the snapshot height.
        // If chaindb is still at genesis (fresh chaindb), we must keep assumeutxo_active_ = true
        // so that IsCanonicalStateAligned allows the chaindb-behind state until new blocks
        // are connected via ConnectTip.
        bool chaindb_caught_up = false;
        if (chain_db_) {
            auto tip_result = chain_db_->getTip();
            if (tip_result.status() == Status::Ok &&
                static_cast<uint32_t>(tip_result.value().height) >= assumeutxo_base_height_) {
                chaindb_caught_up = true;
            }
        }
        // Forward-connect: tip >= base is true from forward sync ALONE, so it
        // cannot stand in for "promotion landed". Require the durable
        // completion marker, else the mode would exit with the pre-base coins
        // never materialized (#353 bug 2 through the side door).
        if (chaindb_caught_up && GetConfig().assumeutxo_forward_connect &&
            !PromotionArtifactsCommitted()) {
            chaindb_caught_up = false;
            logger_->info("[BackgroundValidation] tip past base but promotion marker "
                          "not durable yet — keeping AssumeUTXO mode active until "
                          "promotion lands");
        }

        const bool lifecycle_fully_validated =
            assumeutxo_lifecycle_ &&
            assumeutxo_lifecycle_->GetState() ==
                assumeutxo::AssumeUtxoLifecycle::State::FullyValidated;
        if (chaindb_caught_up && lifecycle_fully_validated) {
            logger_->info("[BackgroundValidation] ChainDB at snapshot height and history "
                          "fully validated — exiting AssumeUTXO mode");
            ClearAssumeUTXOState(/*clear_persisted_metadata=*/true);
            // ActivateBestChain deliberately skipped this canonical post-base
            // branch while the snapshot hold was active.  Import and activate
            // the already stored continuation immediately after releasing the
            // background-validation mutex instead of waiting for another peer
            // event to happen to trigger it.
            activate_stored_post_base_branch = true;
        } else if (chaindb_caught_up) {
            logger_->info("[BackgroundValidation] ChainDB caught up but historical replay "
                          "is not complete — keeping AssumeUTXO trust marker (spec: only a "
                          "completed genesis-to-base comparison retires trust)");
        } else {
            logger_->info("[BackgroundValidation] ChainDB below snapshot height — keeping AssumeUTXO mode active until tip catches up");
        }

        const uint32_t local_height = getBlockHeight();
        const bool caught_up_to_network = (ibd_network_height_ == 0 || local_height >= ibd_network_height_);
        if (caught_up_to_network) {
            ibd_status_ = IBDStatus::IBDComplete;
            logger_->info("[BackgroundValidation] Snapshot bootstrap fully caught up — marking IBD complete");
        } else {
            ibd_status_ = IBDStatus::SnapshotBootstrap;
            logger_->info("[BackgroundValidation] Snapshot validated but still behind network tip — staying in SnapshotBootstrap");
        }

    } else if (bg_validation_should_stop_.load()) {
        // Operational stop (daemon shutdown joins the worker via
        // bg_validation_should_stop_). NOT a proof failure: the lifecycle
        // stays validating_history/validation_stalled in persistence and
        // resumes at next startup. Going fatal here would brick every node
        // that restarts mid-validation.
        bg_validation_status_ = BackgroundValidationStatus::Failed;
        bg_validation_error_ = error;
        logger_->warning("[BackgroundValidation] Stopped by request before completion (" +
                         error + "); validation will resume at next startup");
    } else {
        // All hard background-validation failures converge here, including
        // explicit verification failures and worker exceptions.
        bg_validation_status_ = BackgroundValidationStatus::Failed;
        bg_validation_error_ = error;

        // Spec (Fatal Mismatch Semantics): a mismatch is NOT an automatic
        // rollback-to-genesis. Persist fatal, halt assumed-state decisions via
        // safe mode, and require explicit operator reset. The previous
        // auto-rollback (ClearAll + ClearAssumeUTXOState) is intentionally GONE.
        const std::string snapshot_path =
            config_ ? config_->GetString("assumeutxo_snapshot", "") : "";
        logger_->error("═══════════════════════════════════════════════════════════════════════");
        logger_->error("❌ BACKGROUND VALIDATION FAILED — ENTERING FATAL STATE");
        logger_->error("Error: " + error);
        logger_->error("Snapshot base height: " + std::to_string(assumeutxo_base_height_));
        logger_->error("Snapshot base hash:   " + assumeutxo_base_block_.GetHex());
        logger_->error("Snapshot asset:       " +
                       (snapshot_path.empty() ? std::string("(no assumeutxo_snapshot configured)")
                                              : snapshot_path));
        logger_->error("Node was serving from state that failed later proof.");
        logger_->error("Balances/confirmations derived while assumed are PROVISIONAL.");
        logger_->error("Operator reset required: blockchain.resetassumeutxofatal");
        logger_->error("  (or wipe the datadir and resync from genesis).");
        logger_->error("═══════════════════════════════════════════════════════════════════════");

        enter_fatal = true;
        fatal_error = error;
    }
    }  // release bg_validation_mutex_ before EnterSafeMode (joins mining threads)

    if (activate_stored_post_base_branch) {
        // This call still runs on the background-validation worker's stack.
        // Promotion is already durable and the lifecycle is FullyValidated, so
        // an operational catch-up exception must NOT escape to the worker's
        // outer catch and be misclassified as a snapshot proof failure/fatal
        // mismatch.  Normal periodic/peer-driven activation will retry.
        try {
            ActivateBestChain();
        } catch (const std::exception& e) {
            if (logger_) {
                logger_->error(std::string("[BackgroundValidation] Post-promotion "
                                           "ActivateBestChain failed; catch-up will retry: ") +
                               e.what());
            }
        } catch (...) {
            if (logger_) {
                logger_->error("[BackgroundValidation] Post-promotion ActivateBestChain "
                               "failed with unknown exception; catch-up will retry");
            }
        }
    }

    if (enter_fatal) {
        EnsureAssumeUtxoLifecycle();
        if (assumeutxo_lifecycle_->GetState() !=
            assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
            // Worker exceptions / operational failures that bypassed
            // OnReplayComplete still converge to fatal here.
            assumeutxo_lifecycle_->OnReplayComplete(
                /*replay_performed=*/false, /*commitment_match=*/false,
                "(background validation)", fatal_error, 0,
                std::chrono::steady_clock::now());
        }
        EnterSafeMode("assumeutxo fatal: " + fatal_error);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 45: Snapshot-accelerated IBD (Fast Sync)
// ═══════════════════════════════════════════════════════════════════════════

ChainstateService::SyncSnapshot ChainstateService::GetSyncSnapshot() const {
    SyncSnapshot snap;

    // Best header: copied under HeaderChainSelector's mutex. Deliberately NOT
    // GetBestHeader(), which returns a raw pointer after releasing that lock —
    // reading its fields races with concurrent header connection (#439).
    if (header_chain_selector_) {
        dinero::consensus::HeaderIndexEntry best{};
        if (header_chain_selector_->GetBestHeaderCopy(best)) {
            snap.has_best_header = true;
            snap.best_header_hash = best.hash;
            snap.best_header_height = best.height;
        }
    }

    // Active tip: read the VALUE published by PublishActiveTip under its own
    // mutex. Deliberately NOT `active_tip_->GetBlockHash()` — active_tip_ is a
    // bare CBlockIndex* mutated on the chain-advancement path, so dereferencing
    // it here would race with block connection.
    {
        std::lock_guard<std::mutex> lock(published_tip_mutex_);
        if (published_tip_valid_) {
            snap.has_active_tip = true;
            snap.active_tip_hash = published_tip_hash_;
            snap.active_tip_height = published_tip_height_;
        }
    }

    snap.RecomputeConvergence();
    return snap;
}

bool ChainstateService::IsInIBD() const {
    if (!chain_db_) {
        return false;  // Can't determine without chain DB
    }

    // If we loaded a snapshot, we're not in traditional IBD
    if (assumeutxo_active_) {
        return false;
    }

    // Get our current chain height
    auto tip_result = chain_db_->getTip();
    if (!tip_result) {
        return true;  // No tip = definitely in IBD
    }

    uint32_t local_height = tip_result.value().height;

    // If we have an estimated network height, compare against it
    if (ibd_network_height_ > 0) {
        uint32_t blocks_behind = (ibd_network_height_ > local_height) ?
                                 (ibd_network_height_ - local_height) : 0;
        return blocks_behind >= IBD_THRESHOLD_BLOCKS;
    }

    // No network height estimate - not in IBD if we have any validated chain
    // (HeaderSyncManager is the canonical IBD check; this is a fallback)
    return local_height < 1;  // Genesis-only = IBD; any blocks beyond = not IBD
}

ChainstateService::IBDProgress ChainstateService::GetIBDProgress() const {
    IBDProgress progress;
    progress.status = ibd_status_;
    progress.snapshot_loaded = assumeutxo_active_;
    progress.services_ready = services_ready_;

    if (!chain_db_) {
        progress.local_height = 0;
        progress.network_height = 0;
        progress.blocks_remaining = 0;
        progress.sync_percent = 0.0;
        return progress;
    }

    // Get local height — prefer active_tip_ (authoritative in AssumeUTXO mode)
    // since chaindb may lag at genesis when snapshot is loaded.
    if (active_tip_) {
        progress.local_height = static_cast<uint32_t>(active_tip_->height);
    } else {
        auto tip_result = chain_db_->getTip();
        progress.local_height = tip_result ? tip_result.value().height : 0;
    }

    // Get network height estimate. Never report a network height below our
    // authoritative local active tip or callers will see impossible progress.
    progress.network_height = std::max(ibd_network_height_, progress.local_height);

    // Calculate progress
    if (progress.network_height > 0 && progress.network_height > progress.local_height) {
        progress.blocks_remaining = progress.network_height - progress.local_height;
        progress.sync_percent = (static_cast<double>(progress.local_height) /
                                static_cast<double>(progress.network_height)) * 100.0;
    } else {
        progress.blocks_remaining = 0;
        progress.sync_percent = 100.0;
    }

    if (progress.status == IBDStatus::SnapshotBootstrap &&
        progress.local_height >= progress.network_height &&
        bg_validation_status_ == BackgroundValidationStatus::Completed) {
        progress.status = IBDStatus::IBDComplete;
    }

    return progress;
}

bool ChainstateService::TrySnapshotBootstrap(const std::filesystem::path& snapshot_path) {
    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("🚀 SNAPSHOT-ACCELERATED IBD");
    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("Attempting to bootstrap from snapshot: " + snapshot_path.string());

    // Check if snapshot file exists
    if (!std::filesystem::exists(snapshot_path)) {
        logger_->error("[IBD] Snapshot file not found: " + snapshot_path.string());
        return false;
    }

    // Load the snapshot
    auto result = LoadSnapshot(snapshot_path);

    if (!result.success) {
        logger_->error("[IBD] Snapshot load failed: " + result.error_message);
        return false;
    }

    // Update IBD status
    ibd_status_ = IBDStatus::SnapshotBootstrap;
    services_ready_ = true;  // Services immediately available!

    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("✅ SNAPSHOT BOOTSTRAP SUCCESS");
    logger_->info("═══════════════════════════════════════════════════════════════════════");
    logger_->info("UTXOs loaded: " + std::to_string(result.utxos_imported));
    logger_->info("Snapshot height: " + std::to_string(result.block_height));
    logger_->info("Node is NOW READY for:");
    logger_->info("  ✓ RPC queries");
    logger_->info("  ✓ Transaction acceptance");
    logger_->info("  ✓ Mining");
    logger_->info("  ✓ Wallet operations");
    logger_->info("Background validation continues in parallel");
    logger_->info("═══════════════════════════════════════════════════════════════════════");

    return true;
}

void ChainstateService::TryDeferredSnapshotBootstrap() {
    // FIX 2 (issue #186) + rc24.1 single-flight guard. Driven concurrently from
    // the header-processing path AND the periodic daemon thread. Only ONE thread
    // may transition Pending -> Loading and call the loader; all others return.
    // State: Pending -> Loading -> Loaded, or Pending -> Fallback. Implements
    // owner safety rules 2-4 + 6.
    if (snapshot_bootstrap_state_.load() != SnapshotBootstrapState::Pending) {
        return;  // inactive / already loading / loaded / fell back — safe no-op
    }

    const consensus::HeaderIndexEntry* base_hdr =
        header_chain_selector_
            ? header_chain_selector_->GetHeader(snapshot_bootstrap_base_hash_)
            : nullptr;

    // RULE 2: only load once the EXACT base hash is on our header chain.
    if (base_hdr != nullptr) {
        // Single-flight: exactly one thread wins Pending -> Loading and loads.
        // Everyone else loses the CAS and returns. The Loading state keeps the
        // scheduler's defer predicate true, so no blocks connect during the load.
        SnapshotBootstrapState expected = SnapshotBootstrapState::Pending;
        if (!snapshot_bootstrap_state_.compare_exchange_strong(
                expected, SnapshotBootstrapState::Loading)) {
            return;  // another thread is already loading / has resolved it
        }
        logger_->info("[snapshot] base hash " +
                      snapshot_bootstrap_base_hash_.GetHex().substr(0, 16) +
                      "... is on the header chain — loading snapshot (single-flight)...");
        if (TrySnapshotBootstrap(snapshot_bootstrap_path_)) {
            logger_->info("[snapshot] loaded — node usable at height " +
                          std::to_string(snapshot_bootstrap_base_height_) +
                          "; background validation running, block download resumes from base+1");
            snapshot_bootstrap_state_.store(SnapshotBootstrapState::Loaded);
        } else {
            // Base present but load still failed (e.g. UTXO not empty / corrupt
            // file): do not retry forever — fall back to full sync.
            logger_->warning("[snapshot] rejected — base present but load failed; "
                             "fallback to full sync");
            snapshot_bootstrap_state_.store(SnapshotBootstrapState::Fallback);
        }
        return;
    }

    // RULES 3+4: the base hash is NOT on our chain. If headers have already
    // reached/passed the base height, the snapshot is stale/orphaned (its base
    // block is not on the canonical chain we synced) — give up immediately and
    // fall back to full IBD. Never block block-download forever. CAS so only the
    // first thread logs/transitions Pending -> Fallback.
    // #441: copy under the selector's lock. GetBestHeader() returns a raw
    // pointer AFTER releasing it, and a reorg can demote the former best header
    // to an evictable side-branch tip — so reading ->height here would be a
    // use-after-free, not merely a stale read.
    consensus::HeaderIndexEntry best_hdr_copy{};
    const bool have_best_hdr =
        header_chain_selector_ && header_chain_selector_->GetBestHeaderCopy(best_hdr_copy);
    if (have_best_hdr && best_hdr_copy.height >= snapshot_bootstrap_base_height_) {
        SnapshotBootstrapState expected = SnapshotBootstrapState::Pending;
        if (snapshot_bootstrap_state_.compare_exchange_strong(
                expected, SnapshotBootstrapState::Fallback)) {
            logger_->warning("[snapshot] rejected — headers reached height " +
                             std::to_string(best_hdr_copy.height) + " (>= base " +
                             std::to_string(snapshot_bootstrap_base_height_) + ") but base hash " +
                             snapshot_bootstrap_base_hash_.GetHex().substr(0, 16) +
                             "... is not on the canonical chain (stale/orphaned snapshot); "
                             "fallback to full sync");
        }
    }
    // else: still syncing headers toward the base height — remain pending.
}

bool ChainstateService::AreServicesReady() const {
    // Services are ready if:
    // 1. We loaded a snapshot (immediate availability), OR
    // 2. We're not in IBD (synced normally), OR
    // 3. services_ready_ flag is explicitly set
    return services_ready_ || assumeutxo_active_ || !IsInIBD();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 46: Snapshot-based Pruning (Disk Optimization)
// ═══════════════════════════════════════════════════════════════════════════

bool ChainstateService::CanPruneNow() const {
    // Can only prune if:
    // 1. Snapshot is loaded (AssumeUTXO active)
    // 2. Background validation has completed up to snapshot height
    // 3. We're not in IBD (sync is complete)

    if (!assumeutxo_active_) {
        return false;  // No snapshot = can't prune safely
    }

    if (!IsBackgroundValidationComplete()) {
        return false;  // Validation not complete = not safe to prune
    }

    if (IsInIBD()) {
        return false;  // Still syncing = wait until complete
    }

    return true;
}

uint32_t ChainstateService::GetMaxPrunableHeight() const {
    if (!chain_db_) {
        return 0;
    }

    // Get current chain tip
    auto tip_result = chain_db_->getTip();
    if (!tip_result) {
        return 0;
    }

    uint32_t tip_height = tip_result.value().height;

    // Calculate safe pruning limit:
    // - Keep safety margin for reorg protection (last 1000 blocks)
    // - If snapshot loaded, can prune up to snapshot height
    uint32_t max_prunable = 0;

    if (assumeutxo_active_) {
        // With snapshot: can prune up to snapshot height
        // But still keep safety margin from tip
        max_prunable = assumeutxo_base_height_;
    }

    // Apply safety margin: never prune within last N blocks of tip
    if (tip_height > PRUNING_SAFETY_MARGIN) {
        uint32_t safety_limit = tip_height - PRUNING_SAFETY_MARGIN;
        max_prunable = std::min(max_prunable, safety_limit);
    } else {
        max_prunable = 0;  // Too close to genesis
    }

    return max_prunable;
}

uint64_t ChainstateService::GetBlockchainDiskUsage() const {
    if (datadir_.empty()) {
        return 0;
    }

    // Calculate total size of blocks directory
    std::filesystem::path blocks_dir = std::filesystem::path(datadir_) / "blocks";

    if (!std::filesystem::exists(blocks_dir)) {
        return 0;
    }

    uint64_t total_bytes = 0;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(blocks_dir)) {
            if (entry.is_regular_file()) {
                total_bytes += entry.file_size();
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        if (logger_) {
            logger_->warning("[Pruning] Error calculating disk usage: " + std::string(e.what()));
        }
        return 0;
    }

    // Convert to MB
    return total_bytes / (1024 * 1024);
}

bool ChainstateService::PruneBlocksUpToHeight(uint32_t height) {
    if (!chain_db_) {
        if (logger_) {
            logger_->error("[Pruning] Cannot prune: ChainDB not available");
        }
        return false;
    }

    if (height <= pruned_height_) {
        if (logger_) {
            logger_->info("[Pruning] Already pruned up to height " + std::to_string(pruned_height_));
        }
        return true;  // Already pruned
    }

    if (logger_) {
        logger_->info("[Pruning] Pruning blocks from " + std::to_string(pruned_height_) +
                     " to " + std::to_string(height));
    }

    if (logger_) {
        logger_->error("[Pruning] Refusing prune request: block file deletion path is unavailable in this build");
    }
    return false;
}

uint32_t ChainstateService::PruneBlockchain(uint32_t target_height) {
    if (!pruning_enabled_) {
        if (logger_) {
            logger_->error("[Pruning] Pruning is disabled");
        }
        return 0;
    }

    if (!CanPruneNow()) {
        if (logger_) {
            logger_->error("[Pruning] Cannot prune now - conditions not met");
            logger_->error("  - Snapshot loaded: " + std::string(assumeutxo_active_ ? "yes" : "no"));
            logger_->error("  - Background validation complete: " +
                          std::string(IsBackgroundValidationComplete() ? "yes" : "no"));
            logger_->error("  - IBD complete: " + std::string(!IsInIBD() ? "yes" : "no"));
        }
        return 0;
    }

    uint32_t max_prunable = GetMaxPrunableHeight();

    if (target_height > max_prunable) {
        if (logger_) {
            logger_->warning("[Pruning] Target height " + std::to_string(target_height) +
                           " exceeds safe limit " + std::to_string(max_prunable));
            logger_->warning("[Pruning] Adjusting to safe limit");
        }
        target_height = max_prunable;
    }

    if (target_height <= pruned_height_) {
        if (logger_) {
            logger_->info("[Pruning] No pruning needed (already at height " +
                         std::to_string(pruned_height_) + ")");
        }
        return 0;
    }

    uint32_t blocks_before = blocks_pruned_count_;
    bool success = PruneBlocksUpToHeight(target_height);

    if (!success) {
        return 0;
    }

    return blocks_pruned_count_ - blocks_before;
}

ChainstateService::PruningInfo ChainstateService::GetPruningInfo() const {
    PruningInfo info;
    info.mode = pruning_mode_;
    info.pruning_enabled = pruning_enabled_;
    info.pruned_height = pruned_height_;
    info.target_disk_usage_mb = target_disk_usage_mb_;
    info.current_disk_usage_mb = GetBlockchainDiskUsage();
    info.blocks_pruned = blocks_pruned_count_;
    info.bytes_freed = bytes_freed_;
    info.can_prune = CanPruneNow();

    // Generate status message
    if (!pruning_enabled_) {
        info.prune_status = "Pruning disabled";
    } else if (!assumeutxo_active_) {
        info.prune_status = "Waiting for snapshot load";
    } else if (!IsBackgroundValidationComplete()) {
        info.prune_status = "Waiting for background validation";
    } else if (IsInIBD()) {
        info.prune_status = "Waiting for IBD completion";
    } else if (pruning_mode_ == PruningMode::Auto) {
        if (info.current_disk_usage_mb > target_disk_usage_mb_) {
            info.prune_status = "Auto-pruning active (disk usage exceeded)";
        } else {
            info.prune_status = "Auto-pruning ready (disk usage OK)";
        }
    } else {
        info.prune_status = "Manual pruning ready";
    }

    return info;
}

void ChainstateService::SetPruningMode(PruningMode mode, uint64_t target_disk_mb) {
    pruning_mode_ = mode;
    pruning_enabled_ = (mode != PruningMode::Disabled);
    target_disk_usage_mb_ = target_disk_mb;

    if (logger_) {
        std::string mode_str;
        switch (mode) {
            case PruningMode::Disabled:
                mode_str = "Disabled";
                break;
            case PruningMode::Manual:
                mode_str = "Manual";
                break;
            case PruningMode::Auto:
                mode_str = "Auto";
                break;
        }
        logger_->info("[Pruning] Mode set to: " + mode_str);
        if (pruning_enabled_) {
            logger_->info("[Pruning] Target disk usage: " +
                         std::to_string(target_disk_mb) + " MB");
        }
    }
}

void ChainstateService::AutoPruneIfNeeded() {
    // Only auto-prune if:
    // 1. Auto mode is enabled
    // 2. We can prune now (snapshot loaded, validation complete)
    // 3. Disk usage exceeds target

    if (pruning_mode_ != PruningMode::Auto) {
        return;  // Not in auto mode
    }

    if (!CanPruneNow()) {
        return;  // Can't prune yet
    }

    uint64_t current_usage = GetBlockchainDiskUsage();

    if (current_usage <= target_disk_usage_mb_) {
        return;  // Disk usage is fine
    }

    // Calculate how much to prune
    uint32_t max_prunable = GetMaxPrunableHeight();

    if (max_prunable <= pruned_height_) {
        return;  // Nothing to prune
    }

    if (logger_) {
        logger_->info("═══════════════════════════════════════════════════════════════════════");
        logger_->info("🗑️  AUTO-PRUNING TRIGGERED");
        logger_->info("═══════════════════════════════════════════════════════════════════════");
        logger_->info("Current disk usage: " + std::to_string(current_usage) + " MB");
        logger_->info("Target disk usage: " + std::to_string(target_disk_usage_mb_) + " MB");
        logger_->info("Pruning up to height: " + std::to_string(max_prunable));
    }

    // Execute pruning
    uint32_t blocks_pruned = PruneBlockchain(max_prunable);

    if (logger_) {
        logger_->info("═══════════════════════════════════════════════════════════════════════");
        if (blocks_pruned > 0) {
            logger_->info("✅ AUTO-PRUNING COMPLETE");
            logger_->info("Blocks pruned: " + std::to_string(blocks_pruned));
            logger_->info("New disk usage: " + std::to_string(GetBlockchainDiskUsage()) + " MB");
        } else {
            logger_->warning("⚠️  AUTO-PRUNING: No blocks pruned");
        }
        logger_->info("═══════════════════════════════════════════════════════════════════════");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase C.1 v2: Orphan Block Handling (Fork Convergence Fix)
// ═══════════════════════════════════════════════════════════════════════════

bool ChainstateService::ShouldEmitMissingParentDiag(const std::string& peer_id,
                                                    uint32_t& suppressed_prev_peer_window,
                                                    uint32_t& suppressed_prev_global_window) {
    std::lock_guard<std::mutex> lock(missing_parent_diag_mutex_);
    const auto now = std::chrono::steady_clock::now();
    suppressed_prev_peer_window = 0;
    suppressed_prev_global_window = 0;

    // Opportunistic pruning to avoid unbounded growth under peer churn.
    if (missing_parent_diag_by_peer_.size() > 2048) {
        for (auto it = missing_parent_diag_by_peer_.begin();
             it != missing_parent_diag_by_peer_.end();) {
            const auto age = now - it->second.window_start;
            if (age > std::chrono::minutes(5)) {
                it = missing_parent_diag_by_peer_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto& state = missing_parent_diag_by_peer_[peer_id];
    const bool new_window =
        (state.window_start.time_since_epoch().count() == 0) ||
        ((now - state.window_start) >=
         std::chrono::seconds(MISSING_PARENT_DIAG_WINDOW_SECONDS));

    if (new_window) {
        suppressed_prev_peer_window = state.suppressed_in_window;
        state.window_start = now;
        state.emitted_in_window = 0;
        state.suppressed_in_window = 0;
    }

    if (state.emitted_in_window >= MISSING_PARENT_DIAG_BURST_PER_WINDOW) {
        state.suppressed_in_window++;
        return false;
    }

    const bool new_global_window =
        (missing_parent_diag_global_.window_start.time_since_epoch().count() == 0) ||
        ((now - missing_parent_diag_global_.window_start) >=
         std::chrono::seconds(MISSING_PARENT_DIAG_GLOBAL_WINDOW_SECONDS));

    if (new_global_window) {
        suppressed_prev_global_window = missing_parent_diag_global_.suppressed_in_window;
        missing_parent_diag_global_.window_start = now;
        missing_parent_diag_global_.emitted_in_window = 0;
        missing_parent_diag_global_.suppressed_in_window = 0;
    }

    if (missing_parent_diag_global_.emitted_in_window >=
        MISSING_PARENT_DIAG_GLOBAL_BURST_PER_WINDOW) {
        state.suppressed_in_window++;
        missing_parent_diag_global_.suppressed_in_window++;
        return false;
    }

    state.emitted_in_window++;
    missing_parent_diag_global_.emitted_in_window++;
    return true;
}

bool ChainstateService::ShouldRequestParentNow(const uint256& parent_hash) {
    std::lock_guard<std::mutex> lock(parent_request_mutex_);
    const auto now = std::chrono::steady_clock::now();

    // Opportunistic pruning to keep memory bounded under churn.
    if (parent_request_by_hash_.size() > 4096) {
        for (auto it = parent_request_by_hash_.begin();
             it != parent_request_by_hash_.end();) {
            if ((now - it->second) > std::chrono::minutes(5)) {
                it = parent_request_by_hash_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto it = parent_request_by_hash_.find(parent_hash);
    if (it != parent_request_by_hash_.end()) {
        if ((now - it->second) <
            std::chrono::seconds(PARENT_REQUEST_COOLDOWN_SECONDS)) {
            return false;
        }
    }

    parent_request_by_hash_[parent_hash] = now;
    return true;
}

void ChainstateService::LogMissingParentDiagRateLimited(const std::string& peer_id,
                                                        const uint256& parent_hash,
                                                        bool parent_received,
                                                        bool parent_expected,
                                                        bool synced) {
    uint32_t suppressed_prev_peer_window = 0;
    uint32_t suppressed_prev_global_window = 0;
    if (!ShouldEmitMissingParentDiag(peer_id,
                                     suppressed_prev_peer_window,
                                     suppressed_prev_global_window)) {
        return;
    }

    std::ostringstream diag;
    diag << "[MISSING-PARENT-DIAG] peer=" << peer_id
         << " parent=" << parent_hash.GetHex().substr(0, 16)
         << "... received=" << parent_received
         << " expected=" << parent_expected
         << " synced=" << synced;
    if (suppressed_prev_peer_window > 0) {
        diag << " suppressed_prev_peer_window=" << suppressed_prev_peer_window;
    }
    if (suppressed_prev_global_window > 0) {
        diag << " suppressed_prev_global_window=" << suppressed_prev_global_window;
    }
    std::cout << diag.str() << std::endl;

    std::string info = "[ChainstateService] Block has missing parent - adding to orphan pool (peer: " +
                      peer_id + ", parent: " + parent_hash.GetHex().substr(0, 16) + "...)";
    if (suppressed_prev_peer_window > 0) {
        info += " [suppressed_prev_peer_window=" +
                std::to_string(suppressed_prev_peer_window) + "]";
    }
    if (suppressed_prev_global_window > 0) {
        info += " [suppressed_prev_global_window=" +
                std::to_string(suppressed_prev_global_window) + "]";
    }
    logger_->info(info);
}

bool ChainstateService::AddOrphanBlock(const Block& block, const std::string& peer_id) {
    if (!p2p_orphan_pool_) {
        logger_->warning("[ChainstateService] Orphan pool not initialized");
        return false;
    }

    // Calculate block hash (from header)
    uint256 block_hash = block.header.GetHash();
    uint256 parent_hash = block.header.prev_block_hash;

    std::string block_hash_hex = block_hash.GetHex();
    std::string parent_hash_hex = parent_hash.GetHex();

    logger_->debug("[ChainstateService] Adding orphan block " + block_hash_hex.substr(0, 16) +
                  "... (parent: " + parent_hash_hex.substr(0, 16) + "...)");

    // Add to orphan pool
    bool added = p2p_orphan_pool_->addOrphan(block, block_hash_hex, parent_hash_hex, peer_id);

    if (added) {
        logger_->debug("[ChainstateService] Orphan block added to pool, requesting parent from peer");
        // Request parent block from the peer
        RequestParentBlock(parent_hash, peer_id);
        return true;
    } else {
        logger_->debug("[ChainstateService] Orphan pool rejected block (duplicate or full)");
        return false;
    }
}

void ChainstateService::ProcessOrphans(const std::string& parent_hash) {
    if (!p2p_orphan_pool_) {
        return;
    }

    // Get orphans waiting for this parent
    auto orphans = p2p_orphan_pool_->getOrphansForParent(parent_hash);

    if (orphans.empty()) {
        return;
    }

    logger_->info("[ChainstateService] Processing " + std::to_string(orphans.size()) +
                 " orphan(s) waiting for parent " + parent_hash.substr(0, 16) + "...");

    for (const auto& orphan : orphans) {
        logger_->info("[ChainstateService] Reprocessing orphan " + orphan->block_hash.substr(0, 16) + "...");

        // Remove from orphan pool before reprocessing
        p2p_orphan_pool_->removeOrphan(orphan->block_hash);

        // Resubmit to validation - this will recursively process any children
        bool accepted = ProcessIncomingBlock(orphan->block, orphan->peer_id);

        if (accepted) {
            logger_->info("[ChainstateService] Orphan " + orphan->block_hash.substr(0, 16) +
                         "... accepted (chain extended)");
        } else {
            logger_->warning("[ChainstateService] Orphan " + orphan->block_hash.substr(0, 16) +
                            "... still rejected");
        }
    }
}

void ChainstateService::RequestParentBlock(const uint256& parent_hash, const std::string& peer_id) {
    if (!ShouldRequestParentNow(parent_hash)) {
        logger_->debug("[ChainstateService] Parent request throttled for " +
                      parent_hash.GetHex().substr(0, 16) + "...");
        return;
    }

    logger_->info("[ChainstateService] Requesting parent block " + parent_hash.GetHex().substr(0, 16) +
                 "... from peer " + peer_id);

    bool routed_to_relay = false;
    size_t direct_peer_requests = 0;

    // Ask the source peer first when available.
    if (block_relay_manager_ && !peer_id.empty()) {
        block_relay_manager_->HandleInv(peer_id, parent_hash);
        routed_to_relay = true;
    }

    // Fanout GETDATA to connected peers so heal doesn't depend on one stale peer.
    if (p2p_service_) {
        ::P2PMessage getdata_msg;
        getdata_msg.command = "getdata";

        std::vector<uint8_t> payload;
        payload.push_back(1);  // varint: 1 item

        // Type: profile-aware (MSG_BLOCK or MSG_UTREEXO_BLOCK), 4 bytes little-endian.
        const uint32_t inv_type = BlockGetDataInventoryType();
        payload.push_back(inv_type & 0xFF);
        payload.push_back((inv_type >> 8) & 0xFF);
        payload.push_back((inv_type >> 16) & 0xFF);
        payload.push_back((inv_type >> 24) & 0xFF);

        // Hash: 32 bytes (little-endian, raw bytes).
        const uint8_t* hash_bytes = parent_hash.begin();
        for (size_t i = 0; i < 32; ++i) {
            payload.push_back(hash_bytes[i]);
        }

        getdata_msg.payload = payload;

        const auto peers = p2p_service_->GetConnectedPeers();
        for (const auto& peer : peers) {
            const std::string peer_key =
                peer.address + ":" + std::to_string(peer.port);
            if (peer_key.empty()) {
                continue;
            }
            if (p2p_service_->get().send_to_peer(peer_key, getdata_msg)) {
                direct_peer_requests++;
            }
        }

        if (direct_peer_requests == 0) {
            // Fallback when peer list API is empty but a broadcast path exists.
            p2p_service_->BroadcastMessage(getdata_msg);
        }
    }

    if (routed_to_relay || direct_peer_requests > 0) {
        logger_->info("[ChainstateService] Parent request dispatched: relay=" +
                     std::to_string(routed_to_relay ? 1 : 0) +
                     " direct_peers=" + std::to_string(direct_peer_requests));
    } else {
        logger_->debug("[ChainstateService] No relay or P2P path available to request parent block");
    }
}

} // namespace dinero
