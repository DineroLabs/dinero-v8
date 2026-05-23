#include "daemon/daemon_app.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "daemon/chainstate_recovery_marker.h"
#include "daemon/header_metadata_recovery.h"
#include "daemon/undo_rebuild_orchestrator.h"  // Commit #5: --rebuild-undo-range
#include "daemon/config.h"  // Phase 8: For GetConfig()
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/block_ingress_service.h"  // Step 5: IBlockIngress implementation
#include "dinero/daemon/block_acceptor.h"
#include "daemon/services/wallet_service.h"
#include "wallet/hd_wallet.h"
#include "wallet/wallet_worker.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/rpc_service.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/metrics_service.h"
#include "daemon/utreexo_proof_mode.h"
#include "daemon/block_relay_manager.h"  // Phase G.2: Block propagation
#include "daemon/tx_relay_manager.h"  // Phase G.3: Mempool relay
#include "mempool/tx_orphan_pool.h"  // Transaction orphan pool
#include "network/bridge_node.h"  // Phase P.2: Utreexo proof generation for stateless clients
#include "network/stateless_node.h"  // Phase P.3: CSN block+proof validation
#include "network/utreexo_messages.h"  // Phase P.3: UtreexoProofMessage for CSN validation
#include "network/types.h"  // ServiceFlags capability gating for proof-serving requests
#include "consensus/adapters/wallet_utxo_adapter.h"  // v2.2.0: UTXO adapter for consensus interface
#include "mining/block_assembler.h"  // Phase C: Block template assembly
#include "daemon/services/peer_scoring_service.h"  // Phase 5D: DoS protection
#include "daemon/services/headers_sync_service.h"  // Phase 5A: Headers-first sync
#include "daemon/services/compact_block_service.h"  // Phase 5B: Compact blocks
#include "daemon/services/address_manager_service.h"  // Phase 5C: Address manager
#include "daemon/services/rbf_policy_service.h"  // Phase 5E: RBF policy
#include "daemon/services/prune_service.h"  // Phase 34.8: Block pruning
#include "consensus/pow_consensus_engine.h"  // Phase 2: Consensus engine
#include "ipc/oracles/chain_oracle_client.h"  // Phase 9.2: Chain oracle for lightningd
#include "ipc/oracles/time_oracle_client.h"   // Phase 9.2: Time oracle for lightningd
#include "ipc/oracles/transaction_oracle_client.h"  // Phase 9.2: Transaction oracle for lightningd
#include "ipc/watch_registration_server.h"  // Phase 9.3: Bidirectional oracle communication
// Phase 39: chain_manager.h deleted (ChainManager removed)
#include "consensus/chainparams.h"  // Chainparams for genesis workaround
#include "consensus/consensus.hpp"  // Consensus parameters (difficulty floor logging)
#include "consensus/validation_mode.h"  // Phase 8: Stateless validation mode
#include "consensus/chainstate_guard.h"  // Phase 6B: Thread-safe UTXO access
#include "consensus/reindexer.h"  // Blockchain reindex functionality
#include "wallet/utxo_index.h"    // For clearing stale reorg_in_progress marker after reindex
#include "consensus/parallel_block_validator.h"  // Phase 6B: Parallel validation
#include "consensus/validation_queue.h"  // Phase 6B: ValidationQueue wiring
#include "consensus/cpu_budget_monitor.h"  // Phase E.2.d / E.3.1: CPU budget monitoring
#include "consensus/startup_validator.h"  // Phase E.1: Startup consistency validation
#include "consensus/header_sync_p2p.h"  // Phase N.3: Header sync P2P integration
#include "consensus/header_chain.h"  // Phase N.3: Header chain selector
#include "consensus/header_store.h"  // Phase N.3: Header storage
#include "consensus/active_chain_ancestry.h"
#include "consensus/block_download_scheduler.h"  // Phase N.4: Block download scheduler
#include "consensus/proof_gossip.h"  // Phase 9.3: Proof availability gossip
#include "p2p/block_download_scheduler.h"  // Phase G: Parallel block download
#include "consensus/block_lifecycle.h"  // Phase P.2: BLOCK_HAVE_DATA flag
#include "common/serialization.h"  // Phase N: Block deserialization
#include "primitives/transaction.h"  // Pool payout callback tx decode + txid
#include "common/crash_injection.h"
#include <sstream>  // P2P sync fix: For parsing pipe-separated headers
#include "storage/archival_block_reader.h"
#include "storage/chain_db.h"  // ONE DB: Direct ChainDB construction
#include "storage/block_storage.h"  // Block storage for reindex operation
#include "storage/disk_space_monitor.h"  // Phase E.2.b: Disk space monitoring
#include "p2p/network_limits_monitor.h"  // Phase E.2.c: Network limits monitoring
#include "common/status.h"  // ONE DB: Status enum
#include "daemon/genesis_init.hpp"  // Genesis initialization
#include "common/logger.h"
#include "common/production_logger.h"  // Logger dependency injection
#include "common/json_logger.h"  // Per-service JSON logging
#include "common/logger_router.h"  // Unified log aggregator
#include "rpc/methods_pool.h"  // Pool accounting RPC feature gate
#include "pool/pool_manager.h"  // Pool accounting manager (worker/share ingress)
// Optional services
#include "rpc/event_bus.h"
#include "bridge/fiat_bridge_manager.h"
#include "p2p/marketplace_manager.h"
#include "p2p/escrow_manager.h"
// NOTE: Stratum server removed from dinerod - use separate dinero-stratum binary
// #include "stratum/stratum_server.h"  // Stratum V1 mining server
// #include "stratum/ssl_cert_generator.h"  // P4: SSL certificate generation
#include "daemon/ws_globals.h"  // Phase 3F: WebSocket subscription manager (g_subscriptions)
// Phase 3: Lightning moved to separate process (lightningd)
// #include "lightning/lightning_service.h"  // NO LONGER USED - Lightning is external
#ifndef DISABLE_GRPC
#include "grpc/grpc_server.h"  // Phase 3: gRPC server for Lightning wallet API (dev mode only)
#endif
#include "grpc/socket_wallet_server.h"  // Phase 5: Socket wallet server (always enabled)
#include <iostream>
#include <fstream>  // Phase F.2: Mining state persistence
#include <atomic>
#include <cassert>
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>

namespace dinero {

// ============================================================================
// Phase N: Helper Functions for Message Parsing
// ============================================================================

namespace {

// Helper: Convert bytes to hex string
std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

// Helper: Convert hex string to bytes
static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        char* end_ptr = nullptr;
        long val = std::strtol(hex.substr(i, 2).c_str(), &end_ptr, 16);
        bytes.push_back(static_cast<uint8_t>(val));
    }
    return bytes;
}

using ShutdownClock = std::chrono::steady_clock;

void LogShutdownPhase(const char* phase,
                      const ShutdownClock::time_point& start,
                      const std::string& detail = {}) {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ShutdownClock::now() - start).count();
    std::cout << "[ShutdownPhase] phase=" << phase
              << " elapsed_ms=" << elapsed_ms;
    if (!detail.empty()) {
        std::cout << " detail=\"" << detail << "\"";
    }
    std::cout << std::endl;
}

std::string SanitizeHeaderStoreReason(const std::string& reason) {
    std::string sanitized;
    sanitized.reserve(reason.size());

    bool last_was_dash = false;
    for (unsigned char ch : reason) {
        if (std::isalnum(ch)) {
            sanitized.push_back(static_cast<char>(std::tolower(ch)));
            last_was_dash = false;
        } else if (!last_was_dash) {
            sanitized.push_back('-');
            last_was_dash = true;
        }
    }

    while (!sanitized.empty() && sanitized.front() == '-') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '-') {
        sanitized.pop_back();
    }

    if (sanitized.empty()) {
        sanitized = "schema-recovery";
    }
    if (sanitized.size() > 48) {
        sanitized.resize(48);
    }
    return sanitized;
}

std::string HeaderStoreBackupTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now);
#else
    localtime_r(&now, &local_tm);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local_tm);
    return buffer;
}

std::filesystem::path BuildHeaderStoreBackupPath(const std::filesystem::path& headers_path,
                                                 const std::string& reason) {
    const std::string stem = headers_path.filename().string() +
                             ".backup-" + HeaderStoreBackupTimestamp() +
                             "-" + SanitizeHeaderStoreReason(reason);
    std::filesystem::path candidate = headers_path.parent_path() / stem;

    std::error_code ec;
    for (int suffix = 1; std::filesystem::exists(candidate, ec) && !ec; ++suffix) {
        candidate = headers_path.parent_path() /
                    (stem + "-" + std::to_string(suffix));
    }

    return candidate;
}

struct ReindexPromotionJournalEntry {
    std::string label;
    std::filesystem::path live_path;
    std::filesystem::path temp_path;
    std::filesystem::path backup_path;
    bool sqlite_sidecars{false};
};

std::filesystem::path ReindexPromotionJournalPath(const std::filesystem::path& data_dir_path) {
    return data_dir_path / "blockchain" / "reindex_promotion.marker";
}

bool RenameIfExists(const std::filesystem::path& from,
                    const std::filesystem::path& to,
                    std::error_code& ec) {
    if (!std::filesystem::exists(from, ec) || ec) {
        ec.clear();
        return true;
    }
    std::filesystem::rename(from, to, ec);
    return !ec;
}

bool WriteReindexPromotionJournal(const std::filesystem::path& journal_path,
                                  const std::vector<ReindexPromotionJournalEntry>& entries,
                                  std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(journal_path.parent_path(), ec);
    if (ec) {
        if (error) {
            *error = "create-directories: " + ec.message();
        }
        return false;
    }

    std::ofstream out(journal_path, std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "open-journal-for-write";
        }
        return false;
    }

    for (const auto& entry : entries) {
        out << std::quoted(entry.label) << '\t'
            << (entry.sqlite_sidecars ? 1 : 0) << '\t'
            << std::quoted(entry.live_path.string()) << '\t'
            << std::quoted(entry.temp_path.string()) << '\t'
            << std::quoted(entry.backup_path.string()) << '\n';
    }

    out.flush();
    if (!out) {
        if (error) {
            *error = "flush-journal";
        }
        return false;
    }

    return true;
}

bool ReadReindexPromotionJournal(const std::filesystem::path& journal_path,
                                 std::vector<ReindexPromotionJournalEntry>* entries,
                                 std::string* error) {
    std::ifstream in(journal_path);
    if (!in) {
        if (error) {
            *error = "open-journal-for-read";
        }
        return false;
    }

    std::vector<ReindexPromotionJournalEntry> parsed;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        ReindexPromotionJournalEntry entry;
        int sqlite_sidecars = 0;
        std::string live_path;
        std::string temp_path;
        std::string backup_path;
        if (!(iss >> std::quoted(entry.label) >> sqlite_sidecars >>
              std::quoted(live_path) >> std::quoted(temp_path) >> std::quoted(backup_path))) {
            if (error) {
                *error = "parse-journal-line";
            }
            return false;
        }
        entry.sqlite_sidecars = sqlite_sidecars != 0;
        entry.live_path = live_path;
        entry.temp_path = temp_path;
        entry.backup_path = backup_path;
        parsed.push_back(std::move(entry));
    }

    if (entries) {
        *entries = std::move(parsed);
    }
    return true;
}

void RemoveSqliteSidecars(const std::filesystem::path& base_path, std::error_code& ec) {
    std::filesystem::remove(base_path.string() + "-wal", ec);
    ec.clear();
    std::filesystem::remove(base_path.string() + "-shm", ec);
    ec.clear();
}

bool RenameSqliteSidecarsIfPresent(const std::filesystem::path& from_base,
                                   const std::filesystem::path& to_base,
                                   std::error_code& ec) {
    if (!RenameIfExists(from_base.string() + "-wal", to_base.string() + "-wal", ec)) {
        return false;
    }
    if (!RenameIfExists(from_base.string() + "-shm", to_base.string() + "-shm", ec)) {
        return false;
    }
    return true;
}

bool RecoverInterruptedReindexPromotion(const std::filesystem::path& data_dir_path,
                                        std::string* error) {
    const auto journal_path = ReindexPromotionJournalPath(data_dir_path);
    if (!std::filesystem::exists(journal_path)) {
        return true;
    }

    std::vector<ReindexPromotionJournalEntry> entries;
    if (!ReadReindexPromotionJournal(journal_path, &entries, error)) {
        return false;
    }

    std::error_code ec;
    bool any_temp_entries = false;
    for (const auto& entry : entries) {
        if (std::filesystem::exists(entry.temp_path, ec) && !ec) {
            any_temp_entries = true;
            break;
        }
        ec.clear();
    }

    for (const auto& entry : entries) {
        const bool live_exists = std::filesystem::exists(entry.live_path, ec);
        if (ec) {
            if (error) {
                *error = "stat-live-path-" + entry.label + ": " + ec.message();
            }
            return false;
        }
        const bool temp_exists = std::filesystem::exists(entry.temp_path, ec);
        if (ec) {
            if (error) {
                *error = "stat-temp-path-" + entry.label + ": " + ec.message();
            }
            return false;
        }
        const bool backup_exists = std::filesystem::exists(entry.backup_path, ec);
        if (ec) {
            if (error) {
                *error = "stat-backup-path-" + entry.label + ": " + ec.message();
            }
            return false;
        }

        if (temp_exists) {
            if (live_exists && !backup_exists) {
                std::filesystem::rename(entry.live_path, entry.backup_path, ec);
                if (ec) {
                    if (error) {
                        *error = "backup-live-during-recovery-" + entry.label + ": " + ec.message();
                    }
                    return false;
                }
                if (entry.sqlite_sidecars &&
                    !RenameSqliteSidecarsIfPresent(entry.live_path, entry.backup_path, ec)) {
                    if (error) {
                        *error = "backup-sqlite-sidecars-during-recovery-" + entry.label + ": " + ec.message();
                    }
                    return false;
                }
            }

            std::filesystem::rename(entry.temp_path, entry.live_path, ec);
            if (ec) {
                if (error) {
                    *error = "promote-temp-during-recovery-" + entry.label + ": " + ec.message();
                }
                return false;
            }
            if (entry.sqlite_sidecars &&
                !RenameSqliteSidecarsIfPresent(entry.temp_path, entry.live_path, ec)) {
                if (error) {
                    *error = "promote-sqlite-sidecars-during-recovery-" + entry.label + ": " + ec.message();
                }
                return false;
            }
        } else if (!live_exists && backup_exists) {
            std::filesystem::rename(entry.backup_path, entry.live_path, ec);
            if (ec) {
                if (error) {
                    *error = "restore-backup-during-recovery-" + entry.label + ": " + ec.message();
                }
                return false;
            }
            if (entry.sqlite_sidecars &&
                !RenameSqliteSidecarsIfPresent(entry.backup_path, entry.live_path, ec)) {
                if (error) {
                    *error = "restore-sqlite-sidecars-during-recovery-" + entry.label + ": " + ec.message();
                }
                return false;
            }
        } else if (!live_exists && !backup_exists) {
            if (error) {
                *error = "missing-live-and-backup-during-recovery-" + entry.label;
            }
            return false;
        } else if (any_temp_entries && live_exists && backup_exists) {
            // Another artifact still had a temp rebuild pending, so preserve
            // the fully promoted live artifact and keep its pre-reindex backup.
        }

        RemoveSqliteSidecars(entry.temp_path, ec);
        std::filesystem::remove_all(entry.temp_path, ec);
        ec.clear();
    }

    std::filesystem::remove(journal_path, ec);
    if (ec) {
        if (error) {
            *error = "remove-journal: " + ec.message();
        }
        return false;
    }

    return true;
}

bool QuarantineHeaderStoreDirectory(const std::filesystem::path& headers_path,
                                    const std::filesystem::path& backup_path,
                                    std::string& detail) {
    std::error_code ec;
    if (!std::filesystem::exists(headers_path, ec) || ec) {
        detail = "source header store directory is missing";
        return true;
    }

    std::filesystem::rename(headers_path, backup_path, ec);
    if (!ec) {
        detail = "renamed in place";
        return true;
    }

    const std::string rename_error = ec.message();
    ec.clear();
    std::filesystem::copy(headers_path,
                          backup_path,
                          std::filesystem::copy_options::recursive,
                          ec);
    if (ec) {
        detail = "rename failed (" + rename_error + "); copy failed (" + ec.message() + ")";
        return false;
    }

    ec.clear();
    std::filesystem::remove_all(headers_path, ec);
    if (ec) {
        detail = "copied to backup but failed to remove source (" + ec.message() + ")";
        return false;
    }

    detail = "copied recursively after rename failed (" + rename_error + ")";
    return true;
}

// Helper: Parse headers from P2P message
// P2P sync fix: Parse pipe-separated hex format from create_headers()
// BlockHeader v1 is 128 bytes (see include/primitives/block.h):
//   0x00: version (4 bytes)
//   0x04: prev_block_hash (32 bytes)
//   0x24: merkle_root (32 bytes)
//   0x44: utreexo_root (32 bytes)
//   0x64: timestamp (8 bytes)
//   0x6C: difficulty (4 bytes)
//   0x70: nonce (4 bytes)
//   0x74: reserved (12 bytes)
std::vector<BlockHeader> ParseHeadersFromP2PMessage(const ::P2PMessage& msg) {
    std::vector<BlockHeader> headers;
    const auto& payload = msg.payload;

    // Bitcoin wire format: varint(count) + (header_bytes + varint(tx_count))*N
    // Dinero headers are 128 bytes (not Bitcoin's 80 bytes)
    // tx_count is always 0 for headers message

    if (payload.size() < 1) {
        g_logger.warning("[ParseHeaders] Empty headers payload");
        return headers;
    }

    size_t offset = 0;

    // Read varint for header count
    auto read_varint = [&]() -> uint64_t {
        if (offset >= payload.size()) return 0;
        uint8_t first = payload[offset++];
        if (first < 0xFD) {
            return first;
        } else if (first == 0xFD) {
            if (offset + 2 > payload.size()) return 0;
            uint64_t val = payload[offset] | (static_cast<uint64_t>(payload[offset + 1]) << 8);
            offset += 2;
            return val;
        } else if (first == 0xFE) {
            if (offset + 4 > payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 4; i++) val |= static_cast<uint64_t>(payload[offset + i]) << (i * 8);
            offset += 4;
            return val;
        } else {
            if (offset + 8 > payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 8; i++) val |= static_cast<uint64_t>(payload[offset + i]) << (i * 8);
            offset += 8;
            return val;
        }
    };

    uint64_t count = read_varint();
    if (count == 0) {
        // Zero headers is a valid "nothing new" response (peer at same tip).
        return headers;
    }
    if (count > 2000) {
        g_logger.warning("[ParseHeaders] Invalid header count: " + std::to_string(count));
        return headers;
    }

    // Parse each header (128 bytes) + tx_count varint (should be 0)
    for (uint64_t i = 0; i < count; i++) {
        // Check we have enough bytes for 128-byte header
        if (offset + 128 > payload.size()) {
            g_logger.warning("[ParseHeaders] Header too short: " + std::to_string(payload.size() - offset) +
                           " bytes remaining (expected 128)");
            break;
        }

        // Parse the 128-byte header (little-endian Dinero format)
        BlockHeader header;
        header.version = *reinterpret_cast<const uint32_t*>(&payload[offset + 0x00]);

        // prev_block_hash (32 bytes at offset 0x04)
        std::memcpy(header.prev_block_hash.data, &payload[offset + 0x04], 32);

        // merkle_root (32 bytes at offset 0x24)
        std::memcpy(header.merkle_root.data, &payload[offset + 0x24], 32);

        // utreexo_root (32 bytes at offset 0x44)
        std::memcpy(header.utreexo_root.data, &payload[offset + 0x44], 32);

        // timestamp (8 bytes at offset 0x64)
        header.timestamp = *reinterpret_cast<const uint64_t*>(&payload[offset + 0x64]);

        // difficulty (4 bytes at offset 0x6C)
        header.difficulty = *reinterpret_cast<const uint32_t*>(&payload[offset + 0x6C]);

        // nonce (4 bytes at offset 0x70)
        header.nonce = *reinterpret_cast<const uint32_t*>(&payload[offset + 0x70]);

        // reserved (12 bytes at offset 0x74) - must be zero, stored in struct
        std::memcpy(header.reserved, &payload[offset + 0x74], 12);

        offset += 128;  // Move past header

        // Read tx_count varint (should be 0)
        read_varint();

        headers.push_back(header);
    }

    g_logger.info("[ParseHeaders] Parsed " + std::to_string(headers.size()) + " headers from P2P message");
    return headers;
}

// Helper: Deserialize block from P2P message
Block DeserializeBlockFromP2PMessage(const ::P2PMessage& msg) {
    auto block_opt = Block::Deserialize(
        reinterpret_cast<const uint8_t*>(msg.payload.data()),
        msg.payload.size());
    if (!block_opt.has_value()) {
        throw std::runtime_error("Block::Deserialize failed");
    }
    return *block_opt;
}

Transaction DeserializeTransactionFromP2PMessage(const ::P2PMessage& msg) {
    Reader reader(msg.payload);
    Transaction tx;
    Deserialize(reader, tx);
    return tx;
}

// Peer ID mapping (simple hash-based for now)
// TODO: Replace with proper bidirectional mapping maintained by P2PService
std::unordered_map<std::string, uint64_t> g_peer_addr_to_id;
std::unordered_map<uint64_t, std::string> g_peer_id_to_addr;

uint64_t GetPeerID(const std::string& peer_addr) {
    auto it = g_peer_addr_to_id.find(peer_addr);
    if (it != g_peer_addr_to_id.end()) {
        return it->second;
    }
    // Create new ID
    uint64_t peer_id = std::hash<std::string>{}(peer_addr);
    g_peer_addr_to_id[peer_addr] = peer_id;
    g_peer_id_to_addr[peer_id] = peer_addr;
    return peer_id;
}

std::string GetPeerAddress(uint64_t peer_id) {
    auto it = g_peer_id_to_addr.find(peer_id);
    return (it != g_peer_id_to_addr.end()) ? it->second : "";
}

} // anonymous namespace

// Constructor implementation (must be in .cpp to allow unique_ptr with forward-declared types)
DaemonApp::DaemonApp() = default;

// Destructor implementation (must be in .cpp to allow unique_ptr with forward-declared types)
DaemonApp::~DaemonApp() {
    try {
        Stop();
    } catch (...) {
        std::cerr << "[DaemonApp] Exception in destructor during Stop()" << std::endl;
    }
    // Explicitly reset members that might throw during destruction.
    try { chain_db_.reset(); } catch (...) {
        std::cerr << "[DaemonApp] Exception destroying ChainDB" << std::endl;
    }
    try { socket_wallet_server_.reset(); } catch (...) {}
    #ifndef DISABLE_GRPC
    try { grpc_server_.reset(); } catch (...) {}
    #endif
}

bool DaemonApp::Init(int argc, char** argv) {
    std::cout << "[DaemonApp] Initializing services..." << std::endl;

    // Defensive reset for repeated Init() on the same DaemonApp instance.
    // Drop any retained service pointers/DB handles from prior attempts.
    auto request_shutdown = std::move(ctx_.request_shutdown);
    ctx_ = DaemonContext{};
    ctx_.request_shutdown = std::move(request_shutdown);
    services_.clear();
    chain_db_.reset();

    bool init_succeeded = false;
    struct InitFailureGuard {
        DaemonApp* self;
        bool* success;
        ~InitFailureGuard() {
            if (!self || !success || *success) {
                return;
            }
            // Init() failed before Start(); aggressively clear partial state so a
            // subsequent in-process retry does not inherit stale singletons/handles.
            auto request_shutdown = std::move(self->ctx_.request_shutdown);
            DaemonContext::setInstance(nullptr);
            self->ctx_ = DaemonContext{};
            self->ctx_.request_shutdown = std::move(request_shutdown);
            self->services_.clear();
            self->chain_db_.reset();
            self->wallet_logger_.reset();
            self->p2p_logger_.reset();
            self->mining_logger_.reset();
            self->mempool_logger_.reset();
            self->logger_router_.reset();
        }
    } init_failure_guard{this, &init_succeeded};

    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: Verify BlockHeader serialization round-trip at startup
    // This catches field order mismatches that break P2P relay (timestamp corruption)
    // ═══════════════════════════════════════════════════════════════════════════
    if (!VerifyBlockHeaderSerializationRoundTrip()) {
        std::cerr << "FATAL: BlockHeader serialization round-trip failed!" << std::endl;
        std::cerr << "This indicates a field order mismatch between Serialize/Deserialize." << std::endl;
        std::cerr << "Check include/common/serialization.h matches block.cpp SerializeForHash()." << std::endl;
        return false;
    }
    std::cout << "[DaemonApp] ✅ BlockHeader serialization round-trip verified" << std::endl;

    // Register DaemonContext as global singleton (for legacy global shim)
    DaemonContext::setInstance(&ctx_);
    std::cout << "[DaemonApp] Registered DaemonContext singleton" << std::endl;

    // Phase 1: Core infrastructure (no dependencies)
    std::cout << "[DaemonApp] Phase 1: Core infrastructure" << std::endl;

    // Logger first (no dependencies). Phase D.1 (Dinero Core 1.0):
    // file-logging is opt-in. Pass empty path here; the actual path
    // (if any) is populated below from `debug.log_file` after the
    // config-file loader runs.
    auto logger = std::make_shared<LoggerService>("");
    ctx_.logger = logger;
    services_.push_back(logger);

    // Config second (depends on logger)
    auto config = std::make_shared<ConfigService>();
    ctx_.config = config;
    services_.push_back(config);

    // Phase B (Dinero Core 1.0) — load config file BEFORE CLI flag parse so
    // that CLI flags can override file values (precedence: CLI > file >
    // defaults). Two-step:
    //   1. Pre-scan argv for --conf and --datadir to determine the config
    //      path. We don't Set() these here; the full CLI parse below will.
    //   2. LoadConfigFile() applies the file's keys via ConfigService::Set(),
    //      which handles flat→dotted normalization and multi-value-key
    //      append semantics.
    // After step 2, the existing CLI parse loop runs unchanged. Single-value
    // CLI flags overwrite file values; multi-value flags append (matching
    // Bitcoin Core's bitcoin.conf behavior).
    if (argc > 0 && argv != nullptr) {
        std::string cli_conf_path;
        std::string cli_datadir;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            // -conf=<path> or --conf=<path>
            if (arg.rfind("--conf=", 0) == 0) {
                cli_conf_path = arg.substr(7);
            } else if (arg.rfind("-conf=", 0) == 0) {
                cli_conf_path = arg.substr(6);
            } else if ((arg == "--conf" || arg == "-conf") && i + 1 < argc) {
                cli_conf_path = argv[i + 1];
            }
            // -datadir=<path> or --datadir=<path>
            else if (arg.rfind("--datadir=", 0) == 0) {
                cli_datadir = arg.substr(10);
            } else if (arg.rfind("-datadir=", 0) == 0) {
                cli_datadir = arg.substr(9);
            } else if ((arg == "--datadir" || arg == "-datadir") && i + 1 < argc) {
                cli_datadir = argv[i + 1];
            }
        }

        // If --datadir was given, seed wallet.datadir before computing the
        // default config path. Set() will be called again during the full
        // CLI parse below — single-value keys are last-wins, so this is safe.
        if (!cli_datadir.empty()) {
            config->Set("datadir", cli_datadir);
        }

        std::string conf_path = cli_conf_path.empty()
            ? config->DefaultConfigPath()
            : cli_conf_path;

        if (!config->LoadConfigFile(conf_path)) {
            std::cerr << "[DaemonApp] WARNING: I/O error reading config file "
                      << conf_path << "; proceeding with CLI flags + defaults"
                      << std::endl;
        }
    }

    // Parse command-line arguments and inject into ConfigService
    if (argc > 0 && argv != nullptr) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            // NOTE: --no-stratum flag deprecated - stratum is now separate binary
            // Use dinero-stratum binary instead
            if (arg == "--no-stratum" || arg == "--stratum") {
                std::cout << "[DaemonApp] Note: Stratum flags deprecated. Use separate dinero-stratum binary." << std::endl;
                continue;
            }

            // Pure boolean flags — never take a value, even space-separated
            if (arg == "-daemon" || arg == "--help" || arg == "-h" ||
                arg == "--version" || arg == "-v" || arg == "--testnet" ||
                arg == "--regtest" || arg == "--reindex" || arg == "--reindex-chainstate" ||
                arg == "--confirm" || arg == "--repair-db") {
                continue;
            }

            // Parse -key=value or --key=value format
            if ((arg.substr(0, 2) == "--" || arg.substr(0, 1) == "-") &&
                arg.find('=') != std::string::npos) {
                size_t eq_pos = arg.find('=');
                size_t key_start = (arg[1] == '-') ? 2 : 1;  // Skip - or --
                std::string key = arg.substr(key_start, eq_pos - key_start);
                std::string value = arg.substr(eq_pos + 1);

                // Append comma-separated for multi-value keys (e.g. multiple -addnode flags)
                if (key == "addnode" || key == "connect") {
                    std::string existing = config->GetString(key, "");
                    if (!existing.empty()) {
                        config->Set(key, existing + "," + value);
                    } else {
                        config->Set(key, value);
                    }
                } else {
                    config->Set(key, value);
                }
            }
            // Parse --key value format (space-separated)
            // Optional-value flags: --listen, --rpc, --server
            //   - --listen 0  -> listen=0
            //   - --listen    -> listen=1
            // Value-required flags: all others (e.g. --datadir /path)
            else if (arg.substr(0, 1) == "-") {
                size_t key_start = (arg.size() > 1 && arg[1] == '-') ? 2 : 1;
                std::string key = arg.substr(key_start);
                const bool optional_bool_flag =
                    (arg == "--rpc" || arg == "--listen" || arg == "--server" ||
                     arg == "-rpc" || arg == "-listen" || arg == "-server" ||
                     arg == "--rpc-readonly" || arg == "-rpc-readonly");

                if (i + 1 < argc) {
                    std::string next = argv[i + 1];
                    if (!next.empty() && next[0] != '-') {
                        // Append comma-separated for multi-value keys
                        if (key == "addnode" || key == "connect") {
                            std::string existing = config->GetString(key, "");
                            if (!existing.empty()) {
                                config->Set(key, existing + "," + next);
                            } else {
                                config->Set(key, next);
                            }
                        } else {
                            config->Set(key, next);
                        }
                        i++;  // Skip the value arg
                    } else if (optional_bool_flag) {
                        // Next arg is another flag — optional bool present without explicit value
                        config->Set(key, "1");
                    }
                } else if (optional_bool_flag) {
                    // Last arg — optional bool present without explicit value
                    config->Set(key, "1");
                }
            }
        }
    }

    // Phase D.1 (Dinero Core 1.0): wire `debug.log_file` from final
    // config (file + CLI overrides applied) into LoggerService BEFORE
    // app.Start() runs the LoggerService::Start() lifecycle hook. Empty
    // string ⇒ no file logging, log to stderr/journal only (the new 1.0
    // default). Operators wanting file logging set `debug.log_file =
    // <path>` in dinero.conf or pass `--debug.log_file=<path>`.
    logger->SetLogPath(config->GetString("debug.log_file", ""));

    auto ApplySyncProfilePolicy = [&]() {
        // Resolve sync profile. sync-profile is authoritative.
        // Without sync-profile, keep legacy utreexo-stateless compatibility.
        std::string sync_profile = config->GetString("sync-profile", "");
        std::transform(sync_profile.begin(), sync_profile.end(), sync_profile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const bool legacy_stateless = config->GetBool("utreexo-stateless", false);
        if (sync_profile.empty()) {
            sync_profile = legacy_stateless ? "ios_utreexo" : "mac_fullblock";
        }

        if (sync_profile == "ios_utreexo") {
            GetConfig().utreexo_stateless = true;
            GetConfig().allow_local_mining = false;
            GetConfig().allow_pool_mining = false;

            // Mobile defaults: compact sync footprint.
            if (config->GetInt("maxconnections", 0) <= 0) {
                config->Set("maxconnections", "4");
            }
            if (config->GetInt("dbcache", 0) <= 0) {
                config->Set("dbcache", "128");
            }

            // Hard-disable local mining in stateless/iOS profile.
            config->Set("gen", "0");
            config->Set("genproclimit", "0");
        } else if (sync_profile == "mac_fullblock") {
            GetConfig().utreexo_stateless = false;
            GetConfig().allow_local_mining = true;
            GetConfig().allow_pool_mining = true;

            // Desktop defaults: full sync resources.
            if (config->GetInt("maxconnections", 0) <= 0) {
                config->Set("maxconnections", "125");
            }
            if (config->GetInt("dbcache", 0) <= 0) {
                config->Set("dbcache", "450");
            }
        } else {
            std::cout << "[DaemonApp] WARNING: Unknown sync profile '" << sync_profile
                      << "', falling back to legacy utreexo-stateless="
                      << (legacy_stateless ? "1" : "0") << std::endl;
            GetConfig().utreexo_stateless = legacy_stateless;
            GetConfig().allow_local_mining = true;
            GetConfig().allow_pool_mining = true;
            sync_profile = GetConfig().utreexo_stateless ? "ios_utreexo" : "mac_fullblock";
        }

        GetConfig().sync_profile = sync_profile;
        GetConfig().utreexo_bridge = config->GetBool("utreexo-bridge", true);

        // Phase 3a of the shielded reorg invertibility plan
        // (docs/specs/atomic_consensus_persistence_phase3.md). Hidden
        // flag, default off. ConsensusWriteBatch::IsEnabled() reads
        // this. Phase 3a scaffold only — DEV / REGTEST USE ONLY.
        // Do NOT enable on the live fleet until phase 3b lands the
        // working-copy pattern + journal row + DisconnectTip
        // routing. With the flag on today, only ConnectTip's UTXO
        // mutations route through the batch; DisconnectTip and the
        // remaining containers stay on legacy persist paths.
        GetConfig().consensus_atomic_persist =
            config->GetBool("consensus.atomic_persist", false);
        if (GetConfig().consensus_atomic_persist) {
            std::cout << "[DaemonApp] consensus.atomic_persist=1 "
                      << "(PHASE 3A SCAFFOLD — DEV/REGTEST ONLY) — "
                      << "ConnectTip's UTXO map mutations route through "
                      << "ConsensusWriteBatch. DisconnectTip and other "
                      << "containers stay on legacy persist paths in 3a."
                      << std::endl;
        }

        const std::string request_type = GetConfig().utreexo_stateless ? "MSG_UTREEXO_BLOCK" : "MSG_BLOCK";
        const int configured_max_peers = config->GetInt("maxconnections", GetConfig().utreexo_stateless ? 4 : 125);
        std::cout << "[DaemonApp] Sync profile: " << GetConfig().sync_profile
                  << " | mode=" << (GetConfig().utreexo_stateless ? "STATELESS" : "STATEFUL")
                  << " | getdata=" << request_type
                  << " | maxconnections=" << configured_max_peers
                  << " | mining_local=" << (GetConfig().allow_local_mining ? "1" : "0")
                  << " | mining_pool=" << (GetConfig().allow_pool_mining ? "1" : "0")
                  << std::endl;

        if (GetConfig().utreexo_bridge) {
            std::cout << "[DaemonApp] Utreexo bridge mode ENABLED" << std::endl;
        }
    };

    ApplySyncProfilePolicy();

    auto ConfigurePoolAccountingRuntime = [&]() {
        const bool pool_requested = config->GetBool("pool.accounting.enable", false);

        bool pool_enabled = false;
        std::string disable_reason;
        std::shared_ptr<pool::PoolDB> pool_db;
        std::shared_ptr<pool::PoolManager> pool_manager;

        if (!pool_requested) {
            disable_reason =
                "Pool accounting disabled by default (set --pool.accounting.enable=1 to activate)";
        } else if (!GetConfig().allow_pool_mining) {
            disable_reason = "Pool accounting disabled by sync profile: " + GetConfig().sync_profile;
        } else {
            const std::filesystem::path pool_dir =
                std::filesystem::path(config->DataDir()) / "pool";
            const std::filesystem::path pool_db_path = pool_dir / "pool_accounting.sqlite";

            try {
                std::filesystem::create_directories(pool_dir);
                auto candidate = std::make_shared<pool::PoolDB>(pool_db_path.string());
                if (!candidate->initialize()) {
                    disable_reason = "Failed to initialize pool accounting DB: " + pool_db_path.string();
                } else {
                    auto manager_candidate = std::make_shared<pool::PoolManager>(pool_db_path.string());
                    if (!manager_candidate->initialize()) {
                        disable_reason = "Failed to initialize pool manager: " + pool_db_path.string();
                    } else {
                        pool_db = std::move(candidate);
                        pool_manager = std::move(manager_candidate);
                        pool_enabled = true;
                    }
                }
            } catch (const std::exception& e) {
                disable_reason = std::string("Pool accounting startup failed: ") + e.what();
            }
        }

        pool_manager_runtime_ = pool_manager;
        din::rpc::configurePoolRpc(pool_db, pool_manager, pool_enabled, disable_reason);
        if (pool_enabled) {
            std::cout << "[DaemonApp] Pool accounting: ENABLED" << std::endl;
        } else {
            std::cout << "[DaemonApp] Pool accounting: DISABLED (" << disable_reason << ")" << std::endl;
        }
    };

    ConfigurePoolAccountingRuntime();

    if (pool_manager_runtime_) {
        pool_manager_runtime_->setPaymentCallback([this](const std::string& address,
                                                         uint64_t amount,
                                                         std::string& txid_out) -> bool {
            if (amount == 0 || address.empty()) {
                g_logger.error("[Pool] payout callback rejected empty address or zero amount");
                return false;
            }

            if (!ctx_.wallet || !ctx_.tx_ingress) {
                g_logger.error("[Pool] payout callback unavailable: wallet or tx ingress not ready");
                return false;
            }

            auto wallet_service = std::dynamic_pointer_cast<WalletService>(ctx_.wallet);
            if (!wallet_service) {
                g_logger.error("[Pool] payout callback unavailable: wallet service cast failed");
                return false;
            }

            WalletManager& wallet_mgr = wallet_service->get();
            if (!wallet_mgr.hasActiveWallet()) {
                g_logger.error("[Pool] payout callback failed: no active wallet");
                return false;
            }
            if (wallet_mgr.isWalletLocked()) {
                g_logger.error("[Pool] payout callback failed: active wallet is locked");
                return false;
            }

            HDWallet* hd_wallet = wallet_mgr.getHDWallet();
            if (!hd_wallet) {
                g_logger.error("[Pool] payout callback failed: HD wallet is unavailable");
                return false;
            }

            std::vector<HDWallet::TxOutput> outputs;
            outputs.push_back(HDWallet::TxOutput{address, amount});

            std::string tx_hex;
            std::string create_error;
            constexpr uint64_t kPoolPayoutFeeRate = 2;
            if (!hd_wallet->CreateTransaction(outputs, kPoolPayoutFeeRate, tx_hex, create_error)) {
                g_logger.error("[Pool] payout callback failed to create transaction: " + create_error);
                return false;
            }

            Transaction payout_tx;
            if (!TransactionSerializer::Deserialize(payout_tx, tx_hex)) {
                g_logger.error("[Pool] payout callback failed to deserialize tx hex");
                return false;
            }

            auto submit = ctx_.tx_ingress->Submit(payout_tx, TxOrigin::WALLET);
            if (submit.rejected()) {
                g_logger.error("[Pool] payout callback rejected by mempool: " + submit.message);
                return false;
            }

            txid_out = payout_tx.GetTxid().AsUint256().GetHex();
            g_logger.info("[Pool] payout transaction submitted: " + txid_out.substr(0, 16) +
                          "... amount=" + std::to_string(amount));
            return true;
        });
    }

    // Phase 2: Data layer (depends on logger + config)
    std::cout << "[DaemonApp] Phase 2: Data layer" << std::endl;

    // ═══════════════════════════════════════════════════════════════════════════
    // ONE DB Definition: DaemonApp constructs ChainDB and ChainManager
    // ═══════════════════════════════════════════════════════════════════════════
    std::filesystem::path data_dir_path;
    std::filesystem::path chain_db_path;

    try {
        chain_db_ = std::make_unique<ChainDB>();

        // Get datadir (use same default as ConfigService: ~/.dinero)
        // Note: config isn't initialized yet, so we get the raw value or default
        std::string datadir_str = ctx_.config->GetString("datadir", "~/.dinero");

        // Expand tilde if present
        if (datadir_str.size() > 0 && datadir_str[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) {
                if (datadir_str.size() == 1) {
                    datadir_str = home;
                } else if (datadir_str[1] == '/') {
                    datadir_str = std::string(home) + datadir_str.substr(1);
                }
            }
        }

        data_dir_path = datadir_str;
        chain_db_path = data_dir_path / "blockchain" / "chaindb";

        std::filesystem::create_directories(chain_db_path.parent_path());

        const auto reindex_promotion_journal = ReindexPromotionJournalPath(data_dir_path);
        const bool had_interrupted_reindex_promotion =
            std::filesystem::exists(reindex_promotion_journal);
        std::string reindex_recovery_error;
        if (!RecoverInterruptedReindexPromotion(data_dir_path, &reindex_recovery_error)) {
            std::cerr << "[DaemonApp] ❌ Failed to recover interrupted reindex promotion: "
                      << reindex_recovery_error << std::endl;
            return false;
        }
        if (had_interrupted_reindex_promotion) {
            std::cout << "[DaemonApp] ✅ Recovered interrupted reindex promotion before ChainDB init" << std::endl;
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // Phase E.2.b: Disk Space Check
        // ═══════════════════════════════════════════════════════════════════════════
        // CRITICAL: Check disk space before initializing ChainDB
        // This prevents starting with insufficient disk space
        {
            std::cout << "\n";
            std::cout << "════════════════════════════════════════════════════════════════\n";
            std::cout << "💾 DISK SPACE CHECK (Phase E.2.b)\n";
            std::cout << "════════════════════════════════════════════════════════════════\n";

            storage::DiskSpaceMonitor disk_monitor(data_dir_path);
            auto disk_info = disk_monitor.checkDiskSpace();

            // Show disk usage report
            std::cout << disk_monitor.getDiskUsageReport();

            // FATAL: Refuse to start if disk is full
            if (disk_info.status == storage::DiskSpaceStatus::FULL) {
                std::cerr << "\n❌ FATAL: Insufficient disk space to start node\n";
                std::cerr << "   Available: " << (disk_info.available_bytes / (1024.0 * 1024.0 * 1024.0)) << " GB\n";
                std::cerr << "   Minimum required: 1.0 GB\n";
                std::cerr << "\n   Free up disk space and try again.\n\n";
                return false;
            }

            // CRITICAL: Warn if disk space is low
            if (disk_info.status == storage::DiskSpaceStatus::CRITICAL) {
                std::cerr << "\n⚠️  WARNING: Disk space CRITICAL\n";
                std::cerr << "   Node may stop accepting blocks soon.\n";
                std::cerr << "   Consider freeing up disk space or enabling pruning.\n\n";
            } else if (disk_info.status == storage::DiskSpaceStatus::LOW) {
                std::cerr << "\n⚠️  WARNING: Disk space LOW\n";
                std::cerr << "   Monitor disk usage closely.\n\n";
            } else {
                std::cout << "\n✅ Disk space check passed. Continuing startup...\n\n";
            }
        }

        auto status = chain_db_->init(chain_db_path);
        if (status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to initialize ChainDB: " << StatusToString(status) << std::endl;
            return false;
        }
        std::cout << "[DaemonApp] ✅ ChainDB constructed and initialized (ONE DB)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[DaemonApp] ❌ Exception constructing ChainDB: " << e.what() << std::endl;
        return false;
    }

    // ChainDB is owned by DaemonApp; services use non-owning pointers.
    ChainDB* chain_db_ptr = chain_db_.get();
    if (pool_manager_runtime_) {
        pool_manager_runtime_->setChainDB(chain_db_ptr);
    }

    // ─────────────────────────────────────────────────────────────────
    // Offline `--rebuild-undo-range=A:B` orchestrator (commit #5)
    // ─────────────────────────────────────────────────────────────────
    // Defaults to dry-run (preflight only); `--rebuild-undo-write` is
    // required to enable verified LIVE undo writes. Daemon exits
    // cleanly after the run — normal services are NEVER started.
    // The RocksDB exclusive lock on chain_db_ provides the "refuse if
    // another daemon is active on this datadir" guarantee: this lambda
    // runs inside the same exclusive-open window as `--reindex`.
    auto parse_height_range = [](const std::string& range,
                                 uint32_t& window_start,
                                 uint32_t& window_end,
                                 const char* flag_name) -> bool {
        const auto colon = range.find(':');
        if (colon == std::string::npos) {
            std::cerr << "[DaemonApp] ❌ " << flag_name << " expects A:B" << std::endl;
            return false;
        }
        try {
            window_start = static_cast<uint32_t>(std::stoul(range.substr(0, colon)));
            window_end = static_cast<uint32_t>(std::stoul(range.substr(colon + 1)));
        } catch (const std::exception&) {
            std::cerr << "[DaemonApp] ❌ " << flag_name << ": malformed numbers" << std::endl;
            return false;
        }
        return true;
    };

    auto run_header_metadata_recovery_if_requested = [&](bool& should_exit_after) -> bool {
        should_exit_after = false;
        std::optional<std::string> range_arg;
        bool write_flag = false;
        std::filesystem::path manifest_override;

        if (argc > 0 && argv != nullptr) {
            for (int i = 1; i < argc; i++) {
                const std::string arg = argv[i];
                const std::string range_prefix = "--recover-header-metadata-range=";
                const std::string manifest_prefix = "--recover-header-metadata-manifest=";
                if (arg.rfind(range_prefix, 0) == 0) {
                    range_arg = arg.substr(range_prefix.size());
                } else if (arg == "--recover-header-metadata-write") {
                    write_flag = true;
                } else if (arg.rfind(manifest_prefix, 0) == 0) {
                    manifest_override = arg.substr(manifest_prefix.size());
                }
            }
        }
        if (!range_arg.has_value()) {
            return true;
        }
        should_exit_after = true;

        uint32_t window_start = 0;
        uint32_t window_end = 0;
        if (!parse_height_range(*range_arg, window_start, window_end,
                                "--recover-header-metadata-range")) {
            return false;
        }

        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "🧭 OFFLINE HEADER-METADATA RECOVERY REQUESTED\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "Window: [" << window_start << ", " << window_end << "]\n";
        std::cout << "Mode:   " << (write_flag ? "LIVE WRITES (--recover-header-metadata-write)"
                                               : "DRY RUN (default)") << "\n";
        if (!write_flag) {
            std::cout << "        Pass --recover-header-metadata-write to reconstruct missing rows.\n";
        }
        std::cout << "Manifest: " << (manifest_override.empty()
            ? (data_dir_path / "recover_header_metadata_manifest.json").string()
            : manifest_override.string()) << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n\n";

        auto recovery_block_storage = std::make_unique<BlockStorage>();
        const auto bs_status = recovery_block_storage->init(data_dir_path);
        if (bs_status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to init BlockStorage for header-metadata recovery: "
                      << StatusToString(bs_status) << std::endl;
            return false;
        }

        daemon::HeaderMetadataRecoveryOptions opts;
        ChainWriteToken recovery_write_token;
        opts.datadir = data_dir_path;
        opts.window_start = window_start;
        opts.window_end = window_end;
        opts.write = write_flag;
        opts.manifest_path_override = manifest_override.empty()
            ? (data_dir_path / "recover_header_metadata_manifest.json")
            : manifest_override;
        opts.live_chain_db = chain_db_ptr;
        opts.live_block_storage = recovery_block_storage.get();
        if (write_flag) {
            opts.write_token = &recovery_write_token;
        }

        const auto result = daemon::RecoverMissingHeaderMetadataRange(opts);
        if (!result.ok()) {
            std::cerr << "[DaemonApp] ❌ header-metadata recovery returned status: "
                      << StatusToString(result.status()) << std::endl;
            return false;
        }
        const auto& m = result.value();
        std::cout << "[DaemonApp] ✅ header-metadata recovery complete: final_status="
                  << m.final_status
                  << " scanned=" << m.scanned
                  << " already_ok=" << m.already_ok
                  << " recoverable=" << m.recoverable
                  << " recovered=" << m.recovered
                  << " failed=" << m.failed << std::endl;
        return m.final_status != "failed" && m.final_status != "scan_failed" &&
               m.final_status != "write_failed";
    };

    bool exit_after_header_metadata_recovery = false;
    if (!run_header_metadata_recovery_if_requested(exit_after_header_metadata_recovery)) {
        return false;
    }
    if (exit_after_header_metadata_recovery) {
        std::cout << "[DaemonApp] header-metadata recovery finished; exiting before service init."
                  << " Restart the daemon normally, or run undo rebuild next.\n";
        return false;
    }

    auto run_undo_rebuild_if_requested = [&](bool& should_exit_after) -> bool {
        should_exit_after = false;
        std::optional<std::string> range_arg;
        bool write_flag = false;
        std::optional<uint32_t> anchor_height;
        std::filesystem::path manifest_override;
        if (argc > 0 && argv != nullptr) {
            for (int i = 1; i < argc; i++) {
                std::string arg = argv[i];
                const std::string range_prefix = "--rebuild-undo-range=";
                const std::string anchor_prefix = "--rebuild-undo-anchor-height=";
                const std::string manifest_prefix = "--rebuild-undo-manifest=";
                if (arg.rfind(range_prefix, 0) == 0) {
                    range_arg = arg.substr(range_prefix.size());
                } else if (arg == "--rebuild-undo-write") {
                    write_flag = true;
                } else if (arg.rfind(anchor_prefix, 0) == 0) {
                    try {
                        anchor_height = static_cast<uint32_t>(
                            std::stoul(arg.substr(anchor_prefix.size())));
                    } catch (const std::exception&) {
                        std::cerr << "[DaemonApp] ❌ malformed "
                                  << anchor_prefix << " argument" << std::endl;
                        return false;
                    }
                } else if (arg.rfind(manifest_prefix, 0) == 0) {
                    manifest_override = arg.substr(manifest_prefix.size());
                }
            }
        }
        if (!range_arg.has_value()) {
            return true;  // flag not requested — normal startup continues
        }
        should_exit_after = true;

        uint32_t window_start = 0, window_end = 0;
        if (!parse_height_range(*range_arg, window_start, window_end,
                                "--rebuild-undo-range")) {
            return false;
        }

        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "🔧 OFFLINE UNDO-REBUILD REQUESTED\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "Window: [" << window_start << ", " << window_end << "]\n";
        std::cout << "Anchor: ";
        if (anchor_height.has_value()) {
            std::cout << "height=" << *anchor_height << " (caller-provided)\n";
        } else {
            std::cout << "GENESIS (full replay into temp DB)\n";
        }
        std::cout << "Mode:   " << (write_flag ? "LIVE WRITES (--rebuild-undo-write)" : "DRY RUN (default)") << "\n";
        if (!write_flag) {
            std::cout << "        Pass --rebuild-undo-write to enable verified LIVE undo writes.\n";
        }
        std::cout << "Manifest: " << (manifest_override.empty()
            ? (data_dir_path / "rebuild_undo_manifest.json").string()
            : manifest_override.string()) << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n\n";

        auto rebuild_block_storage = std::make_unique<BlockStorage>();
        const auto bs_status = rebuild_block_storage->init(data_dir_path);
        if (bs_status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to init BlockStorage for undo-rebuild: "
                      << StatusToString(bs_status) << std::endl;
            return false;
        }

        daemon::UndoRebuildOptions opts;
        opts.datadir = data_dir_path;
        opts.window_start = window_start;
        opts.window_end = window_end;
        opts.anchor.height = anchor_height.value_or(0);
        opts.live_chain_db = chain_db_ptr;
        opts.live_block_storage = rebuild_block_storage.get();
        opts.dry_run = !write_flag;
        opts.manifest_path_override = manifest_override;

        const auto result = daemon::RunOfflineUndoRebuild(opts);
        if (!result.ok()) {
            std::cerr << "[DaemonApp] ❌ undo-rebuild returned status: "
                      << StatusToString(result.status())
                      << " — see manifest for per-height detail" << std::endl;
            return false;
        }
        const auto& m = result.value();
        std::cout << "[DaemonApp] ✅ undo-rebuild complete: final_status="
                  << m.final_status
                  << " rebuilt=" << m.rebuilt_count
                  << " verify_failed=" << m.verify_failed_count
                  << " already_ok=" << m.already_ok_count
                  << " holes_remaining=" << m.holes_count
                  << " missing_metadata=" << m.missing_metadata_count
                  << " blocked=" << m.blocked_count << std::endl;
        return true;
    };

    bool exit_after_undo_rebuild = false;
    if (!run_undo_rebuild_if_requested(exit_after_undo_rebuild)) {
        return false;
    }
    if (exit_after_undo_rebuild) {
        // Refuse to bring up normal services after an undo-rebuild run.
        // The chain_db_ has either been written-to in a verified
        // surgical way (live writes mode) OR not at all (dry run); either
        // way the operator should restart the daemon normally to bring
        // the node back up.
        std::cout << "[DaemonApp] undo-rebuild finished; exiting before service init."
                  << " Restart the daemon normally to resume operation.\n";
        return false;
    }

    auto run_reindex_if_requested = [&]() -> bool {
        bool do_reindex = false;
        bool do_reindex_chainstate = false;

        if (argc > 0 && argv != nullptr) {
            for (int i = 1; i < argc; i++) {
                std::string arg = argv[i];
                if (arg == "--reindex") {
                    do_reindex = true;
                } else if (arg == "--reindex-chainstate") {
                    do_reindex_chainstate = true;
                }
            }
        }

        std::string recovery_marker_error;
        const auto recovery_marker =
            daemon::ReadChainstateRecoveryMarker(data_dir_path, &recovery_marker_error);

        if (!do_reindex && !do_reindex_chainstate && recovery_marker.has_value()) {
            std::cout << "\n";
            std::cout << "════════════════════════════════════════════════════════════════\n";
            std::cout << "🛟 CHAINSTATE RECOVERY MARKER DETECTED\n";
            std::cout << "════════════════════════════════════════════════════════════════\n";
            std::cout << "Reason: " << recovery_marker->reason << "\n";
            std::cout << "Detected at unix time: " << recovery_marker->timestamp << "\n";
            if (daemon::kAutomaticChainstateRecoveryArmed) {
                do_reindex_chainstate = true;
                std::cout << "Action: running built-in --reindex-chainstate recovery\n";
            } else {
                std::cout << "Action: automatic replay is DISABLED by safety fuse; "
                             "start normally or run manual recovery explicitly\n";
            }
            std::cout << "════════════════════════════════════════════════════════════════\n";
            std::cout << "\n";
        } else if (!recovery_marker_error.empty()) {
            std::cerr << "[DaemonApp] ⚠️  Failed to read chainstate recovery marker: "
                      << recovery_marker_error << std::endl;
        }

        if (!do_reindex && !do_reindex_chainstate) {
            return true;
        }

        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "🔄 REINDEX OPERATION REQUESTED\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "Mode: " << (do_reindex ? "Full reindex (--reindex)" : "Chainstate only (--reindex-chainstate)") << "\n";
        std::cout << "\n";

        std::unique_ptr<BlockStorage> block_storage = std::make_unique<BlockStorage>();
        auto storage_status = block_storage->init(data_dir_path);
        if (storage_status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to initialize BlockStorage for reindex: "
                      << StatusToString(storage_status) << std::endl;
            return false;
        }

        std::filesystem::path temp_chain_db_path = chain_db_path;
        temp_chain_db_path += ".reindex.tmp";
        const std::filesystem::path shielded_frontier_path =
            chain_db_path.parent_path() / "shielded_frontier.bin";
        std::filesystem::path temp_shielded_frontier_path = shielded_frontier_path;
        temp_shielded_frontier_path += ".reindex.tmp";
        const std::filesystem::path shielded_nullifier_db_path =
            chain_db_path.parent_path() / "shielded_nullifiers.db";
        std::filesystem::path temp_shielded_nullifier_db_path = shielded_nullifier_db_path;
        temp_shielded_nullifier_db_path += ".reindex.tmp";

        const auto ts = std::to_string(static_cast<long long>(std::time(nullptr)));
        std::filesystem::path backup_chain_db_path = chain_db_path;
        backup_chain_db_path += ".pre-reindex-" + ts;
        std::filesystem::path backup_shielded_frontier_path = shielded_frontier_path;
        backup_shielded_frontier_path += ".pre-reindex-" + ts;
        std::filesystem::path backup_shielded_nullifier_db_path = shielded_nullifier_db_path;
        backup_shielded_nullifier_db_path += ".pre-reindex-" + ts;

        std::error_code fs_error;
        std::filesystem::remove_all(temp_chain_db_path, fs_error);
        fs_error.clear();
        std::filesystem::remove_all(temp_shielded_frontier_path, fs_error);
        fs_error.clear();
        std::filesystem::remove_all(temp_shielded_nullifier_db_path, fs_error);
        fs_error.clear();
        std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-wal", fs_error);
        fs_error.clear();
        std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-shm", fs_error);
        fs_error.clear();

        auto rebuilt_chain_db = std::make_unique<ChainDB>();
        auto rebuilt_status = rebuilt_chain_db->init(temp_chain_db_path);
        if (rebuilt_status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to initialize temporary ChainDB for reindex: "
                      << StatusToString(rebuilt_status) << std::endl;
            return false;
        }



        consensus::BlockReindexer::Config reindex_config;
        reindex_config.mode = do_reindex
            ? consensus::BlockReindexer::Mode::FULL
            : consensus::BlockReindexer::Mode::CHAINSTATE_ONLY;
        reindex_config.use_assumevalid = true;
        reindex_config.progress_interval = 1000;
        reindex_config.shielded_frontier_output_path = temp_shielded_frontier_path;
        reindex_config.shielded_nullifier_db_path = temp_shielded_nullifier_db_path;

        consensus::BlockReindexer reindexer(data_dir_path, rebuilt_chain_db.get(), block_storage.get(), reindex_config);
        auto reindex_result = reindexer.execute();
        if (!reindex_result.ok()) {
            std::cerr << "[DaemonApp] ❌ Reindex failed with status: "
                      << StatusToString(reindex_result.status()) << std::endl;
            rebuilt_chain_db->close();
            std::filesystem::remove_all(temp_chain_db_path, fs_error);
            fs_error.clear();
            std::filesystem::remove_all(temp_shielded_frontier_path, fs_error);
            fs_error.clear();
            std::filesystem::remove_all(temp_shielded_nullifier_db_path, fs_error);
            fs_error.clear();
            std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-wal", fs_error);
            fs_error.clear();
            std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-shm", fs_error);
            return false;
        }

        const auto& stats = reindex_result.value();
        if (!stats.success) {
            std::cerr << "[DaemonApp] ❌ Reindex failed: " << stats.error << std::endl;
            rebuilt_chain_db->close();
            std::filesystem::remove_all(temp_chain_db_path, fs_error);
            fs_error.clear();
            std::filesystem::remove_all(temp_shielded_frontier_path, fs_error);
            fs_error.clear();
            std::filesystem::remove_all(temp_shielded_nullifier_db_path, fs_error);
            fs_error.clear();
            std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-wal", fs_error);
            fs_error.clear();
            std::filesystem::remove(temp_shielded_nullifier_db_path.string() + "-shm", fs_error);
            return false;
        }

        rebuilt_chain_db->close();
        chain_db_->close();

        struct ReindexPromotion {
            std::filesystem::path live_path;
            std::filesystem::path temp_path;
            std::filesystem::path backup_path;
            bool had_existing = false;
            bool sqlite_sidecars = false;
            std::string label;
        };

        std::vector<ReindexPromotion> promotions;
        std::vector<ReindexPromotionJournalEntry> promotion_journal_entries;
        auto rollback_promotions = [&](const std::vector<ReindexPromotion>& completed) {
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                std::filesystem::remove_all(it->live_path, fs_error);
                fs_error.clear();
                if (it->sqlite_sidecars) {
                    std::filesystem::remove(it->live_path.string() + "-wal", fs_error);
                    fs_error.clear();
                    std::filesystem::remove(it->live_path.string() + "-shm", fs_error);
                    fs_error.clear();
                }
                if (it->had_existing) {
                    std::filesystem::rename(it->backup_path, it->live_path, fs_error);
                    if (fs_error) {
                        std::cerr << "[DaemonApp] ❌ Failed to restore " << it->label
                                  << " after reindex promotion failure: " << fs_error.message() << std::endl;
                        fs_error.clear();
                    }
                    if (it->sqlite_sidecars) {
                        std::filesystem::rename(it->backup_path.string() + "-wal",
                                                it->live_path.string() + "-wal", fs_error);
                        fs_error.clear();
                        std::filesystem::rename(it->backup_path.string() + "-shm",
                                                it->live_path.string() + "-shm", fs_error);
                        fs_error.clear();
                    }
                }
            }
        };

        auto promote_reindex_path = [&](const std::filesystem::path& live_path,
                                        const std::filesystem::path& temp_path,
                                        const std::filesystem::path& backup_path,
                                        const std::string& label,
                                        bool sqlite_sidecars = false) -> bool {
            if (!std::filesystem::exists(temp_path)) {
                return true;
            }

            ReindexPromotion promotion;
            promotion.live_path = live_path;
            promotion.temp_path = temp_path;
            promotion.backup_path = backup_path;
            promotion.had_existing = std::filesystem::exists(live_path);
            promotion.sqlite_sidecars = sqlite_sidecars;
            promotion.label = label;

            if (promotion.had_existing) {
                std::filesystem::rename(live_path, backup_path, fs_error);
                if (fs_error) {
                    std::cerr << "[DaemonApp] ❌ Failed to move existing " << label
                              << " aside before swap: " << fs_error.message() << std::endl;
                    fs_error.clear();
                    return false;
                }
                if (sqlite_sidecars) {
                    if (!RenameSqliteSidecarsIfPresent(live_path, backup_path, fs_error)) {
                        std::cerr << "[DaemonApp] ❌ Failed to move existing " << label
                                  << " SQLite sidecars aside before swap: " << fs_error.message() << std::endl;
                        fs_error.clear();
                        return false;
                    }
                }
            }

            dinero::testing::MaybeAbortAt("after_reindex_backup_before_promote",
                                          dinero::Params().network_id == "regtest");

            std::filesystem::rename(temp_path, live_path, fs_error);
            if (fs_error) {
                std::cerr << "[DaemonApp] ❌ Failed to promote rebuilt " << label
                          << " into place: " << fs_error.message() << std::endl;
                fs_error.clear();
                if (promotion.had_existing) {
                    std::filesystem::rename(backup_path, live_path, fs_error);
                    fs_error.clear();
                    if (sqlite_sidecars) {
                        RenameSqliteSidecarsIfPresent(backup_path, live_path, fs_error);
                        fs_error.clear();
                    }
                }
                return false;
            }

            if (sqlite_sidecars &&
                !RenameSqliteSidecarsIfPresent(temp_path, live_path, fs_error)) {
                std::cerr << "[DaemonApp] ❌ Failed to promote rebuilt " << label
                          << " SQLite sidecars into place: " << fs_error.message() << std::endl;
                fs_error.clear();
                return false;
            }

            promotions.push_back(std::move(promotion));
            return true;
        };

        if (std::filesystem::exists(temp_chain_db_path) ||
            std::filesystem::exists(temp_shielded_frontier_path) ||
            std::filesystem::exists(temp_shielded_nullifier_db_path)) {
            promotion_journal_entries.push_back(
                {"ChainDB", chain_db_path, temp_chain_db_path, backup_chain_db_path, false});
            promotion_journal_entries.push_back(
                {"shielded frontier", shielded_frontier_path, temp_shielded_frontier_path,
                 backup_shielded_frontier_path, false});
            promotion_journal_entries.push_back(
                {"shielded nullifier DB", shielded_nullifier_db_path, temp_shielded_nullifier_db_path,
                 backup_shielded_nullifier_db_path, true});

            std::string journal_error;
            if (!WriteReindexPromotionJournal(ReindexPromotionJournalPath(data_dir_path),
                                             promotion_journal_entries,
                                             &journal_error)) {
                std::cerr << "[DaemonApp] ❌ Failed to write reindex promotion journal: "
                          << journal_error << std::endl;
                return false;
            }
        }

        if (!promote_reindex_path(chain_db_path, temp_chain_db_path, backup_chain_db_path, "ChainDB")) {
            rollback_promotions(promotions);
            return false;
        }
        if (!promote_reindex_path(shielded_frontier_path, temp_shielded_frontier_path,
                                  backup_shielded_frontier_path, "shielded frontier")) {
            rollback_promotions(promotions);
            return false;
        }
        if (!promote_reindex_path(shielded_nullifier_db_path, temp_shielded_nullifier_db_path,
                                  backup_shielded_nullifier_db_path, "shielded nullifier DB", true)) {
            rollback_promotions(promotions);
            return false;
        }

        auto reopen_status = chain_db_->init(chain_db_path);
        if (reopen_status != Status::Ok) {
            std::cerr << "[DaemonApp] ❌ Failed to reopen rebuilt ChainDB: "
                      << StatusToString(reopen_status) << std::endl;
            chain_db_->close();
            rollback_promotions(promotions);
            auto restore_status = chain_db_->init(chain_db_path);
            if (restore_status != Status::Ok) {
                std::cerr << "[DaemonApp] ❌ Failed to reopen restored ChainDB: "
                          << StatusToString(restore_status) << std::endl;
            }
            return false;
        }

        std::filesystem::remove(ReindexPromotionJournalPath(data_dir_path), fs_error);
        fs_error.clear();



        chain_db_ptr = chain_db_.get();
        if (pool_manager_runtime_) {
            pool_manager_runtime_->setChainDB(chain_db_ptr);
        }

        if (recovery_marker.has_value()) {
            std::string clear_error;
            if (!daemon::ClearChainstateRecoveryMarker(data_dir_path, &clear_error)) {
                std::cerr << "[DaemonApp] ⚠️  Reindex succeeded, but failed to clear chainstate recovery marker: "
                          << clear_error << std::endl;
            } else {
                std::cout << "[DaemonApp] ✅ Cleared chainstate recovery marker after successful rebuild\n";
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // Post-reindex UTXOIndex hygiene
        // ═══════════════════════════════════════════════════════════════════════════
        // The reindex rebuilds canonical ChainDB + Utreexo + shielded state,
        // but the UTXOIndex (blockchain/utxo)
        // is untouched. If a `reorg_in_progress` marker was set before the
        // reindex (by ActivateBestChain starting a reorg that was interrupted,
        // which is exactly what triggers an automatic reindex in the first
        // place), that marker persists into the freshly-rebuilt chainstate.
        //
        // ChainstateService::Init then reads the stale marker and fires its
        // consistency check even though the recovered chainstate is already
        // canonical again.
        //
        // Since the reindex has produced a complete fresh chainstate from the
        // archival block files, any prior "reorg in progress" state is stale
        // and should be cleared.
        //
        // See also: include/daemon/chainstate_recovery_marker.h (Apr 18 2026
        // comment documenting this failure mode).
        // ═══════════════════════════════════════════════════════════════════════════
        {
            const std::filesystem::path utxo_index_path =
                chain_db_path.parent_path() / "utxo";
            if (std::filesystem::exists(utxo_index_path)) {
                try {
                    UTXOIndex post_reindex_index(utxo_index_path.string());
                    if (post_reindex_index.Initialize()) {
                        if (post_reindex_index.GetMetadata("reorg_in_progress").has_value()) {
                            if (post_reindex_index.DeleteMetadata("reorg_in_progress")) {
                                std::cout << "[DaemonApp] ✅ Cleared stale reorg_in_progress marker from UTXOIndex after reindex\n";
                            } else {
                                std::cerr << "[DaemonApp] ⚠️  Failed to clear reorg_in_progress marker from UTXOIndex post-reindex\n";
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[DaemonApp] ⚠️  Unable to access UTXOIndex post-reindex: "
                              << e.what() << std::endl;
                }
            }
        }

        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "✅ REINDEX COMPLETE\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "Blocks processed: " << stats.blocks_processed << "\n";
        std::cout << "Files scanned: " << stats.files_scanned << "\n";
        std::cout << "UTXOs created: " << stats.utxos_created << "\n";
        std::cout << "UTXOs spent: " << stats.utxos_spent << "\n";
        std::cout << "Total bytes: " << stats.total_bytes << "\n";
        std::cout << "Duration: " << (stats.duration_ms / 1000.0) << " seconds\n";
        if (std::filesystem::exists(backup_chain_db_path)) {
            std::cout << "Previous ChainDB backup: " << backup_chain_db_path.string() << "\n";
        }
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "\n";
        return true;
    };

    if (!run_reindex_if_requested()) {
        return false;
    }

    // Temporary archival reader used during startup backfills before the
    // long-lived runtime BlockStorage is initialized.
    std::unique_ptr<BlockStorage> startup_block_storage;

    {
        std::string startup_datadir_str = ctx_.config->GetString("datadir", "~/.dinero");
        if (!startup_datadir_str.empty() && startup_datadir_str[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) {
                if (startup_datadir_str.size() == 1) startup_datadir_str = home;
                else if (startup_datadir_str[1] == '/') startup_datadir_str = std::string(home) + startup_datadir_str.substr(1);
            }
        }
        std::filesystem::path startup_datadir = startup_datadir_str;
        startup_block_storage = std::make_unique<BlockStorage>();
        auto startup_storage_status = startup_block_storage->init(startup_datadir);
        if (startup_storage_status != Status::Ok) {
            std::cerr << "[DaemonApp] ⚠️ Failed to initialize startup BlockStorage reader: "
                      << StatusToString(startup_storage_status) << std::endl;
            startup_block_storage.reset();
        }
    }


    // ═══════════════════════════════════════════════════════════════════════════
    // Phase E.1.a: Startup Consistency Validation
    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: Validate chain state consistency before accepting blocks
    // This catches corruption from power loss, disk errors, or torn writes
    //
    // TODO: Re-enable after fixing startup_validator.cpp API mismatches
    // (Temporarily disabled to allow Phase E.2.b testing)
    /*
    {
        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";
        std::cout << "🔍 STARTUP CONSISTENCY VALIDATION (Phase E.1)\n";
        std::cout << "════════════════════════════════════════════════════════════════\n";

        consensus::StartupValidator validator(chain_db_ptr);
        auto status = validator.Validate();

        // Show validation report
        std::cout << validator.GetValidationReport();

        // Handle validation result
        if (status.result == consensus::StartupValidationResult::FATAL) {
            std::cerr << "\n❌ FATAL ERROR: " << status.message << "\n";
            std::cerr << "   " << status.guidance << "\n\n";
            return false;  // chain_db_ cleaned up by unique_ptr
        }

        if (status.result == consensus::StartupValidationResult::NEEDS_REINDEX) {
            // Check if --reindex flag was passed
            bool reindex_requested = false;
            if (argc > 0 && argv != nullptr) {
                for (int i = 1; i < argc; i++) {
                    std::string arg = argv[i];
                    if (arg == "--reindex" || arg == "--reindex-chainstate") {
                        reindex_requested = true;
                        break;
                    }
                }
            }

            if (!reindex_requested) {
                std::cerr << "\n⚠️  VALIDATION FAILED: " << status.message << "\n";
                std::cerr << "   " << status.guidance << "\n";
                std::cerr << "\n   Run daemon with --reindex to rebuild chain state.\n\n";
                return false;  // chain_db_ cleaned up by unique_ptr
            }

            // Reindex was requested, will proceed to reindex below
            std::cout << "\n✅ Validation detected corruption, but --reindex was passed.\n";
            std::cout << "   Continuing to reindex operation...\n\n";
        }

        if (status.result == consensus::StartupValidationResult::RECOVERED) {
            std::cout << "\n🔄 RECOVERED: " << status.message << "\n";
            std::cout << "   " << status.guidance << "\n";
            std::cout << "   Node recovered automatically, continuing startup.\n\n";
        }

        if (status.result == consensus::StartupValidationResult::OK) {
            std::cout << "\n✅ All consistency checks passed. Continuing startup...\n\n";
        }
    }
    */

    // Initialize genesis block if chain is empty
    try {
        auto* db = chain_db_ptr;  // Phase 39: Direct access (ChainManager deleted)
        auto tip_result = db->getTip();

        // Check if chain is empty (no tip or tip is all zeros)
        bool chain_is_empty = (tip_result.status() != Status::Ok) ||
                             (tip_result.value().height == 0 &&
                              tip_result.value().hash == uint256::FromHexUnsafe(std::string(64, '0')));

        if (chain_is_empty) {
            std::cout << "[DaemonApp] Chain is empty, initializing genesis block..." << std::endl;

            // Call genesis initialization (creates genesis block at height 0)
            bool genesis_ok = InitializeGenesis(db, startup_block_storage.get(), nullptr);

            if (!genesis_ok) {
                std::cerr << "[DaemonApp] ❌ Failed to initialize genesis block" << std::endl;
                return false;
            }

            std::cout << "[DaemonApp] ✅ Genesis block initialized" << std::endl;

            // CRITICAL: After genesis init, manually create BlockIndex for genesis
            // WORKAROUND: getTip() has issues reading immediately after setTip()
            // So we directly create the BlockIndex from the known genesis params
            const auto& params = Params();
            uint256 genesis_hash = uint256::FromHexUnsafe(params.genesis.genesisHashHex);

            // Phase 41: Create genesis BlockIndex and activate
            dinero::BlockHeader genesis_header;
            genesis_header.version = params.genesis.nVersion;
            genesis_header.prev_block_hash = uint256();  // Zero hash (no parent)
            genesis_header.merkle_root = uint256::FromHexUnsafe(params.genesis.merkleRootHex);
            genesis_header.timestamp = params.genesis.nTime;
            genesis_header.difficulty = params.genesis.nBits;
            genesis_header.nonce = params.genesis.nNonce;

            // Add genesis to BlockIndex graph
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_.chainstate);
            if (chainstate) {
                auto* genesis_index = chainstate->AddBlockIndex(genesis_header, 0);
                if (genesis_index) {
                    // Mark genesis as fully validated
                    genesis_index->status = dinero::BLOCK_VALID_SCRIPTS |
                                           dinero::BLOCK_VALID_TRANSACTIONS |
                                           dinero::BLOCK_VALID_CHAIN;

                    // Add as candidate tip
                    chainstate->AddCandidate(genesis_index);

                    // Activate genesis block
                    chainstate->ActivateBestChain();

                    std::cout << "[DaemonApp] ✅ Genesis BlockIndex created and activated" << std::endl;
                } else {
                    std::cout << "[DaemonApp] ⚠️  Failed to create genesis BlockIndex" << std::endl;
                }
            } else {
                std::cout << "[DaemonApp] ✅ Genesis block inserted (BlockIndex creation skipped)" << std::endl;
            }
        } else {
            std::cout << "[DaemonApp] ✅ Chain already initialized (height="
                     << tip_result.value().height << ")" << std::endl;

            // Phase 41: Rebuild BlockIndex graph from ChainDB
            // Walk the chain from genesis to tip, creating BlockIndex entries
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_.chainstate);
            if (chainstate) {
                uint32_t current_height = tip_result.value().height;
                std::cout << "[DaemonApp] Rebuilding BlockIndex graph from height 0 to " << current_height << "..." << std::endl;

                // Load blocks from ChainDB and create BlockIndex entries
                for (uint32_t h = 0; h <= current_height; ++h) {
                    // Get block hash by height
                    auto hash_result = chain_db_ptr->getBlockHashByHeight(h);
                    if (hash_result.status() != dinero::Status::Ok) {
                        continue; // Skip missing blocks
                    }

                    // Get block by hash
                    auto block_result = storage::ReadArchivalBlock(
                        *chain_db_ptr,
                        startup_block_storage.get(),
                        hash_result.value());
                    if (block_result.status() == dinero::Status::Ok) {
                        const auto& block = block_result.value();

                        // Create header from block
                        dinero::BlockHeader header;
                        header.version = block.header.version;
                        header.prev_block_hash = block.header.prev_block_hash;
                        header.merkle_root = block.header.merkle_root;
                        header.timestamp = block.header.timestamp;
                        header.difficulty = block.header.difficulty;
                        header.nonce = block.header.nonce;

                        // Add to BlockIndex
                        auto* block_index = chainstate->AddBlockIndex(header, h);
                        if (block_index) {
                            block_index->status = dinero::BLOCK_VALID_SCRIPTS |
                                                 dinero::BLOCK_VALID_TRANSACTIONS |
                                                 dinero::BLOCK_VALID_CHAIN;
                        }
                    }

                    // Progress indicator every 1000 blocks
                    if (h % 1000 == 0 && h > 0) {
                        std::cout << "  ...loaded " << h << " blocks" << std::endl;
                    }
                }

                // Set the tip as candidate
                const auto& tip = tip_result.value();
                auto* tip_index = chainstate->FindBlockIndex(tip.hash);
                if (tip_index) {
                    chainstate->AddCandidate(tip_index);
                    chainstate->ActivateBestChain();
                    std::cout << "[DaemonApp] ✅ BlockIndex graph rebuilt and tip activated" << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[DaemonApp] ❌ Exception during genesis initialization: " << e.what() << std::endl;
        return false;
    }

    // Phase 39: Create ChainstateService and pass it the ChainDB
    auto chainstate = std::make_shared<ChainstateService>();
    chainstate->setChainDB(chain_db_ptr);  // Non-owning; DaemonApp owns chain_db_
    chainstate->setPoolManager(pool_manager_runtime_);

    // Phase 8: Set validation mode based on config
    if (GetConfig().utreexo_stateless) {
        chainstate->setValidationMode(consensus::ValidationMode::STATELESS);
        std::cout << "[DaemonApp] ✅ Validation mode: STATELESS (Utreexo proof-based)" << std::endl;
    } else {
        chainstate->setValidationMode(consensus::ValidationMode::STATEFUL);
        std::cout << "[DaemonApp] ✅ Validation mode: STATEFUL (UTXO database)" << std::endl;
    }

    ctx_.chainstate = chainstate;
    services_.push_back(chainstate);

    auto mempool = std::make_shared<MempoolService>();
    ctx_.mempool = mempool;
    services_.push_back(mempool);

    // Step 5: Wire up ITxIngress and IBlockTemplateSource interfaces
    // MempoolService implements both interfaces
    ctx_.tx_ingress = mempool.get();
    ctx_.block_template_source = mempool.get();
    std::cout << "[DaemonApp] Step 5: ITxIngress and IBlockTemplateSource wired to MempoolService" << std::endl;

    // Step 5: Create BlockIngressService (IBlockIngress implementation)
    auto block_ingress = std::make_shared<BlockIngressService>();
    ctx_.block_ingress = block_ingress.get();
    services_.push_back(block_ingress);
    std::cout << "[DaemonApp] Step 5: IBlockIngress wired to BlockIngressService" << std::endl;

    // Phase C: Create BlockAssembler (belongs to consensus layer)
    // BlockAssembler creates block templates for mining
    // Dependencies: ChainDB (for chain tip, difficulty)
    // Will be injected into MiningManager during mining service Init()
    // NOTE: Mempool is wired AFTER services Init() in Start() - see wireBlockAssemblerMempool()
    std::cout << "[DaemonApp] Phase C: Creating BlockAssembler (block template assembly)" << std::endl;
    ctx_.block_assembler = std::make_unique<BlockAssembler>(chain_db_ptr);
    std::cout << "[DaemonApp] ✅ BlockAssembler created (mempool wiring deferred to Start)" << std::endl;

    auto wallet = std::make_shared<WalletService>();
    ctx_.wallet = wallet;
    services_.push_back(wallet);

    // 🛡️ Phase 5: Network & Protocol Hardening (November 11, 2025)

    // Phase 5D: Peer scoring and DoS protection
    // Replaces g_peer_scoring global - provides banscore-based reputation system
    auto peer_scoring = std::make_shared<daemon::PeerScoringService>();
    ctx_.peer_scoring = peer_scoring;
    services_.push_back(peer_scoring);
    std::cout << "[DaemonApp] ✅ PeerScoring service registered (DoS protection)" << std::endl;

    // Phase 5A: Headers-first sync
    // Replaces g_headers_sync global - provides efficient blockchain synchronization
    auto headers_sync = std::make_shared<daemon::HeadersSyncService>();
    ctx_.headers_sync = headers_sync;
    services_.push_back(headers_sync);
    std::cout << "[DaemonApp] ✅ HeadersSync service registered (headers-first sync)" << std::endl;

    // Phase 5B: Compact block relay (BIP152)
    // Replaces g_compact_blocks global - provides 90% bandwidth reduction
    auto compact_blocks = std::make_shared<daemon::CompactBlockService>();
    ctx_.compact_blocks = compact_blocks;
    services_.push_back(compact_blocks);
    std::cout << "[DaemonApp] ✅ CompactBlocks service registered (BIP152 relay)" << std::endl;

    // Phase 5C: Address manager and peer selection
    // Replaces g_addrman global - provides peer discovery and intelligent selection
    auto address_manager = std::make_shared<daemon::AddressManagerService>();
    ctx_.address_manager = address_manager;
    services_.push_back(address_manager);
    std::cout << "[DaemonApp] ✅ AddressManager service registered (peer discovery)" << std::endl;

    // Phase 5E: RBF (Replace-By-Fee) policy
    // Provides transaction replacement rules and fee increment requirements
    auto rbf_policy = std::make_shared<daemon::RBFPolicyService>();
    ctx_.rbf_policy = rbf_policy;
    services_.push_back(rbf_policy);
    std::cout << "[DaemonApp] ✅ RBFPolicy service registered (Replace-By-Fee)" << std::endl;

    // 🚀 Phase 6B: Parallel Validation & Pipelining (November 11, 2025)
    // Two-phase block validation: parallel script checks + serial UTXO updates
    // Expected: 1.5-2× faster validation for blocks with 20+ transactions

    // Step 1: Initialize chainstate guard (thread-safe UTXO access)
    auto chainstate_guard = std::make_shared<dinero::consensus::ChainstateGuard>();
    ctx_.chainstate_guard = chainstate_guard;
    std::cout << "[DaemonApp] ✅ ChainstateGuard initialized (thread-safe UTXO access)" << std::endl;

    // 🛡️ Phase E.2.d / E.3.1: Initialize CPU Budget Monitor
    // Tracks validation CPU usage and enforces timeouts to prevent DoS
    dinero::consensus::CPUBudgetConfig cpu_config;
    cpu_config.max_script_validation_ms = 100;      // 100ms per script
    cpu_config.max_block_validation_ms = 30000;     // 30s per block
    cpu_config.max_signature_verification_ms = 50;  // 50ms per signature
    cpu_config.enable_script_timeout = true;
    cpu_config.enable_block_timeout = true;
    cpu_config.enable_signature_timeout = true;

    ctx_.cpu_monitor = std::make_unique<dinero::consensus::CPUBudgetMonitor>(cpu_config);
    std::cout << "[DaemonApp] ✅ CPUBudgetMonitor initialized (script: " << cpu_config.max_script_validation_ms
              << "ms, block: " << cpu_config.max_block_validation_ms << "ms)" << std::endl;

    // 🛡️ Phase E.2.b: Initialize Disk Space Monitor
    // Get datadir for disk monitoring
    std::string disk_datadir = ctx_.config ?
        std::dynamic_pointer_cast<ConfigService>(ctx_.config)->DataDir() :
        "./dinero_data";

    dinero::storage::DiskLimitsConfig disk_config;
    disk_config.min_free_bytes = 1024ULL * 1024 * 1024;  // 1 GB minimum
    disk_config.min_free_percent = 5.0;  // 5% minimum
    disk_config.low_space_threshold_bytes = 5ULL * 1024 * 1024 * 1024;  // 5 GB warning
    disk_config.low_space_threshold_percent = 10.0;  // 10% warning

    ctx_.disk_monitor = std::make_unique<dinero::storage::DiskSpaceMonitor>(disk_datadir, disk_config);
    std::cout << "[DaemonApp] ✅ DiskSpaceMonitor initialized (datadir: " << disk_datadir
              << ", min_free: 1GB)" << std::endl;

    // 🧹 Phase 34.8: Initialize Prune Service
    ctx_.prune = std::make_shared<dinero::daemon::PruneService>();
    ctx_.prune->Init(ctx_);

    // Configure PruneService from CLI flags
    if (ctx_.prune && ctx_.config) {
        dinero::daemon::PruneConfig prune_cfg;

        // Check -prune flag
        // Archival guard: if --archival is set (default for controlled nodes),
        // pruning is permanently disabled regardless of -prune flag.
        // This prevents accidental data loss on seed nodes.
        const bool archival_mode = ctx_.config->GetBool("storage.archival", true);

        std::string prune_val = ctx_.config->GetString("storage.prune_target", "");
        if (!prune_val.empty()) {
            if (archival_mode) {
                g_logger.warn("[Pruning] -prune flag IGNORED: node is in archival mode (default). "
                              "Use --no-archival to explicitly allow pruning.");
            } else {
                uint32_t keep_blocks = static_cast<uint32_t>(std::stoul(prune_val));
                if (keep_blocks > 0 && keep_blocks < 288) {
                    g_logger.warn("[Pruning] -prune value below minimum, using 288");
                    keep_blocks = 288;
                }
                prune_cfg.enabled = true;
                prune_cfg.keep_blocks = keep_blocks;
                g_logger.info("[Pruning] Enabled: keep " + std::to_string(keep_blocks) + " blocks");
            }
        }

        if (archival_mode) {
            prune_cfg.enabled = false;
            g_logger.info("[Pruning] Archival mode (default): keeping ALL blocks and blk*.dat files permanently");
        }

        ctx_.prune->setConfig(prune_cfg);
    }
    std::cout << "[DaemonApp] ✅ PruneService initialized" << std::endl;

    // 🛡️ Phase E.2.c: Initialize Network Limits Monitor
    // NOTE: NetworkLimitsMonitor requires P2P internal components (ConnectionManager, RateLimiter, PeerScoringManager)
    // These are owned by P2PService and not directly accessible during DaemonContext initialization.
    // Network monitoring will be wired after P2PService initialization in Start().
    // For now, network status in RPC will return safe defaults.

    // Note: ParallelBlockValidator initialization moved to Start() after service Init()
    // (needs UTXO index which is only available after chainstate->Init())

    // 🔗 Phase N: Headers-First Blockchain Synchronization
    std::cout << "[DaemonApp] Phase N: Headers-first blockchain sync" << std::endl;

    // Get datadir for header storage
    std::string datadir = ctx_.config ?
        std::dynamic_pointer_cast<ConfigService>(ctx_.config)->DataDir() :
        "./dinero_data";

    // Initialize HeaderStore (persistent header storage)
    auto header_store = std::make_shared<dinero::consensus::HeaderStore>(datadir + "/headers");
    if (!header_store->Open()) {
        std::cerr << "[DaemonApp] ❌ Failed to open header store" << std::endl;
        return false;
    }
    ctx_.header_store = header_store;
    std::cout << "[DaemonApp] ✅ HeaderStore initialized (" << datadir << "/headers)" << std::endl;

    // Initialize HeaderChainSelector (consensus logic)
    auto header_chain = std::make_shared<dinero::consensus::HeaderChainSelector>(header_store.get());
    ctx_.header_chain = header_chain;
    std::cout << "[DaemonApp] ✅ HeaderChainSelector initialized" << std::endl;

    const bool has_legacy_entries = header_store->HasLegacyEntries();
    const bool schema_recovery_required = header_store->NeedsSchemaRecovery();
    if (has_legacy_entries || schema_recovery_required) {
        std::string recovery_reason;
        if (schema_recovery_required) {
            recovery_reason = header_store->GetSchemaRecoveryReason();
        }
        if (has_legacy_entries) {
            if (!recovery_reason.empty()) {
                recovery_reason += "; ";
            }
            recovery_reason += "legacy-format headers detected";
        }

        std::cerr << "[DaemonApp] ⚠️ HeaderStore incompatible: " << recovery_reason
                  << ". Quarantining local header state and reseeding from the active chain."
                  << std::endl;

        const std::filesystem::path headers_path(datadir + "/headers");
        const std::filesystem::path backup_path =
            BuildHeaderStoreBackupPath(headers_path, recovery_reason);

        header_chain.reset();
        ctx_.header_chain.reset();

        header_store->Close();
        header_store.reset();
        ctx_.header_store.reset();

        std::string quarantine_detail;
        if (!QuarantineHeaderStoreDirectory(headers_path, backup_path, quarantine_detail)) {
            std::cerr << "[DaemonApp] ❌ Failed to quarantine header store: "
                      << quarantine_detail << std::endl;
            return false;
        }

        std::cerr << "[DaemonApp] ✅ HeaderStore quarantined at " << backup_path
                  << " (" << quarantine_detail << ")" << std::endl;

        header_store = std::make_shared<dinero::consensus::HeaderStore>(headers_path.string());
        if (!header_store->Open()) {
            std::cerr << "[DaemonApp] ❌ Failed to reopen clean header store" << std::endl;
            return false;
        }
        ctx_.header_store = header_store;

        header_chain = std::make_shared<dinero::consensus::HeaderChainSelector>(header_store.get());
        ctx_.header_chain = header_chain;
        std::cout << "[DaemonApp] ✅ Clean HeaderStore reinitialized (" << headers_path
                  << ")" << std::endl;
    }

    // Phase N: Seed HeaderChainSelector with the full active header chain from ChainDB.
    // IMPORTANT: Do not rely on height index ordering here (it may be stale after prior forks).
    // We reconstruct by walking tip -> genesis via prev_block_hash, then replay genesis -> tip.
    if (chain_db_ptr) {
        auto tip_result = chain_db_ptr->getTip();
        if (tip_result.status() == Status::Ok) {
            const uint256 tip_hash = tip_result.value().hash;
            const uint32_t tip_height = static_cast<uint32_t>(tip_result.value().height);

            std::vector<uint256> chain_hashes;
            chain_hashes.reserve(static_cast<size_t>(tip_height) + 1);

            uint256 current_hash = tip_hash;
            bool reached_genesis = false;

            for (uint32_t steps = 0; steps <= tip_height + 1; ++steps) {
                chain_hashes.push_back(current_hash);

                auto header_result = chain_db_ptr->getHeader(current_hash);
                if (header_result.status() != Status::Ok) {
                    std::cerr << "[DaemonApp] ❌ HeaderChainSelector seed walk failed: missing header "
                              << current_hash.GetHex().substr(0, 16) << "..." << std::endl;
                    break;
                }

                const BlockHeader& header = header_result.value();
                if (header.prev_block_hash.IsNull()) {
                    reached_genesis = true;
                    break;
                }
                current_hash = header.prev_block_hash;
            }

            if (!reached_genesis) {
                std::cerr << "[DaemonApp] ❌ HeaderChainSelector seeding aborted: could not reach genesis from tip "
                          << tip_hash.GetHex().substr(0, 16) << "..." << std::endl;
            } else {
                std::reverse(chain_hashes.begin(), chain_hashes.end());

                uint32_t headers_added = 0;
                bool replay_failed = false;
                for (const auto& hash : chain_hashes) {
                    auto header_result = chain_db_ptr->getHeader(hash);
                    if (header_result.status() != Status::Ok) {
                        std::cerr << "[DaemonApp] ❌ HeaderChainSelector seed failed: missing header during replay "
                                  << hash.GetHex().substr(0, 16) << "..." << std::endl;
                        replay_failed = true;
                        break;
                    }
                    if (!header_chain->AddHeader(header_result.value())) {
                        std::cerr << "[DaemonApp] ❌ HeaderChainSelector seed failed: parent-link rejection at "
                                  << hash.GetHex().substr(0, 16) << "..." << std::endl;
                        replay_failed = true;
                        break;
                    }
                    headers_added++;
                }

                // Recovery fallback:
                // If persisted selector state is corrupt/incomplete, clear and reseed
                // from active chain only. Normal path keeps persisted side branches.
                if (replay_failed) {
                    std::cerr << "[DaemonApp] ⚠️ HeaderChainSelector replay failed; "
                              << "recovering via clear + active-chain reseed." << std::endl;
                    header_chain->Clear();
                    headers_added = 0;

                    for (const auto& hash : chain_hashes) {
                        auto header_result = chain_db_ptr->getHeader(hash);
                        if (header_result.status() != Status::Ok) {
                            break;
                        }
                        if (!header_chain->AddHeader(header_result.value())) {
                            break;
                        }
                        headers_added++;
                    }
                }

                std::cout << "[DaemonApp] ✅ Seeded HeaderChainSelector with " << headers_added
                          << "/" << chain_hashes.size()
                          << " headers from active tip walk (tip height " << tip_height
                          << ", preserved persisted side branches when valid)" << std::endl;

                if (headers_added != chain_hashes.size()) {
                    std::cerr << "[DaemonApp] ⚠️ HeaderChainSelector seeding INCOMPLETE after tip walk; "
                              << "header sync may reject fork batches until repaired." << std::endl;
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Startup audit: Fix stale height→hash index entries from pre-fix reorgs.
    // Walks the HEADER CHAIN backwards from the DB tip via prev_block_hash
    // (authoritative parent links) and corrects any height index entries
    // that don't match. This is independent of the in-memory block index
    // which may itself have been built from stale height→hash data.
    // ═══════════════════════════════════════════════════════════════════════════
    {
        auto tip_result = chain_db_ptr->getTip();
        if (tip_result.status() == Status::Ok) {
            ChainWriteToken token;
            uint32_t fixed = 0;
            uint256 current_hash = tip_result.value().hash;
            int current_height = tip_result.value().height;

            while (current_height > 0) {
                auto existing = chain_db_ptr->getBlockHashByHeight(current_height);
                if (existing.status() == Status::Ok && existing.value() != current_hash) {
                    chain_db_ptr->putHeightIndex(token, current_height, current_hash);
                    fixed++;
                    std::cout << "[DaemonApp] Height index audit: height " << current_height
                              << " was " << existing.value().GetHex().substr(0, 16)
                              << "... now " << current_hash.GetHex().substr(0, 16) << "..." << std::endl;
                }
                // Walk backwards via the header's prev_block_hash (authoritative)
                auto header_result = chain_db_ptr->getHeader(current_hash);
                if (header_result.status() != Status::Ok) break;
                current_hash = header_result.value().prev_block_hash;
                current_height--;
            }
            if (fixed > 0) {
                std::cout << "[DaemonApp] ✅ Height index audit: fixed " << fixed
                          << " stale entries from previous reorgs" << std::endl;
            }
        }
    }

    // Release the temporary archival reader before bringing up the shared
    // runtime BlockStorage instance on the same datadir.
    startup_block_storage.reset();

    // Initialize HeaderSyncP2P (P2P protocol layer)
    auto header_sync = std::make_shared<dinero::consensus::HeaderSyncP2P>(header_chain.get());
    ctx_.header_sync = header_sync;
    std::cout << "[DaemonApp] ✅ HeaderSyncP2P initialized" << std::endl;

    // Initialize BlockStorage (flat file storage for downloaded blocks)
    auto block_storage = std::make_shared<dinero::BlockStorage>();
    auto storage_status = block_storage->init(std::filesystem::path(datadir));
    if (storage_status != Status::Ok) {
        std::cerr << "[DaemonApp] ⚠️ Failed to initialize BlockStorage: "
                 << StatusToString(storage_status) << std::endl;
        // Continue without block storage - headers sync will still work
    } else {
        ctx_.block_storage = block_storage;
        std::cout << "[DaemonApp] ✅ BlockStorage initialized (" << datadir << "/blocks)" << std::endl;
    }

    // Initialize BlockDownloadScheduler (block download orchestration)
    auto block_download = std::make_shared<dinero::consensus::BlockDownloadScheduler>(
        header_chain.get(),
        block_storage.get()
    );
    ctx_.block_download = block_download;

    // Seed the scheduler from the validated active chain tip, not the raw
    // ChainDB storage tip. ChainDB can be ahead during restart recovery when
    // blocks are stored on disk but chainstate has not replayed them yet.
    uint32_t scheduler_local_tip = 0;
    int stored_tip_height = -1;
    if (ctx_.chainstate) {
        if (auto* active_tip = ctx_.chainstate->GetActiveTip()) {
            scheduler_local_tip = static_cast<uint32_t>(active_tip->height);
        }
    }
    if (chain_db_ptr) {
        auto tip_result = chain_db_ptr->getTip();
        if (tip_result.status() == Status::Ok) {
            stored_tip_height = tip_result.value().height;
        }
    }
    block_download->SetLocalTipHeight(scheduler_local_tip);
    std::cout << "[DaemonApp] ✅ BlockDownloadScheduler initialized (active tip: "
              << scheduler_local_tip;
    if (stored_tip_height >= 0 && static_cast<uint32_t>(stored_tip_height) != scheduler_local_tip) {
        std::cout << ", stored tip: " << stored_tip_height;
    }
    std::cout << ")" << std::endl;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase G: Parallel Block Download Scheduler (10-20× IBD speedup)
    // ═══════════════════════════════════════════════════════════════════════════
    // Kill-switch: parallel_block_download config option (default: true)
    if (GetConfig().parallel_block_download) {
        std::cout << "[DaemonApp] Phase G: Initializing parallel block download..." << std::endl;

        // Create parallel scheduler with a live targeted-send callback. The
        // callback resolves P2PService at send time because P2PService is not
        // constructed yet at this point in daemon startup.
        auto parallel_download = std::make_shared<dinero::BlockDownloadScheduler>(
            [this](peer_id_t peer_id, const uint256& block_hash) -> bool {
                auto* ctx = DaemonContext::instance();
                if (!ctx || !ctx->p2p) {
                    g_logger.warning("[ParallelBlockDownloadScheduler] P2P service unavailable for GETDATA");
                    return false;
                }

                auto p2p_service = std::dynamic_pointer_cast<P2PService>(ctx->p2p);
                if (!p2p_service) {
                    g_logger.warning("[ParallelBlockDownloadScheduler] P2P service type mismatch");
                    return false;
                }

                const bool csn_mode = GetConfig().utreexo_stateless;
                const uint32_t inv_type = csn_mode
                    ? static_cast<uint32_t>(dinero::InventoryType::MSG_UTREEXO_BLOCK)
                    : static_cast<uint32_t>(dinero::InventoryType::MSG_BLOCK);
                ::P2PMessage msg = ::P2PMessage::create_getdata_binary(
                    block_hash.begin(), 32, inv_type
                );
                const bool sent = p2p_service->get().send_to_peer(peer_id, msg);
                if (!sent) {
                    g_logger.warning("[ParallelBlockDownloadScheduler] Failed targeted GETDATA for block " +
                                     block_hash.GetHex().substr(0, 16) + "... to peer " + peer_id);
                }
                return sent;
            }
        );
        ctx_.parallel_block_download = parallel_download;
        std::cout << "[DaemonApp] ✅ ParallelBlockDownloadScheduler initialized (Phase G: 10-20× IBD speedup)" << std::endl;
    } else {
        std::cout << "[DaemonApp] ℹ️  Parallel block download DISABLED (config: parallel_block_download=false)" << std::endl;
    }

    // Phase G.2: Block propagation (minimal relay)
    auto block_relay = std::make_shared<BlockRelayManager>(
        ctx_.logger_interface,
        ctx_.parallel_block_download
    );
    ctx_.block_relay = block_relay;
    std::cout << "[DaemonApp] ✅ BlockRelayManager initialized (Phase G.2)" << std::endl;

    // Phase G.3: Mempool relay (minimal transaction propagation)
    auto tx_relay = std::make_shared<TxRelayManager>(ctx_.logger_interface);
    ctx_.tx_relay = tx_relay;
    std::cout << "[DaemonApp] ✅ TxRelayManager initialized (Phase G.3)" << std::endl;

    // Transaction orphan pool (holds TXs with missing parents for later resolution)
    auto tx_orphan_pool = std::make_unique<TxOrphanPool>();
    ctx_.orphan_pool = tx_orphan_pool.get();  // Non-owning pointer for RPC access
    tx_relay->SetOrphanPool(tx_orphan_pool.get());
    orphan_pool_owned_ = std::move(tx_orphan_pool);  // DaemonApp owns the lifetime
    std::cout << "[DaemonApp] ✅ TxOrphanPool initialized" << std::endl;

    // Phase 3: Network layer (depends on data layer)
    std::cout << "[DaemonApp] Phase 3: Network layer" << std::endl;

    auto p2p = std::make_shared<P2PService>();
    ctx_.p2p = p2p;
    services_.push_back(p2p);

    // Phase 4: Application layer (depends on everything)
    std::cout << "[DaemonApp] Phase 4: Application layer" << std::endl;

    auto mining = std::make_shared<MiningService>();
    ctx_.mining = mining;
    services_.push_back(mining);

    auto metrics = std::make_shared<MetricsService>();
    ctx_.metrics = metrics;
    services_.push_back(metrics);

    auto rpc = std::make_shared<RPCService>();
    ctx_.rpc = rpc;
    services_.push_back(rpc);


    // ⚡ Phase 7: Lightning Network (November 11, 2025)
    // ═════════════════════════════════════════════════════════════════════
    // Phase 3 COMPLETE: Lightning is now fully decoupled!
    // Lightning runs as a SEPARATE PROCESS (lightningd)
    // - dinerod = pure L1 node (blockchain, mempool, wallet)
    // - lightningd = optional L2 daemon (channels, HTLCs, routing)
    // - Communication via gRPC (WalletClient → dinerod gRPC server)
    // ═════════════════════════════════════════════════════════════════════
    std::cout << "[DaemonApp] ⚡ Lightning: DISABLED (use standalone 'lightningd' binary)" << std::endl;

    // Lightning auto-init DISABLED - run lightningd separately if needed
    // auto lightning = std::make_shared<dinero::lightning::LightningService>();
    // ctx_.lightning = lightning;
    // services_.push_back(lightning);

    // Phase 5: Optional services (feature extensions)
    std::cout << "[DaemonApp] Phase 5: Optional services (disabled for now)" << std::endl;

    // TODO: Re-enable optional services once they're needed
    // ctx_.event_bus = ... (EventBus)
    // ctx_.fiat_bridge = ... (FiatBridgeManager)
    // ctx_.marketplace = ... (MarketplaceManager)
    // ctx_.escrow = ... (EscrowManager)

    // Initialize logger interface for dependency injection BEFORE service Init()
    // ProductionLogger wraps the existing g_logger, enabling gradual migration
    ctx_.logger_interface = &dinero::ProductionLogger::instance();
    std::cout << "[DaemonApp] ✅ Logger interface initialized (dependency injection ready)" << std::endl;

    // Initialize all services
    for (auto& service : services_) {
        std::cout << "[DaemonApp] Initializing " << service->Name() << "..." << std::endl;
        if (!service->Init(ctx_)) {
            std::cerr << "[DaemonApp] ❌ Failed to initialize " << service->Name() << std::endl;
            return false;
        }
        std::cout << "[DaemonApp] ✅ " << service->Name() << " initialized" << std::endl;
    }

    // Phase 2: Create consensus engine after all services are initialized
    // Consensus engine needs ChainDB (from ChainstateService) and Mining (from MiningService)
    if (ctx_.chainstate && ctx_.mining) {
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        if (chainstate) {
            // Phase C: Create PoW consensus engine using BlockAssembler
            auto chain_db = chainstate->GetChainDB();
            if (chain_db && ctx_.block_assembler) {
                ctx_.consensus = CreatePowConsensusEngine(ctx_.block_assembler.get(), chain_db);
                std::cout << "[DaemonApp] ✅ Consensus engine created: " << ctx_.consensus->GetName() << std::endl;
            } else {
                if (!chain_db) {
                    std::cerr << "[DaemonApp] ⚠️ ChainDB not available, consensus engine not created" << std::endl;
                }
                if (!ctx_.block_assembler) {
                    std::cerr << "[DaemonApp] ⚠️ BlockAssembler not available, consensus engine not created" << std::endl;
                }
            }
        }
    }

    // Phase 39: ChainManager->SetMempool() call removed (ChainManager deleted)
    // Reorg reconciliation is now handled via service-to-service communication

    // Phase C.1 v2: Wire ChainstateService to P2PService for block broadcasting
    if (ctx_.chainstate && ctx_.p2p) {
        auto chainstate_service = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        auto p2p_service = std::dynamic_pointer_cast<P2PService>(ctx_.p2p);
        if (chainstate_service && p2p_service) {
            // Wire P2PService reference to Chainstate (for broadcasting)
            chainstate_service->setP2PService(p2p_service);
            std::cout << "[DaemonApp] ✅ ChainstateService wired to P2PService (block broadcasting enabled)" << std::endl;

            // Phase G.2: Wire BlockRelayManager to Chainstate (for announcements)
            if (ctx_.block_relay) {
                chainstate_service->setBlockRelayManager(ctx_.block_relay);
                std::cout << "[DaemonApp] ✅ ChainstateService wired to BlockRelayManager (Phase G.2 announcements)" << std::endl;
            }

            // P2P FIX: Wire HeaderChainSelector to Chainstate (for mined block tracking)
            // When blocks are connected via ConnectTip, their headers are added to HeaderChainSelector
            // This ensures header chain comparison works correctly during reorgs
            if (ctx_.header_chain) {
                chainstate_service->setHeaderChainSelector(ctx_.header_chain);
                std::cout << "[DaemonApp] ✅ ChainstateService wired to HeaderChainSelector (mined block tracking)" << std::endl;
            }

            // Phase 9.2: Wire ChainOracleClient to Chainstate (for Lightning events)
            {
                auto chain_oracle = std::make_unique<dinero::ipc::ChainOracleClient>("/tmp/lightningd.sock");
                if (chain_oracle->connect()) {
                    chainstate_service->setChainOracleClient(std::move(chain_oracle));
                    std::cout << "[DaemonApp] ✅ ChainOracleClient wired (Phase 9.2: Lightning chain events enabled)" << std::endl;
                } else {
                    std::cout << "[DaemonApp] ℹ️  ChainOracleClient connection failed (lightningd not running?)" << std::endl;
                }
            }

            // Phase 9.2: Wire TimeOracleClient to Chainstate (for Lightning time tracking)
            {
                auto time_oracle = std::make_unique<dinero::ipc::TimeOracleClient>("/tmp/lightningd.sock");
                if (time_oracle->connect()) {
                    chainstate_service->setTimeOracleClient(std::move(time_oracle));
                    std::cout << "[DaemonApp] ✅ TimeOracleClient wired (Phase 9.2: Lightning time tracking enabled)" << std::endl;
                } else {
                    std::cout << "[DaemonApp] ℹ️  TimeOracleClient connection failed (lightningd not running?)" << std::endl;
                }
            }

            // Phase 9.2/9.3: Wire TransactionOracleClient to Chainstate (for Lightning TX tracking)
            // Phase 9.3: Now shared_ptr (shared with WatchRegistrationServer for bidirectional communication)
            {
                auto tx_oracle = std::make_shared<dinero::ipc::TransactionOracleClient>("/tmp/lightningd.sock");
                if (tx_oracle->connect()) {
                    chainstate_service->setTransactionOracleClient(tx_oracle);
                    std::cout << "[DaemonApp] ✅ TransactionOracleClient wired (Phase 9.2: Lightning TX tracking enabled)" << std::endl;

                    // Phase 9.3: Start watch registration server (bidirectional oracle communication)
                    // Allows lightningd to register transaction watches dynamically
                    watch_registration_server_ = std::make_unique<dinero::ipc::WatchRegistrationServer>(
                        "/tmp/dinerod.sock",
                        tx_oracle
                    );
                    if (watch_registration_server_->start()) {
                        std::cout << "[DaemonApp] ✅ WatchRegistrationServer started (Phase 9.3: Bidirectional oracle enabled)" << std::endl;
                    } else {
                        std::cout << "[DaemonApp] ⚠️  WatchRegistrationServer failed to start" << std::endl;
                        watch_registration_server_.reset();
                    }
                } else {
                    std::cout << "[DaemonApp] ℹ️  TransactionOracleClient connection failed (lightningd not running?)" << std::endl;
                }
            }

            // Phase G.3: Wire TxRelayManager to Mempool (for announcements)
            auto mempool_service = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);
            if (ctx_.tx_relay && mempool_service) {
                mempool_service->setTxRelayManager(ctx_.tx_relay);
                std::cout << "[DaemonApp] ✅ MempoolService wired to TxRelayManager (Phase G.3 announcements)" << std::endl;
            }

            // Phase G.13: Wire BlockRelayManager to the canonical daemon mempool for
            // compact-block reconstruction against live transaction state.
            if (ctx_.block_relay && mempool_service) {
                ctx_.block_relay->SetMempool(&mempool_service->mempool());
                ctx_.block_relay->SetChainDB(chainstate_service->GetChainDB());
                std::cout << "[DaemonApp] ✅ BlockRelayManager wired to MempoolService (compact reconstruction)" << std::endl;
            }

            // Phase G.2/G.3: Wire P2P relay handlers (blocks and transactions)
            if (ctx_.block_relay || ctx_.tx_relay) {
                auto block_relay = ctx_.block_relay;
                auto tx_relay = ctx_.tx_relay;

                // Phase P.2: Create BridgeNode for serving utreexo blocks to stateless/mobile clients
                std::shared_ptr<network::BridgeNode> bridge_node;
                auto utreexo_forest = chainstate_service->utreexoForest();
                auto* consensus_utxo_set = chainstate_service->GetConsensusUTXOSet();
                if (utreexo_forest && consensus_utxo_set) {
                    // Use consensus UTXO set (ALL chain UTXOs) — NOT wallet index (wallet-only).
                    // IConsensusUTXOSet IS-A IUTXOProvider. Aliasing shared_ptr: non-owning,
                    // safe because ChainstateService outlives BridgeNode (shutdown order).
                    auto utxo_provider = std::shared_ptr<consensus::IUTXOProvider>(
                        std::shared_ptr<void>{}, static_cast<consensus::IUTXOProvider*>(consensus_utxo_set));
                    bridge_node = std::make_shared<network::BridgeNode>(
                        utxo_provider,
                        utreexo_forest,
                        nullptr,                           // No proof cache for now
                        chainstate_service->GetChainDB(),  // ChainDB for height lookups
                        ctx_.block_storage.get()           // Flatfile-first block body reads
                    );
                    std::cout << "[DaemonApp] BridgeNode created with consensus UTXO set (Phase P.2)" << std::endl;

                    // Phase P.2: Wire BridgeNode to ChainstateService for proof pre-caching at connect time
                    // This ensures proofs are generated BEFORE the forest is updated
                    chainstate_service->setBridgeNode(bridge_node);
                    std::cout << "[DaemonApp] BridgeNode wired to ChainstateService (proof pre-caching enabled)" << std::endl;
                }

                // Phase 9.3: Best-effort proof gossip layer (invproof/getproof/proofdata).
                // This distributes recent proof availability and reduces repeated direct requests.
                std::shared_ptr<consensus::ProofGossipManager> proof_gossip;
                auto announced_proof_mutex = std::make_shared<std::mutex>();
                auto announced_proof_hashes = std::make_shared<std::unordered_set<uint256>>();
                auto announced_proof_order = std::make_shared<std::deque<uint256>>();
                if (bridge_node) {
                    proof_gossip = std::make_shared<consensus::ProofGossipManager>();
                    proof_gossip->SetProofProvider(
                        [bridge_node, chainstate_service](const uint256& block_hash)
                            -> std::optional<consensus::BlockUtreexoData> {
                            ChainDB* chain_db = chainstate_service ? chainstate_service->GetChainDB() : nullptr;
                            if (!chain_db) {
                                return std::nullopt;
                            }

                            GetUtreexoProofMessage request;
                            request.block_hashes.push_back(block_hash);
                            if (!request.isValid()) {
                                return std::nullopt;
                            }

                            auto block_provider = [chainstate_service](const uint256& requested_hash) -> std::optional<Block> {
                                auto block_result = chainstate_service->getBlockByHash(requested_hash);
                                if (block_result.status() != Status::Ok) {
                                    return std::nullopt;
                                }
                                return block_result.value();
                            };

                            auto proof_result = bridge_node->HandleProofRequest(request, block_provider);
                            if (proof_result.proofs.empty()) {
                                return std::nullopt;
                            }
                            return proof_result.proofs.front().proof_data;
                        });
                    chainstate_service->setProofGossipManager(proof_gossip);
                    std::cout << "[DaemonApp] ✅ Proof gossip manager initialized (Phase 9.3)" << std::endl;
                }

                // Phase P.3: Create StatelessNode for CSN block+proof validation
                std::shared_ptr<network::StatelessNode> stateless_node;
                if (GetConfig().utreexo_stateless && utreexo_forest) {
                    stateless_node = std::make_shared<network::StatelessNode>(utreexo_forest);

                    // Wire block provider (for fetching blocks by hash during sync loop)
                    // Capture chainstate_service (shared_ptr) to ensure ChainDB lifetime
                    stateless_node->setBlockProvider([chainstate_service](const uint256& hash) -> std::optional<Block> {
                        auto result = chainstate_service->getBlockByHash(hash);
                        if (result.status() != Status::Ok) return std::nullopt;
                        return result.value();
                    });

                    // Wire proof requester (fallback path when separate proof messages are used).
                    // Prefer bridge peers to reduce proof-starvation risk.
                    stateless_node->setProofRequester([p2p_service, proof_gossip](const std::vector<uint256>& block_hashes) {
                        if (block_hashes.empty()) {
                            return;
                        }

                        GetUtreexoProofMessage request;
                        request.block_hashes = block_hashes;
                        if (!request.isValid()) {
                            g_logger.warning("[CSN] Invalid proof request batch (size=" +
                                             std::to_string(block_hashes.size()) + ")");
                            return;
                        }

                        ::P2PMessage msg;
                        msg.command = MessageCommands::GETUTREEXOPROOFS;
                        msg.payload = request.serialize();
                        msg.checksum = 0;

                        auto peers = p2p_service->get().get_connected_peers();
                        std::vector<std::string> utreexo_peers;
                        utreexo_peers.reserve(peers.size());
                        int bridge_sent = 0;
                        for (const auto& peer : peers) {
                            const std::string peer_key = peer.to_string();
                            if (p2p_service->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_UTREEXO)) {
                                utreexo_peers.push_back(peer_key);
                            }
                            if (!p2p_service->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_UTREEXO_BRIDGE)) {
                                continue;
                            }
                            if (p2p_service->get().send_to_peer(peer_key, msg)) {
                                bridge_sent++;
                            }
                        }

                        if (bridge_sent == 0) {
                            g_logger.warning("[CSN] No bridge peers available for getutxoproofs; falling back to GETPROOF gossip path");
                            if (!proof_gossip) {
                                g_logger.warning("[CSN] GETPROOF fallback unavailable (proof_gossip disabled)");
                                return;
                            }
                            if (utreexo_peers.empty()) {
                                g_logger.warning("[CSN] No NODE_UTREEXO peers available for GETPROOF fallback");
                                return;
                            }

                            size_t fallback_sent = 0;
                            for (size_t i = 0; i < block_hashes.size(); ++i) {
                                const auto req = proof_gossip->CreateProofRequest(block_hashes[i]);
                                ::P2PMessage fallback_msg;
                                fallback_msg.command = MessageCommands::GETPROOF;
                                fallback_msg.payload = req.Serialize();
                                fallback_msg.checksum = 0;

                                const std::string& peer_key = utreexo_peers[i % utreexo_peers.size()];
                                if (p2p_service->get().send_to_peer(peer_key, fallback_msg)) {
                                    fallback_sent++;
                                }
                            }

                            if (fallback_sent == 0) {
                                g_logger.warning("[CSN] GETPROOF fallback send failed for all blocks");
                            } else {
                                g_logger.info("[CSN] GETPROOF fallback sent for " + std::to_string(fallback_sent) +
                                              " block(s) across " + std::to_string(utreexo_peers.size()) + " peers");
                            }
                        }
                    });

                    // Wire peer ban callback
                    stateless_node->setPeerBanCallback([p2p_service](
                        const std::string& peer_id, const std::string& reason) {
                        g_logger.warning("[CSN] Banning peer " + peer_id + ": " + reason);
                        p2p_service->get().disconnect_peer(peer_id);
                    });

                    // Wire timeout disconnect callback (proof starvation mitigation).
                    stateless_node->setPeerDisconnectCallback([p2p_service](
                        const std::string& peer_id) {
                        g_logger.warning("[CSN] Disconnecting unresponsive proof peer " + peer_id);
                        p2p_service->get().disconnect_peer(peer_id);
                    });

                    // Wire StatelessNode to ChainstateService for CSN reorg support
                    chainstate_service->setStatelessNode(stateless_node);

                    std::cout << "[DaemonApp] ✅ StatelessNode created for CSN validation (Phase P.3)" << std::endl;
                }

                // OnInv: Handle block/tx announcements (Phase G.2 + G.3)
                // Note: CSN block requests now go through headers-first path (SetSendGetDataCallback)
                // which sends MSG_UTREEXO_BLOCK automatically when utreexo_stateless=true.
                // INV-based relay uses standard HandleInv for both full and CSN nodes.
                auto block_download_for_inv = ctx_.block_download;  // IBD check for inv suppression
                auto chainstate_for_inv = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
                const bool csn_mode_for_inv = GetConfig().utreexo_stateless;
                auto inv_header_refresh_times =
                    std::make_shared<std::unordered_map<std::string, std::chrono::steady_clock::time_point>>();
                auto inv_header_refresh_mutex = std::make_shared<std::mutex>();
                p2p_service->OnInv = [block_relay, tx_relay, block_download_for_inv,
                                      chainstate_for_inv, p2p_service, csn_mode_for_inv,
                                      inv_header_refresh_times, inv_header_refresh_mutex](
                    const std::string& peer_addr, const ::P2PMessage& msg) {
                    g_logger.info("[Relay] OnInv received from " + peer_addr + " (payload size: " + std::to_string(msg.payload.size()) + ")");

                    // Parse inv message: count (1 byte) + [type (4 bytes) + hash (32 bytes)]*count
                    if (msg.payload.empty()) {
                        g_logger.warning("[Relay] Invalid inv message from " + peer_addr);
                        return;
                    }

                    uint8_t inv_count = msg.payload[0];
                    size_t expected_size = 1 + static_cast<size_t>(inv_count) * 36;
                    if (msg.payload.size() < expected_size) {
                        g_logger.warning("[Relay] Truncated inv message from " + peer_addr +
                                         " (count=" + std::to_string(inv_count) +
                                         ", size=" + std::to_string(msg.payload.size()) + ")");
                        return;
                    }

                    bool requested_headers_refresh = false;
                    auto request_headers_refresh = [&]() -> bool {
                        if (requested_headers_refresh || !chainstate_for_inv || !p2p_service) {
                            return false;
                        }

                        const bool near_tip_refresh =
                            block_download_for_inv && block_download_for_inv->IsFullySynchronized();
                        const auto now = std::chrono::steady_clock::now();
                        if (!near_tip_refresh) {
                            std::lock_guard<std::mutex> lock(*inv_header_refresh_mutex);
                            auto it = inv_header_refresh_times->find(peer_addr);
                            if (it != inv_header_refresh_times->end() &&
                                now - it->second < std::chrono::milliseconds(750)) {
                                g_logger.debug("[Relay] Header refresh already requested recently for " +
                                               peer_addr + "; coalescing block inv");
                                return false;
                            }
                            (*inv_header_refresh_times)[peer_addr] = now;
                        }

                        auto locator = chainstate_for_inv->GenerateBlockLocator();
                        std::vector<std::string> locator_hex;
                        locator_hex.reserve(locator.size());
                        for (const auto& hash_item : locator) {
                            locator_hex.push_back(hash_item.GetHex());
                        }

                        auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                        p2p_service->get().send_to_peer(peer_addr, getheaders_msg);
                        requested_headers_refresh = true;
                        return true;
                    };

                    for (size_t i = 0; i < inv_count; ++i) {
                        size_t offset = 1 + i * 36;

                        // Extract inventory type (4 bytes LE)
                        uint32_t inv_type = 0;
                        std::memcpy(&inv_type, &msg.payload[offset], 4);

                        // Extract hash (32 bytes)
                        uint256 hash;
                        std::memcpy(hash.data, &msg.payload[offset + 4], 32);

                        g_logger.info("[Relay] Parsed inv[" + std::to_string(i) + "/" +
                                      std::to_string(inv_count) + "]: type=" +
                                      std::to_string(inv_type) + " hash=" +
                                      hash.GetHex().substr(0, 16) + "...");

                        // Route based on inventory type
                        if ((inv_type == 1 || inv_type == 0x50000001) && tx_relay) {
                            // MSG_TX = 1, MSG_UTREEXO_TX = 0x50000001
                            g_logger.info("[TX-RELAY] Received INV for tx " + hash.GetHex() +
                                         " (type=" + std::to_string(inv_type) + ")");
                            tx_relay->HandleInv(peer_addr, hash);
                        } else if (inv_type == 2 && block_relay) {
                            // MSG_BLOCK = 2
                            if (csn_mode_for_inv) {
                                request_headers_refresh();

                                g_logger.info("[Relay] CSN mode routes block inv through headers-first sync, not BlockRelayManager (" +
                                              peer_addr + ")");
                                continue;
                            }
                            if (block_download_for_inv &&
                                !block_download_for_inv->IsFullySynchronized()) {
                                request_headers_refresh();

                                if (!block_download_for_inv->HeadersSynced()) {
                                    g_logger.info("[Relay] Headers not yet synced; requesting headers instead of routing block inv from " +
                                                  peer_addr);
                                } else {
                                    g_logger.info("[Relay] Block sync still catching up; requesting headers refresh instead of routing block inv from " +
                                                  peer_addr);
                                }
                                continue;
                            }
                            g_logger.info("[Relay] Routing block inv to BlockRelayManager");
                            block_relay->HandleInv(peer_addr, hash);
                        } else {
                            g_logger.debug("[Relay] Unknown inventory type " + std::to_string(inv_type) + " from " + peer_addr);
                        }
                    }
                };

                // OnGetData: Handle block/tx requests (Phase G.2 + G.3 + P.2 + #4)
                struct UtreexoGetDataPeerBudget {
                    std::chrono::steady_clock::time_point window_start{};
                    uint32_t utreexo_block_requests{0};
                    uint32_t utreexo_tx_requests{0};
                    uint32_t total_utreexo_requests{0};
                    uint32_t consecutive_violations{0};
                };
                auto utreexo_getdata_budget_mutex = std::make_shared<std::mutex>();
                auto utreexo_getdata_budget =
                    std::make_shared<std::unordered_map<std::string, UtreexoGetDataPeerBudget>>();
                auto mempool_for_getdata = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);
                auto prune_for_getdata = ctx_.prune;
                p2p_service->OnGetData = [block_relay, tx_relay, bridge_node, chainstate_service, p2p_service,
                                          proof_gossip, announced_proof_mutex, announced_proof_hashes,
                                          announced_proof_order, mempool_for_getdata, prune_for_getdata,
                                          utreexo_getdata_budget_mutex,
                                          utreexo_getdata_budget](const std::string& peer_addr,
                                                                  const ::P2PMessage& msg) {
                    g_logger.info("[Relay] OnGetData received from " + peer_addr + " (payload size: " + std::to_string(msg.payload.size()) + ")");

                    // Parse getdata message: count (1 byte) + [type (4) + hash (32)] * count
                    if (msg.payload.empty()) {
                        g_logger.warning("[Relay] Invalid getdata message from " + peer_addr);
                        return;
                    }

                    uint8_t inv_count = msg.payload[0];
                    size_t expected_size = 1 + static_cast<size_t>(inv_count) * 36;
                    if (msg.payload.size() < expected_size) {
                        g_logger.warning("[Relay] Truncated getdata message from " + peer_addr +
                                         " (count=" + std::to_string(inv_count) +
                                         ", size=" + std::to_string(msg.payload.size()) + ")");
                        return;
                    }

                    // Route based on inventory type
                    // Standard Bitcoin types:
                    //   MSG_TX = 1, MSG_BLOCK = 2
                    // Utreexo extension types (BIP proposal):
                    //   MSG_UTREEXO_BLOCK = 0x50000002 (block + Utreexo proof)
                    constexpr uint32_t MSG_TX = static_cast<uint32_t>(dinero::InventoryType::MSG_TX);
                    constexpr uint32_t MSG_BLOCK = static_cast<uint32_t>(dinero::InventoryType::MSG_BLOCK);
                    constexpr uint32_t MSG_UTREEXO_BLOCK = static_cast<uint32_t>(dinero::InventoryType::MSG_UTREEXO_BLOCK);
                    constexpr uint32_t MSG_UTREEXO_TX = static_cast<uint32_t>(dinero::InventoryType::MSG_UTREEXO_TX);
                    constexpr uint32_t GETDATA_WINDOW_SECONDS = 10;
                    constexpr uint32_t MAX_UTREEXO_GETDATA_PER_WINDOW = 600;
                    constexpr uint32_t MAX_UTREEXO_BLOCK_GETDATA_PER_WINDOW = 500;  // Bulk sync needs headroom
                    constexpr uint32_t MAX_UTREEXO_TX_GETDATA_PER_WINDOW = 128;     // TX proofs stay strict
                    constexpr uint32_t MAX_UTREEXO_GETDATA_VIOLATIONS = 10;
                    constexpr uint32_t MAX_GETDATA_ITEMS = 200;

                    if (inv_count > MAX_GETDATA_ITEMS) {
                        g_logger.warning("[Relay] getdata request too large from " + peer_addr +
                                         " (count=" + std::to_string(inv_count) +
                                         ", max=" + std::to_string(MAX_GETDATA_ITEMS) + ")");
                        return;
                    }

                    auto consume_utreexo_getdata_budget = [&](uint32_t inv_type) -> bool {
                        bool over_budget = false;
                        uint32_t violations = 0;
                        {
                            std::lock_guard<std::mutex> lock(*utreexo_getdata_budget_mutex);
                            auto& budget = (*utreexo_getdata_budget)[peer_addr];
                            const auto now = std::chrono::steady_clock::now();
                            const bool window_expired =
                                budget.window_start.time_since_epoch().count() == 0 ||
                                now - budget.window_start >= std::chrono::seconds(GETDATA_WINDOW_SECONDS);

                            if (window_expired) {
                                budget.window_start = now;
                                budget.utreexo_block_requests = 0;
                                budget.utreexo_tx_requests = 0;
                                budget.total_utreexo_requests = 0;
                                budget.consecutive_violations = 0;
                            }

                            uint32_t* type_counter =
                                (inv_type == MSG_UTREEXO_BLOCK)
                                    ? &budget.utreexo_block_requests
                                    : &budget.utreexo_tx_requests;
                            const uint32_t type_limit =
                                (inv_type == MSG_UTREEXO_BLOCK)
                                    ? MAX_UTREEXO_BLOCK_GETDATA_PER_WINDOW
                                    : MAX_UTREEXO_TX_GETDATA_PER_WINDOW;

                            if (*type_counter + 1 > type_limit ||
                                budget.total_utreexo_requests + 1 > MAX_UTREEXO_GETDATA_PER_WINDOW) {
                                budget.consecutive_violations++;
                                violations = budget.consecutive_violations;
                                over_budget = true;
                            } else {
                                (*type_counter)++;
                                budget.total_utreexo_requests++;
                            }
                        }

                        if (over_budget) {
                            const std::string inv_name =
                                (inv_type == MSG_UTREEXO_BLOCK) ? "MSG_UTREEXO_BLOCK" : "MSG_UTREEXO_TX";
                            g_logger.warning("[Relay] getdata rate-limit exceeded for " + inv_name +
                                             " from " + peer_addr +
                                             " (violations=" + std::to_string(violations) + ")");
                            if (violations >= MAX_UTREEXO_GETDATA_VIOLATIONS) {
                                g_logger.warning("[Relay] Disconnecting peer for repeated getdata abuse: " + peer_addr);
                                p2p_service->get().disconnect_peer(peer_addr);
                            }
                            return false;
                        }

                        return true;
                    };

                    for (size_t i = 0; i < inv_count; ++i) {
                        size_t offset = 1 + i * 36;

                        // Extract inventory type and hash (same wire format as INV)
                        uint32_t inv_type = 0;
                        std::memcpy(&inv_type, &msg.payload[offset], 4);
                        uint256 hash;
                        std::memcpy(hash.data, &msg.payload[offset + 4], 32);

                        g_logger.info("[Relay] Parsed getdata[" + std::to_string(i) + "/" +
                                      std::to_string(inv_count) + "]: type=" +
                                      std::to_string(inv_type) + " hash=" +
                                      hash.GetHex().substr(0, 16) + "...");

                        if (inv_type == MSG_UTREEXO_BLOCK || inv_type == MSG_UTREEXO_TX) {
                            if (!consume_utreexo_getdata_budget(inv_type)) {
                                continue;
                            }
                        }

                    if (inv_type == MSG_TX && tx_relay) {
                        g_logger.info("[TX-RELAY] Received GETDATA for tx " + hash.GetHex());
                        tx_relay->HandleGetData(peer_addr, hash);
                    } else if (inv_type == MSG_UTREEXO_BLOCK) {
                        // ═══════════════════════════════════════════════════════════════
                        // Phase P.2: Serve Utreexo blocks to stateless/mobile clients
                        // ═══════════════════════════════════════════════════════════════
                        // CRITICAL: For MSG_UTREEXO_BLOCK requests, we MUST either:
                        //   1. Send UTXOBLK (block + proof)
                        //   2. Or send NOTFOUND
                        // We must NEVER fall back to sending a regular block, as this
                        // causes state divergence with Utreexo-enabled peers.
                        // ═══════════════════════════════════════════════════════════════
                        g_logger.info("[Relay] Received GETDATA for utreexo block " + hash.GetHex().substr(0, 16) + "...");

                        // Helper to send NOTFOUND for MSG_UTREEXO_BLOCK
                        auto sendUtreexoNotFound = [&]() {
                            std::vector<uint8_t> notfound_payload;
                            notfound_payload.push_back(0x01);  // count = 1
                            // Type = MSG_UTREEXO_BLOCK as uint32_t LE
                            notfound_payload.push_back(MSG_UTREEXO_BLOCK & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_BLOCK >> 8) & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_BLOCK >> 16) & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_BLOCK >> 24) & 0xFF);
                            // Hash (32 bytes)
                            notfound_payload.insert(notfound_payload.end(), hash.begin(), hash.end());

                            ::P2PMessage notfound_msg;
                            notfound_msg.command = "notfound";
                            notfound_msg.payload = notfound_payload;
                            notfound_msg.checksum = 0;
                            p2p_service->get().send_to_peer(peer_addr, notfound_msg);
                            g_logger.info("[Relay] Sent NOTFOUND for utreexo block " + hash.GetHex().substr(0, 16) + "...");
                        };

                        if (!bridge_node) {
                            g_logger.warning("[Relay] No BridgeNode configured - sending NOTFOUND");
                            sendUtreexoNotFound();
                        } else {
                            // Retrieve block from ChainDB via ChainstateService
                            ChainDB* chain_db = chainstate_service ? chainstate_service->GetChainDB() : nullptr;
                            if (!chain_db) {
                                g_logger.error("[Relay] No ChainDB available - sending NOTFOUND");
                                sendUtreexoNotFound();
                            } else {
                                auto block_result = chainstate_service->getBlockByHash(hash);
                                if (block_result.status() != Status::Ok) {
                                    auto height_result = chain_db->getBlockHeight(hash);
                                    if (height_result.ok() && prune_for_getdata &&
                                        !prune_for_getdata->canServeBlock(static_cast<uint32_t>(height_result.value()))) {
                                        g_logger.warning("[Relay] Historical utreexo block pruned at height " +
                                                         std::to_string(height_result.value()) + ": " +
                                                         hash.GetHex().substr(0, 16) + "...");
                                    } else {
                                        g_logger.warning("[Relay] Block not found for utreexo getdata: " +
                                                         hash.GetHex().substr(0, 16) + "...");
                                    }
                                    sendUtreexoNotFound();
                                } else {
                                    const Block block = block_result.value();
                                    const bool block_has_spends = std::any_of(
                                        block.vtx.begin(),
                                        block.vtx.end(),
                                        [](const Transaction& tx) { return !tx.IsCoinbase(); });

                                    auto build_proof_message_from_stored_block =
                                        [&](UtreexoProofMessage& out_msg) -> bool {
                                            if (!block.utreexo.has_value()) {
                                                return false;
                                            }

                                            // For spend blocks, persisted proof data must be non-empty.
                                            // Coinbase-only blocks may carry an empty proof bundle.
                                            if (block_has_spends && block.utreexo->isEmpty()) {
                                                return false;
                                            }

                                            auto height_result = chain_db->getBlockHeight(hash);
                                            if (!height_result.ok()) {
                                                g_logger.warning("[Relay] Missing height index for stored-proof utreexo block " +
                                                                 hash.GetHex().substr(0, 16) + "...");
                                                return false;
                                            }

                                            out_msg.block_hash = hash;
                                            out_msg.block_height = static_cast<uint32_t>(height_result.value());
                                            out_msg.proof_data = *block.utreexo;
                                            out_msg.accumulator_root_after.assign(
                                                block.header.utreexo_root.data,
                                                block.header.utreexo_root.data + 32
                                            );

                                            consensus::UtreexoHash canonical_root_before;
                                            if (block.header.prev_block_hash.IsNull()) {
                                                canonical_root_before.assign(32, 0x00);
                                            } else {
                                                auto prev_result = chainstate_service->getBlockByHash(block.header.prev_block_hash);
                                                if (!prev_result.ok()) {
                                                    g_logger.warning("[Relay] Missing previous block for stored-proof utreexo block " +
                                                                     hash.GetHex().substr(0, 16) + "...");
                                                    return false;
                                                }
                                                const Block& prev_block = prev_result.value();
                                                canonical_root_before.assign(
                                                    prev_block.header.utreexo_root.data,
                                                    prev_block.header.utreexo_root.data + 32
                                                );
                                            }

                                            if (!out_msg.proof_data.accumulator_root_before.empty() &&
                                                out_msg.proof_data.accumulator_root_before != canonical_root_before) {
                                                g_logger.warning("[Relay] Rewriting stale persisted root_before metadata for block " +
                                                                 hash.GetHex().substr(0, 16) + "...");
                                            }
                                            out_msg.proof_data.accumulator_root_before = canonical_root_before;
                                            out_msg.accumulator_root_before = canonical_root_before;

                                            g_logger.info("[Relay] Serving persisted utreexo payload for block " +
                                                          hash.GetHex().substr(0, 16) + "...");
                                            return true;
                                        };

                                    // Route through bridge proof engine to leverage queueing,
                                    // request coalescing, and cache freshness checks.
                                    GetUtreexoProofMessage proof_req;
                                    proof_req.block_hashes.push_back(hash);
                                    auto proof_request_result = bridge_node->HandleProofRequest(
                                        proof_req,
                                        [chainstate_service](const uint256& requested_hash) -> std::optional<Block> {
                                            auto requested_block = chainstate_service->getBlockByHash(requested_hash);
                                            if (requested_block.status() != Status::Ok) {
                                                return std::nullopt;
                                            }
                                            return requested_block.value();
                                        });

                                    std::optional<consensus::UtreexoTransitionProof> transition_proof;
                                    auto load_transition_proof = [&]() -> bool {
                                        auto tp_opt = bridge_node->GetTransitionProof(hash);
                                        if (!tp_opt && chain_db) {
                                            auto height_result = chain_db->getBlockHeight(hash);
                                            if (height_result.ok()) {
                                                auto tp_db = chain_db->getTransitionProof(static_cast<int>(height_result.value()));
                                                if (tp_db.status() == Status::Ok) {
                                                    try {
                                                        tp_opt = consensus::UtreexoTransitionProof::deserialize(tp_db.value());
                                                    } catch (...) {
                                                        tp_opt.reset();
                                                    }
                                                }
                                            }
                                        }
                                        if (tp_opt && !tp_opt->isEmpty()) {
                                            transition_proof = std::move(tp_opt);
                                            return true;
                                        }
                                        return false;
                                    };

                                    UtreexoProofMessage proof_msg;
                                    bool served_transition_only = false;
                                    if (build_proof_message_from_stored_block(proof_msg)) {
                                        // Prefer persisted proof payloads when available. They
                                        // avoid historical regeneration entirely and exactly match
                                        // the proof that originally validated this block.
                                    } else if (!proof_request_result.proofs.empty()) {
                                        proof_msg = proof_request_result.proofs.front();
                                    } else if (load_transition_proof()) {
                                        auto height_result = chain_db->getBlockHeight(hash);
                                        if (!height_result.ok()) {
                                            g_logger.warning("[Relay] Missing height index for TP-only utreexo block " +
                                                             hash.GetHex().substr(0, 16) + "...");
                                            sendUtreexoNotFound();
                                            continue;
                                        }

                                        proof_msg.block_hash = hash;
                                        proof_msg.block_height = static_cast<uint32_t>(height_result.value());
                                        proof_msg.accumulator_root_after.assign(
                                            block.header.utreexo_root.data,
                                            block.header.utreexo_root.data + 32
                                        );
                                        proof_msg.proof_data = consensus::BlockUtreexoData();
                                        served_transition_only = true;
                                        g_logger.info("[Relay] Serving TP-only historical utreexo block " +
                                                      hash.GetHex().substr(0, 16) + "...");
                                    } else {
                                        g_logger.warning("[Relay] Bridge proof engine returned no proof for " +
                                                         hash.GetHex().substr(0, 16) + "...");
                                        sendUtreexoNotFound();
                                        continue;
                                    }

                                    const uint32_t block_height = proof_msg.block_height;
                                    const auto& proof_data = proof_msg.proof_data;

                                    // Serialize utxoblk v3 message:
                                    // [1 byte]   version = 0x03
                                    // [32 bytes] block_hash
                                    // [4 bytes]  block_height (uint32 LE)
                                    // [32 bytes] accumulator_root_after
                                    // [4 bytes]  block_size (uint32 LE)
                                    // [N bytes]  block_data
                                    // [4 bytes]  proof_size (uint32 LE)
                                    // [M bytes]  proof_data
                                    // [4 bytes]  tp_size (uint32 LE)   ← v3
                                    // [T bytes]  transition_proof      ← v3
                                    std::string block_serialized = block.Serialize();
                                    uint32_t block_size = static_cast<uint32_t>(block_serialized.size());
                                    // TP-only historical fallback intentionally carries no batch
                                    // proof payload. Older bridges encoded an "empty" batch proof
                                    // as 32 zero root bytes + zero deletions, which the CSN
                                    // could misclassify as batch mode and reject at root_before.
                                    std::vector<uint8_t> proof_serialized =
                                        served_transition_only ? std::vector<uint8_t>() : proof_data.serialize();
                                    uint32_t proof_size = static_cast<uint32_t>(proof_serialized.size());

                                    std::vector<uint8_t> payload;
                                    payload.reserve(1 + 32 + 4 + 32 + 4 + block_size + 4 + proof_size + 4 + 512 * 1024);

                                    // Version (1 byte)
                                    payload.push_back(0x03);

                                    // Block hash (32 bytes)
                                    payload.insert(payload.end(), hash.begin(), hash.end());

                                    // Block height (4 bytes LE)
                                    payload.push_back(block_height & 0xFF);
                                    payload.push_back((block_height >> 8) & 0xFF);
                                    payload.push_back((block_height >> 16) & 0xFF);
                                    payload.push_back((block_height >> 24) & 0xFF);

                                    // Accumulator root after (32 bytes)
                                    payload.insert(payload.end(),
                                                   proof_msg.accumulator_root_after.begin(),
                                                   proof_msg.accumulator_root_after.end());

                                    // Block size (4 bytes LE)
                                    payload.push_back(block_size & 0xFF);
                                    payload.push_back((block_size >> 8) & 0xFF);
                                    payload.push_back((block_size >> 16) & 0xFF);
                                    payload.push_back((block_size >> 24) & 0xFF);

                                    // Block data
                                    payload.insert(payload.end(), block_serialized.begin(), block_serialized.end());

                                    // Proof size (4 bytes LE)
                                    payload.push_back(proof_size & 0xFF);
                                    payload.push_back((proof_size >> 8) & 0xFF);
                                    payload.push_back((proof_size >> 16) & 0xFF);
                                    payload.push_back((proof_size >> 24) & 0xFF);

                                    // Proof data
                                    payload.insert(payload.end(), proof_serialized.begin(), proof_serialized.end());

                                    // v3: Append transition proof
                                    {
                                        std::vector<uint8_t> tp_bytes;
                                        // Try cache first, then ChainDB
                                        if (!transition_proof.has_value()) {
                                            load_transition_proof();
                                        }
                                        if (transition_proof.has_value()) {
                                            tp_bytes = transition_proof->serialize();
                                        }
                                        uint32_t tp_size = static_cast<uint32_t>(tp_bytes.size());
                                        payload.push_back(tp_size & 0xFF);
                                        payload.push_back((tp_size >> 8) & 0xFF);
                                        payload.push_back((tp_size >> 16) & 0xFF);
                                        payload.push_back((tp_size >> 24) & 0xFF);
                                        if (tp_size > 0) {
                                            payload.insert(payload.end(), tp_bytes.begin(), tp_bytes.end());
                                        }
                                    }

                                    // Send utxoblk message
                                    ::P2PMessage utxoblk_msg;
                                    utxoblk_msg.command = "utxoblk";
                                    utxoblk_msg.payload = payload;
                                    utxoblk_msg.checksum = 0;  // P2PManager will calculate
                                    p2p_service->get().send_to_peer(peer_addr, utxoblk_msg);

                                    // Phase 9.3: Announce recent proof availability (best effort).
                                    if (proof_gossip && !proof_data.isEmpty()) {
                                        bool should_announce = false;
                                        {
                                            std::lock_guard<std::mutex> lock(*announced_proof_mutex);
                                            if (announced_proof_hashes->insert(hash).second) {
                                                announced_proof_order->push_back(hash);
                                                should_announce = true;

                                                constexpr size_t MAX_ANNOUNCED_PROOF_INV = 2048;
                                                while (announced_proof_order->size() > MAX_ANNOUNCED_PROOF_INV) {
                                                    const uint256 evicted = announced_proof_order->front();
                                                    announced_proof_order->pop_front();
                                                    announced_proof_hashes->erase(evicted);
                                                }
                                            }
                                        }

                                        if (should_announce) {
                                            const auto inv = proof_gossip->AnnounceProof(hash, proof_data);
                                            ::P2PMessage inv_msg;
                                            inv_msg.command = MessageCommands::INVPROOF;
                                            inv_msg.payload = inv.Serialize();
                                            inv_msg.checksum = 0;
                                            p2p_service->get().broadcast_message_async(inv_msg);
                                        }
                                    }

                                    // Diagnostic: log roots being sent for debugging
                                    {
                                        auto& rb = proof_data.accumulator_root_before;
                                        std::string rb_hex, ra_hex;
                                        for (size_t i = 0; i < std::min(rb.size(), static_cast<size_t>(8)); i++) {
                                            char buf[3];
                                            snprintf(buf, sizeof(buf), "%02x", rb[i]);
                                            rb_hex += buf;
                                        }
                                        for (size_t i = 0; i < std::min(proof_msg.accumulator_root_after.size(),
                                                                        static_cast<size_t>(8)); i++) {
                                            char buf[3];
                                            snprintf(buf, sizeof(buf), "%02x", proof_msg.accumulator_root_after[i]);
                                            ra_hex += buf;
                                        }
                                        g_logger.info("[Relay] utxoblk height=" + std::to_string(block_height) +
                                                      " root_before=" + rb_hex + "..." +
                                                      " root_after=" + ra_hex + "..." +
                                                      " dels=" + std::to_string(proof_data.spend_proof.targets.size()) +
                                                      " spent_outs=" + std::to_string(proof_data.spent_outputs.size()));
                                    }
                                    g_logger.info("[Relay] Sent utreexo block " + hash.GetHex().substr(0, 16) +
                                                  "... to " + peer_addr +
                                                  " (block=" + std::to_string(block_size) +
                                                  " proof=" + std::to_string(proof_size) + " bytes)");
                                }
                            }
                        }
                    } else if (inv_type == MSG_UTREEXO_TX && mempool_for_getdata) {
                        // ═══════════════════════════════════════════════════════════════
                        // Phase #4: Serve Utreexo tx to CSN peers (tx + per-input proofs)
                        // Bridge nodes generate fresh proofs; CSNs serve cached payloads
                        // ═══════════════════════════════════════════════════════════════
                        g_logger.info("[TX-RELAY] Received GETDATA for utreexo tx " + hash.GetHex().substr(0, 16) + "...");

                        // Helper: send notfound for MSG_UTREEXO_TX
                        auto sendUtreexoTxNotFound = [&]() {
                            std::vector<uint8_t> notfound_payload;
                            notfound_payload.push_back(0x01);
                            notfound_payload.push_back(MSG_UTREEXO_TX & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_TX >> 8) & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_TX >> 16) & 0xFF);
                            notfound_payload.push_back((MSG_UTREEXO_TX >> 24) & 0xFF);
                            notfound_payload.insert(notfound_payload.end(), hash.begin(), hash.end());
                            ::P2PMessage nf_msg;
                            nf_msg.command = "notfound";
                            nf_msg.payload = notfound_payload;
                            nf_msg.checksum = 0;
                            p2p_service->get().send_to_peer(peer_addr, nf_msg);
                        };

                        if (bridge_node) {
                            // BRIDGE PATH: Generate fresh proofs from the forest
                            auto tx_ptr = mempool_for_getdata->getTransaction(hash);
                            if (!tx_ptr) {
                                g_logger.debug("[TX-RELAY] TX not in mempool: " + hash.GetHex().substr(0, 16));
                                sendUtreexoTxNotFound();
                            } else {
                                const Transaction& tx = *tx_ptr;

                                // 2. Generate per-input proofs
                                auto proofs_opt = bridge_node->GenerateProofsForTransaction(tx);
                                if (!proofs_opt.has_value()) {
                                    g_logger.warning("[TX-RELAY] Failed to generate utreexo proofs for tx " +
                                                    hash.GetHex().substr(0, 16));
                                    sendUtreexoTxNotFound();
                                } else {
                                    const auto& proofs = proofs_opt.value();

                                    // 3. Get current accumulator root
                                    auto acc_root = bridge_node->GetCurrentForestCommitment();

                                    // 4. Serialize utxotx wire format
                                    std::vector<uint8_t> tx_serialized = tx.Serialize();
                                    uint32_t tx_size = static_cast<uint32_t>(tx_serialized.size());
                                    uint32_t num_proofs = static_cast<uint32_t>(proofs.size());

                                    std::vector<uint8_t> payload;
                                    payload.reserve(1 + 32 + 4 + tx_size + 4 + num_proofs * 256 + 32);

                                    // Version (1 byte)
                                    payload.push_back(0x01);

                                    // Txid (32 bytes)
                                    payload.insert(payload.end(), hash.begin(), hash.end());

                                    // TX size (4 bytes LE)
                                    payload.push_back(tx_size & 0xFF);
                                    payload.push_back((tx_size >> 8) & 0xFF);
                                    payload.push_back((tx_size >> 16) & 0xFF);
                                    payload.push_back((tx_size >> 24) & 0xFF);

                                    // TX data
                                    payload.insert(payload.end(), tx_serialized.begin(), tx_serialized.end());

                                    // Num proofs (4 bytes LE)
                                    payload.push_back(num_proofs & 0xFF);
                                    payload.push_back((num_proofs >> 8) & 0xFF);
                                    payload.push_back((num_proofs >> 16) & 0xFF);
                                    payload.push_back((num_proofs >> 24) & 0xFF);

                                    // Per-input proofs
                                    for (const auto& [proof, spent] : proofs) {
                                        auto proof_bytes = proof.serialize();
                                        uint32_t proof_size = static_cast<uint32_t>(proof_bytes.size());

                                        // Proof size (4 bytes LE)
                                        payload.push_back(proof_size & 0xFF);
                                        payload.push_back((proof_size >> 8) & 0xFF);
                                        payload.push_back((proof_size >> 16) & 0xFF);
                                        payload.push_back((proof_size >> 24) & 0xFF);

                                        // Proof data
                                        payload.insert(payload.end(), proof_bytes.begin(), proof_bytes.end());

                                        // Value (8 bytes LE)
                                        uint64_t value = spent.value;
                                        for (int b = 0; b < 8; b++)
                                            payload.push_back((value >> (b * 8)) & 0xFF);

                                        // Script size (4 bytes LE)
                                        uint32_t script_size = static_cast<uint32_t>(spent.scriptPubKey.size());
                                        payload.push_back(script_size & 0xFF);
                                        payload.push_back((script_size >> 8) & 0xFF);
                                        payload.push_back((script_size >> 16) & 0xFF);
                                        payload.push_back((script_size >> 24) & 0xFF);

                                        // ScriptPubKey
                                        payload.insert(payload.end(), spent.scriptPubKey.begin(), spent.scriptPubKey.end());
                                    }

                                    // Accumulator root (32 bytes)
                                    payload.insert(payload.end(), acc_root.begin(), acc_root.end());

                                    // 5. Send utxotx message
                                    ::P2PMessage utxotx_msg;
                                    utxotx_msg.command = "utxotx";
                                    utxotx_msg.payload = std::move(payload);
                                    utxotx_msg.checksum = 0;
                                    p2p_service->get().send_to_peer(peer_addr, utxotx_msg);

                                    g_logger.info("[TX-RELAY] Sent utxotx " + hash.GetHex().substr(0, 16) +
                                                 "... to " + peer_addr + " (" + std::to_string(num_proofs) + " proofs)");
                                }
                            }
                        } else {
                            // CSN PATH: Serve cached utxotx payload from mempool
                            auto cached = mempool_for_getdata->mempool().getCachedUtxoTxPayload(hash);
                            if (!cached.has_value()) {
                                g_logger.debug("[TX-RELAY] No cached utxotx for " + hash.GetHex().substr(0, 16) +
                                              " (not found or stale)");
                                sendUtreexoTxNotFound();
                            } else {
                                ::P2PMessage utxotx_msg;
                                utxotx_msg.command = "utxotx";
                                utxotx_msg.payload = std::move(*cached);
                                utxotx_msg.checksum = 0;
                                p2p_service->get().send_to_peer(peer_addr, utxotx_msg);
                                g_logger.info("[TX-RELAY] Served cached utxotx " + hash.GetHex().substr(0, 16) +
                                             "... to " + peer_addr);
                            }
                        }
                    } else if (inv_type == MSG_BLOCK && block_relay) {
                        // Regular block request
                        g_logger.info("[Relay] Routing block getdata to BlockRelayManager");
                        block_relay->HandleGetData(peer_addr, hash);
                    } else {
                        g_logger.debug("[Relay] Unknown getdata type " + std::to_string(inv_type) + " from " + peer_addr);
                    }
                    }
                };

                std::cout << "[DaemonApp] ✅ Phase G.2/G.3 relay handlers wired (OnInv, OnGetData)" << std::endl;

                // Phase 7.4: Wire proof-serving commands into active P2PService path.
                // This keeps bridge-node proof serving on the active P2P service path.
                struct ProofServingPeerBudget {
                    std::chrono::steady_clock::time_point window_start{};
                    uint32_t proof_requests{0};
                    uint32_t proof_hashes{0};
                    uint32_t header_requests{0};
                    uint32_t locator_hashes{0};
                    uint32_t consecutive_violations{0};
                };
                auto proof_serving_budget_mutex = std::make_shared<std::mutex>();
                auto proof_serving_budget = std::make_shared<std::unordered_map<std::string, ProofServingPeerBudget>>();

                // Gossip proof serving (getproof/proofdata) has separate pressure patterns from
                // getutxoproof/getutxohdrs, so it gets its own per-peer budget.
                struct ProofGossipPeerBudget {
                    std::chrono::steady_clock::time_point window_start{};
                    uint32_t getproof_requests{0};
                    uint32_t proofdata_messages{0};
                    uint64_t proofdata_bytes{0};
                    uint32_t consecutive_violations{0};
                };
                auto proof_gossip_budget_mutex = std::make_shared<std::mutex>();
                auto proof_gossip_budget = std::make_shared<std::unordered_map<std::string, ProofGossipPeerBudget>>();

                // Phase 9.3: Proof gossip protocol handlers (best-effort proof availability).
                auto peer_id_from_addr = [](const std::string& addr) -> uint64_t {
                    return static_cast<uint64_t>(std::hash<std::string>{}(addr));
                };

                p2p_service->OnInvProof = [proof_gossip, p2p_service, stateless_node, chainstate_service, peer_id_from_addr](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    if (!proof_gossip) {
                        return;
                    }
                    if (!stateless_node) {
                        // Full nodes don't need to chase gossip proofs.
                        return;
                    }
                    if (msg.payload.size() < 69 || msg.payload.size() > 512) {
                        g_logger.warning("[ProofGossip] Invalid invproof payload size from " + peer_addr +
                                         ": " + std::to_string(msg.payload.size()));
                        return;
                    }

                    const auto inv = consensus::InvProof::Deserialize(msg.payload);
                    if (inv.block_hash.IsNull()) {
                        g_logger.warning("[ProofGossip] invproof missing block hash from " + peer_addr);
                        return;
                    }

                    if (!proof_gossip->HandleInvProof(inv, peer_id_from_addr(peer_addr))) {
                        return;
                    }

                    // Only consume best-effort proof gossip once the CSN is fully synced.
                    // During headers/proofs/blocks sync, proofs already arrive on the primary
                    // utxoblk / scheduled proof paths, and gossip traffic for historical blocks
                    // turns into redundant late responses that can destabilize churn/restart runs.
                    if (stateless_node->GetSyncState() != network::StatelessSyncState::SYNCED) {
                        g_logger.debug("[ProofGossip] Ignoring invproof while CSN sync_state=" +
                                       std::to_string(static_cast<int>(stateless_node->GetSyncState())) +
                                       " for block " + inv.block_hash.GetHex().substr(0, 16) + "...");
                        return;
                    }

                    const auto req = proof_gossip->CreateProofRequest(inv.block_hash);
                    ::P2PMessage request;
                    request.command = MessageCommands::GETPROOF;
                    request.payload = req.Serialize();
                    request.checksum = 0;
                    if (!p2p_service->get().send_to_peer(peer_addr, request)) {
                        g_logger.warning("[ProofGossip] Failed sending getproof to " + peer_addr);
                    } else {
                        stateless_node->TrackExternalProofRequest(inv.block_hash);
                        g_logger.info("[ProofGossip] Requested gossiped proof for block " +
                                      inv.block_hash.GetHex().substr(0, 16) + "... from " + peer_addr);
                    }
                };

                p2p_service->OnGetProof = [proof_gossip, p2p_service, peer_id_from_addr,
                                           proof_gossip_budget_mutex, proof_gossip_budget](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    constexpr uint32_t GETPROOF_WINDOW_SECONDS = 5;
                    constexpr uint32_t MAX_GETPROOF_REQ_PER_WINDOW = 24;
                    constexpr uint32_t MAX_GETPROOF_VIOLATIONS = 4;

                    if (!proof_gossip) {
                        return;
                    }
                    if (!p2p_service->get().peer_has_service_flags(peer_addr, ServiceFlags::NODE_UTREEXO)) {
                        g_logger.warning("[ProofGossip] getproof rejected from non-utreexo peer " + peer_addr);
                        return;
                    }

                    bool over_budget = false;
                    uint32_t violations = 0;
                    {
                        std::lock_guard<std::mutex> lock(*proof_gossip_budget_mutex);
                        auto& budget = (*proof_gossip_budget)[peer_addr];
                        const auto now = std::chrono::steady_clock::now();
                        const bool window_expired =
                            budget.window_start.time_since_epoch().count() == 0 ||
                            now - budget.window_start >= std::chrono::seconds(GETPROOF_WINDOW_SECONDS);

                        if (window_expired) {
                            budget.window_start = now;
                            budget.getproof_requests = 0;
                            budget.proofdata_messages = 0;
                            budget.proofdata_bytes = 0;
                            budget.consecutive_violations = 0;
                        }

                        if (budget.getproof_requests + 1 > MAX_GETPROOF_REQ_PER_WINDOW) {
                            budget.consecutive_violations++;
                            violations = budget.consecutive_violations;
                            over_budget = true;
                        } else {
                            budget.getproof_requests++;
                        }
                    }

                    if (over_budget) {
                        proof_gossip->RecordGetProofRateLimited();
                        g_logger.warning("[ProofGossip] getproof rate-limit exceeded from " + peer_addr +
                                         " (violations=" + std::to_string(violations) + ")");
                        if (violations >= MAX_GETPROOF_VIOLATIONS) {
                            proof_gossip->RecordPeerDisconnectedForAbuse();
                            g_logger.warning("[ProofGossip] Disconnecting peer for repeated getproof abuse: " + peer_addr);
                            p2p_service->get().disconnect_peer(peer_addr);
                        }
                        return;
                    }

                    if (msg.payload.size() != 64) {
                        proof_gossip->RecordInvalidGetProofPayload();
                        g_logger.warning("[ProofGossip] Invalid getproof payload size from " + peer_addr +
                                         ": " + std::to_string(msg.payload.size()));
                        return;
                    }

                    const auto req = consensus::GetProof::Deserialize(msg.payload);
                    if (req.block_hash.IsNull()) {
                        proof_gossip->RecordInvalidGetProofPayload();
                        g_logger.warning("[ProofGossip] getproof missing block hash from " + peer_addr);
                        return;
                    }
                    auto resp = proof_gossip->HandleProofRequest(req, peer_id_from_addr(peer_addr));
                    if (!resp.has_value()) {
                        return;
                    }

                    ::P2PMessage response;
                    response.command = MessageCommands::PROOFDATA;
                    response.payload = resp->Serialize();
                    response.checksum = 0;
                    if (!p2p_service->get().send_to_peer(peer_addr, response)) {
                        g_logger.warning("[ProofGossip] Failed sending proofdata to " + peer_addr);
                    }
                };

                p2p_service->OnProofData = [proof_gossip, stateless_node, chainstate_service, p2p_service,
                                            peer_id_from_addr, proof_gossip_budget_mutex, proof_gossip_budget](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    constexpr uint32_t PROOFDATA_WINDOW_SECONDS = 5;
                    constexpr uint32_t MAX_PROOFDATA_MSG_PER_WINDOW = 64;
                    constexpr uint64_t MAX_PROOFDATA_BYTES_PER_WINDOW = 4ULL * 1024ULL * 1024ULL;
                    constexpr uint32_t MAX_PROOFDATA_VIOLATIONS = 4;

                    if (!proof_gossip) {
                        return;
                    }
                    if (!p2p_service->get().peer_has_service_flags(peer_addr, ServiceFlags::NODE_UTREEXO)) {
                        g_logger.warning("[ProofGossip] proofdata rejected from non-utreexo peer " + peer_addr);
                        return;
                    }

                    bool over_budget = false;
                    uint32_t violations = 0;
                    {
                        std::lock_guard<std::mutex> lock(*proof_gossip_budget_mutex);
                        auto& budget = (*proof_gossip_budget)[peer_addr];
                        const auto now = std::chrono::steady_clock::now();
                        const bool window_expired =
                            budget.window_start.time_since_epoch().count() == 0 ||
                            now - budget.window_start >= std::chrono::seconds(PROOFDATA_WINDOW_SECONDS);

                        if (window_expired) {
                            budget.window_start = now;
                            budget.getproof_requests = 0;
                            budget.proofdata_messages = 0;
                            budget.proofdata_bytes = 0;
                            budget.consecutive_violations = 0;
                        }

                        if (budget.proofdata_messages + 1 > MAX_PROOFDATA_MSG_PER_WINDOW ||
                            budget.proofdata_bytes + msg.payload.size() > MAX_PROOFDATA_BYTES_PER_WINDOW) {
                            budget.consecutive_violations++;
                            violations = budget.consecutive_violations;
                            over_budget = true;
                        } else {
                            budget.proofdata_messages++;
                            budget.proofdata_bytes += msg.payload.size();
                        }
                    }

                    if (over_budget) {
                        proof_gossip->RecordProofDataRateLimited();
                        g_logger.warning("[ProofGossip] proofdata rate-limit exceeded from " + peer_addr +
                                         " (violations=" + std::to_string(violations) + ")");
                        if (violations >= MAX_PROOFDATA_VIOLATIONS) {
                            proof_gossip->RecordPeerDisconnectedForAbuse();
                            g_logger.warning("[ProofGossip] Disconnecting peer for repeated proofdata abuse: " + peer_addr);
                            p2p_service->get().disconnect_peer(peer_addr);
                        }
                        return;
                    }

                    // 32-byte block hash + serialized BlockUtreexoData payload.
                    if (msg.payload.size() < 33 || msg.payload.size() > 12 * 1024 * 1024) {
                        proof_gossip->RecordInvalidProofDataPayload();
                        g_logger.warning("[ProofGossip] Invalid proofdata payload size from " + peer_addr +
                                         ": " + std::to_string(msg.payload.size()));
                        return;
                    }

                    consensus::ProofData data;
                    try {
                        data = consensus::ProofData::Deserialize(msg.payload);
                    } catch (const std::exception& e) {
                        proof_gossip->RecordInvalidProofDataPayload();
                        g_logger.warning("[ProofGossip] Failed to deserialize proofdata from " + peer_addr +
                                         ": " + std::string(e.what()));
                        return;
                    } catch (...) {
                        proof_gossip->RecordInvalidProofDataPayload();
                        g_logger.warning("[ProofGossip] Failed to deserialize proofdata from " + peer_addr);
                        return;
                    }
                    if (data.block_hash.IsNull()) {
                        proof_gossip->RecordInvalidProofDataPayload();
                        g_logger.warning("[ProofGossip] proofdata missing block hash from " + peer_addr);
                        return;
                    }

                    if (!proof_gossip->HandleProofData(data, peer_id_from_addr(peer_addr))) {
                        g_logger.debug("[ProofGossip] Unsolicited proofdata from " + peer_addr +
                                       " block=" + data.block_hash.GetHex().substr(0, 16) + "...");
                        return;
                    }

                    if (!stateless_node || !chainstate_service) {
                        // Full/stateful nodes still meter and track proofdata abuse via
                        // ProofGossipManager stats, but only CSN nodes consume proof payloads.
                        g_logger.debug("[ProofGossip] Expected proofdata received on non-CSN node; ignoring consume path");
                        return;
                    }

                    ChainDB* chain_db = chainstate_service->GetChainDB();
                    if (!chain_db) {
                        g_logger.warning("[ProofGossip] ChainDB unavailable while processing proofdata for block " +
                                         data.block_hash.GetHex().substr(0, 16) + "...");
                        return;
                    }

                    auto block_result = chainstate_service->getBlockByHash(data.block_hash);
                    if (!block_result.ok()) {
                        g_logger.warning("[ProofGossip] Missing block for proofdata " +
                                         data.block_hash.GetHex().substr(0, 16) + "...");
                        return;
                    }

                    auto height_result = chain_db->getBlockHeight(data.block_hash);
                    if (!height_result.ok()) {
                        g_logger.warning("[ProofGossip] Missing block height for proofdata " +
                                         data.block_hash.GetHex().substr(0, 16) + "...");
                        return;
                    }

                    UtreexoProofMessage proof_msg;
                    proof_msg.block_hash = data.block_hash;
                    proof_msg.block_height = static_cast<uint32_t>(height_result.value());
                    proof_msg.accumulator_root_before = data.proof.accumulator_root_before;
                    proof_msg.accumulator_root_after.assign(
                        block_result.value().header.utreexo_root.begin(),
                        block_result.value().header.utreexo_root.end()
                    );
                    proof_msg.proof_data = data.proof;

                    if (!stateless_node->onProofResponse(peer_addr, proof_msg)) {
                        g_logger.warning("[ProofGossip] CSN rejected proofdata for block " +
                                         data.block_hash.GetHex().substr(0, 16) + "... from " + peer_addr);
                        return;
                    }

                    g_logger.info("[ProofGossip] Received proofdata for block " +
                                  data.block_hash.GetHex().substr(0, 16) + "... from " + peer_addr);
                };

                p2p_service->OnGetUtreexoProof = [bridge_node, chainstate_service, p2p_service,
                                                  proof_serving_budget_mutex, proof_serving_budget](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    constexpr uint32_t PROOF_REQ_WINDOW_SECONDS = 5;
                    constexpr uint32_t MAX_GETUTXOPROOF_REQ_PER_WINDOW = 8;
                    constexpr uint32_t MAX_BLOCK_HASHES_PER_WINDOW = 64;
                    constexpr uint32_t MAX_PROOF_SERVING_VIOLATIONS = 3;

                    if (!bridge_node || !chainstate_service) {
                        g_logger.warning("[Bridge] getutxoproof ignored: bridge node not available");
                        return;
                    }

                    if (!p2p_service->get().peer_has_service_flags(peer_addr, ServiceFlags::NODE_UTREEXO)) {
                        g_logger.warning("[Bridge] getutxoproof rejected from non-utreexo peer " + peer_addr);
                        return;
                    }

                    GetUtreexoProofMessage request;
                    if (!request.deserialize(msg.payload) || !request.isValid()) {
                        g_logger.warning("[Bridge] Invalid getutxoproof request from " + peer_addr);
                        return;
                    }

                    // Deduplicate hashes in-request to reduce avoidable proof generation work.
                    GetUtreexoProofMessage deduped_request;
                    deduped_request.flags = request.flags;
                    deduped_request.block_hashes.reserve(request.block_hashes.size());
                    std::unordered_set<uint256> seen_hashes;
                    seen_hashes.reserve(request.block_hashes.size());
                    for (const auto& hash : request.block_hashes) {
                        if (seen_hashes.insert(hash).second) {
                            deduped_request.block_hashes.push_back(hash);
                        }
                    }
                    if (deduped_request.block_hashes.empty()) {
                        g_logger.warning("[Bridge] Empty getutxoproof request after dedupe from " + peer_addr);
                        return;
                    }
                    if (deduped_request.block_hashes.size() != request.block_hashes.size()) {
                        g_logger.warning("[Bridge] getutxoproof duplicate hashes from " + peer_addr +
                                         " (requested=" + std::to_string(request.block_hashes.size()) +
                                         " unique=" + std::to_string(deduped_request.block_hashes.size()) + ")");
                    }

                    // Per-peer serving budget to cap CPU-heavy proof generation requests.
                    bool over_budget = false;
                    uint32_t violations = 0;
                    {
                        std::lock_guard<std::mutex> lock(*proof_serving_budget_mutex);
                        auto& budget = (*proof_serving_budget)[peer_addr];
                        const auto now = std::chrono::steady_clock::now();
                        const bool window_expired =
                            budget.window_start.time_since_epoch().count() == 0 ||
                            now - budget.window_start >= std::chrono::seconds(PROOF_REQ_WINDOW_SECONDS);

                        if (window_expired) {
                            budget.window_start = now;
                            budget.proof_requests = 0;
                            budget.proof_hashes = 0;
                            budget.header_requests = 0;
                            budget.locator_hashes = 0;
                            budget.consecutive_violations = 0;
                        }

                        const uint32_t req_hashes = static_cast<uint32_t>(deduped_request.block_hashes.size());
                        if (budget.proof_requests + 1 > MAX_GETUTXOPROOF_REQ_PER_WINDOW ||
                            budget.proof_hashes + req_hashes > MAX_BLOCK_HASHES_PER_WINDOW) {
                            budget.consecutive_violations++;
                            violations = budget.consecutive_violations;
                            over_budget = true;
                        } else {
                            budget.proof_requests += 1;
                            budget.proof_hashes += req_hashes;
                        }
                    }

                    if (over_budget) {
                        g_logger.warning("[Bridge] getutxoproof rate-limit exceeded from " + peer_addr +
                                         " (violations=" + std::to_string(violations) + ")");
                        if (violations >= MAX_PROOF_SERVING_VIOLATIONS) {
                            g_logger.warning("[Bridge] Disconnecting peer for repeated getutxoproof abuse: " + peer_addr);
                            p2p_service->get().disconnect_peer(peer_addr);
                        }
                        return;
                    }

                    auto block_provider = [chainstate_service](const uint256& block_hash) -> std::optional<Block> {
                        auto block_result = chainstate_service->getBlockByHash(block_hash);
                        if (!block_result.ok()) {
                            return std::nullopt;
                        }
                        return block_result.value();
                    };

                    network::BridgeNode::ProofRequestResult proof_result;
                    try {
                        proof_result = bridge_node->HandleProofRequest(deduped_request, block_provider);
                    } catch (const std::exception& e) {
                        g_logger.error("[Bridge] Failed getutxoproof handling for " + peer_addr +
                                       ": " + std::string(e.what()));
                        return;
                    }

                    const bool request_plural =
                        (msg.command == MessageCommands::GETUTREEXOPROOFS);
                    const std::string response_command =
                        request_plural ? MessageCommands::UTREEXOPROOFS : MessageCommands::UTREEXOPROOF;

                    for (const auto& proof : proof_result.proofs) {
                        ::P2PMessage response;
                        response.command = response_command;
                        response.payload = proof.serialize();
                        response.checksum = 0;  // P2PManager computes checksum
                        if (!p2p_service->get().send_to_peer(peer_addr, response)) {
                            g_logger.warning("[Bridge] Failed sending " + response_command + " to " + peer_addr);
                            return;
                        }
                    }

                    // Send NACK for hashes rejected due to queue backpressure.
                    // This tells the peer to back off and retry after a delay.
                    if (!proof_result.backpressure_rejected.empty()) {
                        UtreexoProofNackMessage nack;
                        nack.reason = ProofNackReason::QUEUE_FULL;
                        nack.retry_after_ms = 5000;  // 5 seconds suggested delay
                        nack.block_hashes = std::move(proof_result.backpressure_rejected);

                        ::P2PMessage nack_msg;
                        nack_msg.command = MessageCommands::UTREEXOPROOF_NACK;
                        nack_msg.payload = nack.serialize();
                        nack_msg.checksum = 0;
                        if (!p2p_service->get().send_to_peer(peer_addr, nack_msg)) {
                            g_logger.warning("[Bridge] Failed sending utxoproofnack to " + peer_addr);
                        } else {
                            g_logger.info("[Bridge] Sent NACK for " +
                                          std::to_string(nack.block_hashes.size()) +
                                          " queue-full hash(es) to " + peer_addr);
                        }
                    }

                    g_logger.info("[Bridge] Served " + std::to_string(proof_result.proofs.size()) +
                                  " " + response_command + " message(s) to " + peer_addr);
                };

                p2p_service->OnGetUtreexoHeaders = [bridge_node, chainstate_service, p2p_service,
                                                    proof_serving_budget_mutex, proof_serving_budget](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    constexpr uint32_t PROOF_REQ_WINDOW_SECONDS = 5;
                    constexpr uint32_t MAX_GETUTXOHDRS_REQ_PER_WINDOW = 4;
                    constexpr uint32_t MAX_LOCATOR_HASHES_PER_WINDOW = 200;
                    constexpr uint32_t MAX_PROOF_SERVING_VIOLATIONS = 3;

                    if (!bridge_node || !chainstate_service) {
                        g_logger.warning("[Bridge] getutxohdrs ignored: bridge node not available");
                        return;
                    }

                    if (!p2p_service->get().peer_has_service_flags(peer_addr, ServiceFlags::NODE_UTREEXO)) {
                        g_logger.warning("[Bridge] getutxohdrs rejected from non-utreexo peer " + peer_addr);
                        return;
                    }

                    GetUtreexoHeadersMessage request;
                    if (!request.deserialize(msg.payload) || !request.isValid()) {
                        g_logger.warning("[Bridge] Invalid getutxohdrs request from " + peer_addr);
                        return;
                    }

                    // Per-peer serving budget for header requests (prevents locator spam).
                    bool over_budget = false;
                    uint32_t violations = 0;
                    {
                        std::lock_guard<std::mutex> lock(*proof_serving_budget_mutex);
                        auto& budget = (*proof_serving_budget)[peer_addr];
                        const auto now = std::chrono::steady_clock::now();
                        const bool window_expired =
                            budget.window_start.time_since_epoch().count() == 0 ||
                            now - budget.window_start >= std::chrono::seconds(PROOF_REQ_WINDOW_SECONDS);

                        if (window_expired) {
                            budget.window_start = now;
                            budget.proof_requests = 0;
                            budget.proof_hashes = 0;
                            budget.header_requests = 0;
                            budget.locator_hashes = 0;
                            budget.consecutive_violations = 0;
                        }

                        const uint32_t locator_count = static_cast<uint32_t>(request.locator_hashes.size());
                        if (budget.header_requests + 1 > MAX_GETUTXOHDRS_REQ_PER_WINDOW ||
                            budget.locator_hashes + locator_count > MAX_LOCATOR_HASHES_PER_WINDOW) {
                            budget.consecutive_violations++;
                            violations = budget.consecutive_violations;
                            over_budget = true;
                        } else {
                            budget.header_requests += 1;
                            budget.locator_hashes += locator_count;
                        }
                    }

                    if (over_budget) {
                        g_logger.warning("[Bridge] getutxohdrs rate-limit exceeded from " + peer_addr +
                                         " (violations=" + std::to_string(violations) + ")");
                        if (violations >= MAX_PROOF_SERVING_VIOLATIONS) {
                            g_logger.warning("[Bridge] Disconnecting peer for repeated getutxohdrs abuse: " + peer_addr);
                            p2p_service->get().disconnect_peer(peer_addr);
                        }
                        return;
                    }

                    auto header_provider = [chainstate_service](const uint256& hash) -> std::optional<BlockHeader> {
                        ChainDB* chain_db = chainstate_service->GetChainDB();
                        if (!chain_db) {
                            return std::nullopt;
                        }
                        auto header_result = chain_db->getHeader(hash);
                        if (!header_result.ok()) {
                            return std::nullopt;
                        }
                        return header_result.value();
                    };

                    auto header_by_height_provider = [chainstate_service](uint32_t height) -> std::optional<BlockHeader> {
                        ChainDB* chain_db = chainstate_service->GetChainDB();
                        if (!chain_db) {
                            return std::nullopt;
                        }
                        auto hash_result = chain_db->getBlockHashByHeight(static_cast<int>(height));
                        if (!hash_result.ok()) {
                            return std::nullopt;
                        }
                        auto header_result = chain_db->getHeader(hash_result.value());
                        if (!header_result.ok()) {
                            return std::nullopt;
                        }
                        return header_result.value();
                    };

                    UtreexoHeadersMessage headers;
                    try {
                        headers = bridge_node->HandleHeadersRequest(
                            request,
                            header_provider,
                            header_by_height_provider
                        );
                    } catch (const std::exception& e) {
                        g_logger.error("[Bridge] Failed getutxohdrs handling for " + peer_addr +
                                       ": " + std::string(e.what()));
                        return;
                    }

                    ::P2PMessage response;
                    response.command = "utxohdrs";
                    response.payload = headers.serialize();
                    response.checksum = 0;  // P2PManager computes checksum
                    if (!p2p_service->get().send_to_peer(peer_addr, response)) {
                        g_logger.warning("[Bridge] Failed sending utxohdrs to " + peer_addr);
                        return;
                    }

                    g_logger.info("[Bridge] Served utxohdrs (" +
                                  std::to_string(headers.headers.size()) +
                                  " headers) to " + peer_addr);
                };

                // Phase P.3: OnUtxoBlock — CSN receives block + utreexo proof combined
                // With windowed IBD, blocks arrive out of order. Buffer and validate sequentially.
                if (stateless_node) {
                    auto block_download_for_csn = ctx_.block_download;
                    auto parallel_download_for_csn = ctx_.parallel_block_download;

                    // Reorder buffer: blocks arrive out of order, validate in height order
                    struct PendingUtxoBlock {
                        Block block;
                        UtreexoProofMessage proof_msg;
                        std::string peer_addr;
                        std::optional<consensus::UtreexoTransitionProof> transition_proof;
                    };
                    auto pending_blocks = std::make_shared<std::map<uint32_t, PendingUtxoBlock>>();
                    auto next_validate_height = std::make_shared<uint32_t>(
                        block_download_for_csn ? block_download_for_csn->GetLocalTipHeight() + 1 : 1
                    );
                    auto buffer_mutex = std::make_shared<std::mutex>();
                    auto pending_count_for_scheduler = std::make_shared<std::atomic<size_t>>(0);
                    auto frontier_refresh_height = std::make_shared<uint32_t>(0);
                    // Retry budget: prevent infinite re-request loop on persistent proof failure
                    auto retry_counts = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
                    constexpr uint32_t MAX_RETRIES_PER_HEIGHT = 3;
                    auto header_chain_for_csn = ctx_.header_chain;
                    auto p2p_service_for_csn = p2p_service;

                    // Global scheduler Tick() runs in multiple places; account for
                    // CSN pending-buffer occupancy in every request decision.
                    if (block_download_for_csn) {
                        block_download_for_csn->SetExternalBackpressureCallback(
                            [pending_count_for_scheduler]() -> size_t {
                                return pending_count_for_scheduler->load(std::memory_order_relaxed);
                            }
                        );
                    }

                    p2p_service->OnUtxoBlock = [stateless_node, chainstate_service, block_relay,
                                                 block_download_for_csn, parallel_download_for_csn,
                                                 pending_blocks, header_chain_for_csn,
                                                 p2p_service_for_csn, frontier_refresh_height,
                                                 next_validate_height, buffer_mutex,
                                                 pending_count_for_scheduler, retry_counts](
                        const std::string& peer_addr,
                        const ::P2PMessage& msg
                    ) {
                        const auto& payload = msg.payload;

                        // ── Hardening: payload size bounds ──
                        constexpr size_t MAX_UTXOBLK_SIZE  = 5 * 1024 * 1024;   // 5 MB total
                        constexpr uint32_t MAX_BLOCK_BYTES = 4 * 1024 * 1024;   // 4 MB block
                        constexpr uint32_t MAX_PROOF_BYTES = 1 * 1024 * 1024;   // 1 MB proof
                        constexpr uint32_t MAX_TP_BYTES    = 512 * 1024;        // 512 KB transition proof

                        // Minimum v2 utxoblk size: version(1) + hash(32) + height(4) + root_after(32)
                        //                         + block_size(4) + proof_size(4) = 77 bytes minimum
                        if (payload.size() < 77) {
                            g_logger.error("[CSN] Invalid utxoblk: payload too small (" +
                                          std::to_string(payload.size()) + " bytes)");
                            return;
                        }
                        if (payload.size() > MAX_UTXOBLK_SIZE) {
                            g_logger.error("[CSN] utxoblk rejected: payload " +
                                          std::to_string(payload.size()) + " bytes exceeds " +
                                          std::to_string(MAX_UTXOBLK_SIZE) + " byte limit");
                            return;
                        }

                        size_t pos = 0;

                        // Version (1 byte)
                        uint8_t version = payload[pos++];
                        if (version < 2) {
                            g_logger.error("[CSN] Unsupported utxoblk version " + std::to_string(version) +
                                          " (expected >= 2)");
                            return;
                        }

                        // Block hash (32 bytes)
                        uint256 block_hash;
                        std::memcpy(block_hash.data, &payload[pos], 32);
                        pos += 32;

                        // Block height (4 bytes LE)
                        uint32_t block_height = 0;
                        std::memcpy(&block_height, &payload[pos], 4);
                        pos += 4;

                        // Accumulator root after (32 bytes)
                        consensus::UtreexoHash root_after(32, 0);
                        std::memcpy(root_after.data(), &payload[pos], 32);
                        pos += 32;

                        // Block size (4 bytes LE)
                        if (pos + 4 > payload.size()) {
                            g_logger.error("[CSN] Invalid utxoblk: truncated at block_size");
                            return;
                        }
                        uint32_t block_size = 0;
                        std::memcpy(&block_size, &payload[pos], 4);
                        pos += 4;

                        // Validate block_size (overflow-safe: subtract instead of add)
                        if (block_size == 0 || block_size > MAX_BLOCK_BYTES ||
                            block_size > payload.size() - pos) {
                            g_logger.error("[CSN] Invalid utxoblk: block_size=" + std::to_string(block_size) +
                                          " (max=" + std::to_string(MAX_BLOCK_BYTES) +
                                          " remaining=" + std::to_string(payload.size() - pos) + ")");
                            return;
                        }

                        // Deserialize block
                        Block block;
                        try {
                            std::vector<uint8_t> block_bytes(payload.begin() + pos,
                                                             payload.begin() + pos + block_size);
                            auto block_opt = Block::Deserialize(block_bytes);
                            if (!block_opt.has_value()) {
                                g_logger.error("[CSN] Failed to deserialize block from utxoblk: Block::Deserialize returned null");
                                return;
                            }
                            block = *block_opt;
                        } catch (const std::exception& e) {
                            g_logger.error("[CSN] Failed to deserialize block from utxoblk: " +
                                          std::string(e.what()));
                            return;
                        }
                        pos += block_size;

                        // Verify block hash matches
                        if (block.GetHash() != block_hash) {
                            g_logger.error("[CSN] Block hash mismatch in utxoblk from " + peer_addr);
                            return;
                        }

                        // Proof size (4 bytes LE)
                        if (pos + 4 > payload.size()) {
                            g_logger.error("[CSN] Invalid utxoblk: truncated at proof_size");
                            return;
                        }
                        uint32_t proof_size = 0;
                        std::memcpy(&proof_size, &payload[pos], 4);
                        pos += 4;

                        consensus::BlockUtreexoData proof_data;
                        // v3 TP-only historical payloads intentionally carry an empty
                        // batch-proof section and rely on the transition proof appended
                        // later in the message. Older CSN parsers rejected proof_size=0
                        // too early and never reached TP parsing.
                        if (proof_size == 0) {
                            if (version < 3) {
                                g_logger.error("[CSN] Invalid utxoblk: proof_size=0 for legacy version");
                                return;
                            }
                        } else {
                            // Validate proof_size (overflow-safe: subtract instead of add)
                            if (proof_size > MAX_PROOF_BYTES ||
                                proof_size > payload.size() - pos) {
                                g_logger.error("[CSN] Invalid utxoblk: proof_size=" + std::to_string(proof_size) +
                                              " (max=" + std::to_string(MAX_PROOF_BYTES) +
                                              " remaining=" + std::to_string(payload.size() - pos) + ")");
                                return;
                            }

                            // Deserialize proof
                            std::vector<uint8_t> proof_bytes(payload.begin() + pos,
                                                             payload.begin() + pos + proof_size);
                            try {
                                proof_data = consensus::BlockUtreexoData::deserialize(proof_bytes);
                            } catch (const std::exception& e) {
                                g_logger.error("[CSN] Failed to deserialize utreexo proof from " +
                                              peer_addr + ": " + std::string(e.what()));
                                return;
                            }

                            pos += proof_size;
                        }

                        // v3: Parse transition proof (if present)
                        std::optional<consensus::UtreexoTransitionProof> transition_proof;
                        if (version >= 3 && pos + 4 <= payload.size()) {
                            uint32_t tp_size = 0;
                            std::memcpy(&tp_size, &payload[pos], 4);
                            pos += 4;
                            if (tp_size > 0 && tp_size <= MAX_TP_BYTES &&
                                tp_size <= payload.size() - pos) {
                                try {
                                    std::vector<uint8_t> tp_bytes(payload.begin() + pos,
                                                                   payload.begin() + pos + tp_size);
                                    transition_proof = consensus::UtreexoTransitionProof::deserialize(tp_bytes);
                                    g_logger.debug("[CSN] Parsed transition proof (" +
                                                  std::to_string(tp_size) + " bytes) for height " +
                                                  std::to_string(block_height));
                                } catch (const std::exception& e) {
                                    g_logger.warning("[CSN] Failed to parse transition proof: " +
                                                    std::string(e.what()) + " — falling back to batch proof");
                                }
                                pos += tp_size;
                            }
                        }

                        // Build UtreexoProofMessage for StatelessNode validation API
                        UtreexoProofMessage proof_msg;
                        proof_msg.block_hash = block_hash;
                        proof_msg.block_height = block_height;
                        proof_msg.accumulator_root_before = proof_data.accumulator_root_before;
                        proof_msg.accumulator_root_after = root_after;
                        proof_msg.proof_data = proof_data;

                        auto maybe_request_frontier_headers = [&](const std::string& source_peer,
                                                                  uint32_t validated_height) {
                            if (!header_chain_for_csn || !p2p_service_for_csn || !chainstate_service) {
                                return;
                            }

                            const auto* best_header = header_chain_for_csn->GetBestHeader();
                            if (!best_header || validated_height < best_header->height) {
                                return;
                            }
                            if (*frontier_refresh_height >= validated_height) {
                                return;
                            }

                            auto locator = chainstate_service->GenerateBlockLocator();
                            if (locator.empty()) {
                                return;
                            }

                            std::vector<std::string> locator_hex;
                            locator_hex.reserve(locator.size());
                            for (const auto& hash : locator) {
                                locator_hex.push_back(hash.GetHex());
                            }

                            auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                            p2p_service_for_csn->get().send_to_peer(source_peer, getheaders_msg);
                            *frontier_refresh_height = validated_height;

                            g_logger.info("[CSN] Reached known header frontier at height " +
                                          std::to_string(validated_height) + " via " + source_peer +
                                          " — requested headers refresh");
                        };

                        auto should_use_transition_proof = [](const auto& pending) {
                            return daemon_helpers::ShouldUseTransitionProof(
                                pending.proof_msg.proof_data,
                                pending.transition_proof
                            );
                        };

                        // --- Thread-safe buffer + drain under lock ---
                        std::lock_guard<std::mutex> lock(*buffer_mutex);

                        // CSN reorg reset: ActivateBestChain signals us to reset after a STATELESS reorg
                        {
                            uint32_t reset_h = chainstate_service->GetCSNReorgResetHeight();
                            if (reset_h > 0) {
                                // One-shot signal: clear immediately once observed.
                                chainstate_service->ClearCSNReorgReset();
                                if (reset_h != *next_validate_height) {
                                    g_logger.info("[CSN] Reorg reset: next_validate_height " +
                                                 std::to_string(*next_validate_height) + " → " +
                                                 std::to_string(reset_h));
                                    *next_validate_height = reset_h;
                                    pending_blocks->clear();
                                    pending_count_for_scheduler->store(0, std::memory_order_relaxed);
                                    retry_counts->clear();
                                }
                            }
                        }

                        // Pre-drain: consume any buffered blocks before inserting new ones.
                        // Without this, the safety limit (below) drops new arrivals while
                        // validated blocks sit in the buffer un-drained, causing CSN IBD stall.
                        while (pending_blocks->count(*next_validate_height)) {
                            auto it = pending_blocks->find(*next_validate_height);
                            auto& pending = it->second;
                            if (pending.proof_msg.block_height != *next_validate_height) {
                                pending_blocks->erase(it);
                                break;
                            }
                            uint64_t peer_id = GetPeerID(pending.peer_addr);
                            const bool use_transition_proof = should_use_transition_proof(pending);
                            bool valid;
                            if (use_transition_proof) {
                                valid = stateless_node->ValidateWithTransitionProof(
                                    pending.block, pending.proof_msg,
                                    pending.transition_proof.value(), peer_id);
                            } else {
                                valid = stateless_node->ValidateUtreexoProof(
                                    pending.block, pending.proof_msg, peer_id);
                            }
                            if (valid) {
                                g_logger.info("[CSN] Block " + pending.proof_msg.block_hash.GetHex().substr(0, 16) +
                                             "... validated with " +
                                             (use_transition_proof ? "transition" : "batch") +
                                             " proof (height=" +
                                             std::to_string(*next_validate_height) + ")");
                                // Keep StatelessNode's notion of sync height aligned with
                                // the ordered CSN validation cursor. Without this, stale
                                // proofdata for already-applied historical heights bypasses
                                // the stale-proof filter and gets treated as malicious.
                                stateless_node->SetSyncHeight(*next_validate_height);
                                // Persist the validated proof payload with the block so
                                // later ConnectTip replays have the stateless spend data.
                                pending.block.utreexo = pending.proof_msg.proof_data;

                                std::string block_serialized = pending.block.Serialize();
                                std::string blockHex;
                                blockHex.reserve(block_serialized.size() * 2);
                                for (unsigned char byte : block_serialized) {
                                    char buf[3];
                                    snprintf(buf, sizeof(buf), "%02x", byte);
                                    blockHex += buf;
                                }
                                chainstate_service->ProcessIncomingBlockHex(blockHex, pending.peer_addr);
                                if (block_download_for_csn) {
                                    block_download_for_csn->MarkBlockConnected(
                                        pending.proof_msg.block_hash
                                    );
                                }
                                if (block_relay) {
                                    block_relay->AnnounceBlock(pending.proof_msg.block_hash);
                                }

                                // CSN reorg support: persist forest checkpoint + spend targets
                                if (auto* cdb = chainstate_service->GetChainDB()) {
                                    ChainWriteToken ckpt_token;
                                    rocksdb::WriteBatch ckpt_batch;
                                    bool persist_ready = true;
                                    if (auto* forest = stateless_node->GetForest()) {
                                        auto forest_data = forest->serialize();
                                        auto ckpt_status = cdb->putUtreexoCheckpoint(
                                            ckpt_token,
                                            static_cast<int>(*next_validate_height),
                                            forest_data,
                                            &ckpt_batch);
                                        persist_ready = persist_ready && (ckpt_status == Status::Ok);
                                    } else {
                                        persist_ready = false;
                                    }
                                    const auto& targets =
                                        use_transition_proof
                                            ? pending.transition_proof->deletion_targets
                                            : pending.proof_msg.proof_data.spend_proof.targets;
                                    std::string targets_blob;
                                    uint32_t tcount = static_cast<uint32_t>(targets.size());
                                    targets_blob.append(reinterpret_cast<const char*>(&tcount), 4);
                                    for (const auto& t : targets) {
                                        targets_blob.append(reinterpret_cast<const char*>(t.data()), t.size());
                                    }
                                    auto targets_status = cdb->putCSNSpendTargets(
                                        ckpt_token,
                                        pending.proof_msg.block_hash,
                                        targets_blob,
                                        &ckpt_batch);
                                    persist_ready = persist_ready && (targets_status == Status::Ok);

                                    if (persist_ready) {
                                        auto write_status = cdb->writeBatch(ckpt_token, std::move(ckpt_batch), true);
                                        if (write_status != Status::Ok) {
                                            g_logger.warning("[CSN] Failed to persist checkpoint+spend-targets atomically");
                                        }
                                    } else {
                                        g_logger.warning("[CSN] Failed preparing checkpoint+spend-target persistence batch");
                                    }
                                }

                                maybe_request_frontier_headers(pending.peer_addr, *next_validate_height);
                            } else {
                                uint32_t h = *next_validate_height;
                                auto& retries = (*retry_counts)[h];
                                retries++;
                                g_logger.error("[CSN] Invalid utreexo proof at height " +
                                              std::to_string(h) + " from " + pending.peer_addr +
                                              " (attempt " + std::to_string(retries) + "/" +
                                              std::to_string(MAX_RETRIES_PER_HEIGHT) + ")");
                                uint256 failed_hash = pending.proof_msg.block_hash;
                                pending_blocks->erase(it);
                                if (block_download_for_csn) {
                                    if (retries < MAX_RETRIES_PER_HEIGHT) {
                                        block_download_for_csn->ReRequestBlock(failed_hash);
                                    } else {
                                        g_logger.error("[CSN] Proof failed " + std::to_string(retries) +
                                                      "x at height " + std::to_string(h) +
                                                      " — halting CSN IBD to avoid infinite loop");
                                        block_download_for_csn->MarkBlockInvalid(failed_hash);
                                    }
                                }
                                break;
                            }
                            pending_blocks->erase(it);
                            (*next_validate_height)++;
                        }
                        pending_count_for_scheduler->store(pending_blocks->size(), std::memory_order_relaxed);

                        std::optional<uint256> expected_hash_at_height;
                        if (block_download_for_csn) {
                            uint256 expected_hash;
                            if (block_download_for_csn->GetExpectedHashAtHeight(block_height, expected_hash)) {
                                expected_hash_at_height = expected_hash;
                            }
                        }

                        // Validate block hash against expected header chain hash (anti-poisoning)
                        if (expected_hash_at_height.has_value() &&
                            block_hash != expected_hash_at_height.value()) {
                            g_logger.error("[CSN] Block at height " + std::to_string(block_height) +
                                          " has wrong hash (expected " +
                                          expected_hash_at_height->GetHex().substr(0, 16) +
                                          "..., got " + block_hash.GetHex().substr(0, 16) + "...)");
                            return;
                        }

                        // Competing-branch reorg support: the scheduler can legitimately
                        // request the best-header hash at a height below the current
                        // validation cursor. When that happens, reset the stateless cursor
                        // immediately so the replacement block is validated instead of being
                        // dropped as stale while waiting for a later chainstate reorg signal.
                        bool competing_reorg_block = false;
                        if (block_height < *next_validate_height &&
                            expected_hash_at_height.has_value() &&
                            block_hash == expected_hash_at_height.value() &&
                            (!block_download_for_csn ||
                             !block_download_for_csn->IsBlockConnected(block_hash))) {
                            uint256 active_hash_at_height;
                            if (consensus::GetActiveChainHashAtHeight(
                                    chainstate_service->GetActiveTip(),
                                    block_height,
                                    active_hash_at_height) &&
                                active_hash_at_height != block_hash) {
                                competing_reorg_block = true;
                                g_logger.info("[CSN] Competing-branch utxoblk at height " +
                                              std::to_string(block_height) +
                                              " matches best-header hash but not active chain — resetting cursor from " +
                                              std::to_string(*next_validate_height) + " to " +
                                              std::to_string(block_height));
                                const uint32_t fork_height = (block_height > 0) ? (block_height - 1) : 0;
                                std::string rewind_error;
                                if (!chainstate_service->RestoreUtreexoCheckpoint(fork_height, rewind_error)) {
                                    g_logger.error("[CSN] Failed to restore fork-point checkpoint at height " +
                                                  std::to_string(fork_height) + ": " + rewind_error);
                                    return;
                                }
                                stateless_node->SyncToForestState(fork_height);
                                g_logger.info("[CSN] Restored stateless pre-state to fork checkpoint height " +
                                              std::to_string(fork_height));
                                *next_validate_height = block_height;
                                pending_blocks->clear();
                                pending_count_for_scheduler->store(0, std::memory_order_relaxed);
                                retry_counts->clear();
                            }
                        }

                        // Reject blocks at heights already validated — prevents zombie entries
                        // from duplicate peer responses (getdata sent to N peers, only first is useful;
                        // late duplicates pass the pending_blocks duplicate check after erasure)
                        if (block_height < *next_validate_height && !competing_reorg_block) {
                            g_logger.debug("[CSN] Ignoring stale utxoblk at height " +
                                          std::to_string(block_height) + " (cursor=" +
                                          std::to_string(*next_validate_height) + ")");
                            return;
                        }

                        // ── Hardening: reject blocks absurdly far ahead of cursor ──
                        constexpr uint32_t MAX_PENDING_WINDOW = 256;
                        if (block_height > *next_validate_height + MAX_PENDING_WINDOW) {
                            g_logger.warning("[CSN] Rejected block at height " +
                                            std::to_string(block_height) + " — too far ahead of cursor " +
                                            std::to_string(*next_validate_height));
                            return;
                        }
                        // Prune stale buffered entries below cursor so replayed or duplicate
                        // responses cannot accumulate indefinitely in pending_blocks.
                        if (!pending_blocks->empty()) {
                            size_t stale_pruned = 0;
                            auto stale_it = pending_blocks->begin();
                            while (stale_it != pending_blocks->end() &&
                                   stale_it->first < *next_validate_height) {
                                stale_it = pending_blocks->erase(stale_it);
                                stale_pruned++;
                            }
                            if (stale_pruned > 0) {
                                g_logger.warning("[CSN] Pruned " + std::to_string(stale_pruned) +
                                                " stale pending block(s) below cursor " +
                                                std::to_string(*next_validate_height));
                            }
                            pending_count_for_scheduler->store(
                                pending_blocks->size(), std::memory_order_relaxed);
                        }
                        // NOTE: No buffer size drop. Received blocks are always inserted.
                        // Scheduler liveness is enforced inside BlockDownloadScheduler::Tick()
                        // (backpressure clamp keeps one request slot available), so we should
                        // not suppress Tick() here based on pending window saturation.

                        // Reject duplicate heights already in the buffer
                        if (pending_blocks->count(block_height)) {
                            g_logger.debug("[CSN] Duplicate utxoblk at height " +
                                          std::to_string(block_height) + ", ignoring");
                            return;
                        }

                        // Notify scheduler: download completed (frees in-flight slot)
                        if (block_download_for_csn) {
                            block_download_for_csn->OnBlockReceived(block);
                        }
                        if (parallel_download_for_csn) {
                            parallel_download_for_csn->notifyBlockReceived(block_hash);
                        }

                        // Buffer block+proof for ordered validation
                        (*pending_blocks)[block_height] = PendingUtxoBlock{
                            std::move(block), std::move(proof_msg), peer_addr,
                            std::move(transition_proof)
                        };
                        pending_count_for_scheduler->store(
                            pending_blocks->size(), std::memory_order_relaxed);

                        // Runtime invariant: buffer + inflight must never exceed max_window
                        if (block_download_for_csn) {
                            uint32_t dbg_max = block_download_for_csn->GetMaxInFlight();
                            size_t dbg_total = pending_blocks->size() + block_download_for_csn->GetInFlightCount();
                            if (dbg_total > dbg_max + 1) {
                                g_logger.error("[CSN] INVARIANT: pending(" +
                                              std::to_string(pending_blocks->size()) +
                                              ") + inflight(" +
                                              std::to_string(block_download_for_csn->GetInFlightCount()) +
                                              ") > max_window(" + std::to_string(dbg_max) + ") + 1");
                            }
                        }

                        // Drain: validate buffered blocks in strict height order
                        // (Utreexo accumulator is sequential: block N+1 depends on N's root)
                        while (pending_blocks->count(*next_validate_height)) {
                            auto it = pending_blocks->find(*next_validate_height);
                            auto& pending = it->second;

                            // Runtime invariant: only validate at exact next_validate_height
                            if (pending.proof_msg.block_height != *next_validate_height) {
                                g_logger.error("[CSN] INVARIANT: validating out-of-order block (expected " +
                                              std::to_string(*next_validate_height) + " got " +
                                              std::to_string(pending.proof_msg.block_height) + ")");
                                pending_blocks->erase(it);
                                break;
                            }
                            uint64_t peer_id = GetPeerID(pending.peer_addr);
                            const bool use_transition_proof = should_use_transition_proof(pending);
                            bool valid;
                            if (use_transition_proof) {
                                valid = stateless_node->ValidateWithTransitionProof(
                                    pending.block, pending.proof_msg,
                                    pending.transition_proof.value(), peer_id);
                            } else {
                                valid = stateless_node->ValidateUtreexoProof(
                                    pending.block, pending.proof_msg, peer_id);
                            }

                            if (valid) {
                                g_logger.info("[CSN] Block " + pending.proof_msg.block_hash.GetHex().substr(0, 16) +
                                             "... validated with " +
                                             (use_transition_proof ? "transition" : "batch") +
                                             " proof (height=" +
                                             std::to_string(*next_validate_height) + ")");
                                // Keep StatelessNode's stale-proof filter aligned with the
                                // ordered CSN validation cursor in both drain paths.
                                stateless_node->SetSyncHeight(*next_validate_height);

                                // Persist the validated proof payload with the block so
                                // later ConnectTip replays have the stateless spend data.
                                pending.block.utreexo = pending.proof_msg.proof_data;

                                // Route validated block to ChainstateService for storage
                                std::string block_serialized = pending.block.Serialize();
                                std::string blockHex;
                                blockHex.reserve(block_serialized.size() * 2);
                                for (unsigned char byte : block_serialized) {
                                    char buf[3];
                                    snprintf(buf, sizeof(buf), "%02x", byte);
                                    blockHex += buf;
                                }
                                chainstate_service->ProcessIncomingBlockHex(blockHex, pending.peer_addr);
                                if (block_download_for_csn) {
                                    block_download_for_csn->MarkBlockConnected(
                                        pending.proof_msg.block_hash
                                    );
                                }

                                // Announce validated block to other peers (also marks as seen)
                                if (block_relay) {
                                    block_relay->AnnounceBlock(pending.proof_msg.block_hash);
                                }

                                // CSN reorg support: persist forest checkpoint + spend targets
                                if (auto* cdb = chainstate_service->GetChainDB()) {
                                    ChainWriteToken ckpt_token;
                                    rocksdb::WriteBatch ckpt_batch;
                                    bool persist_ready = true;
                                    if (auto* forest = stateless_node->GetForest()) {
                                        auto forest_data = forest->serialize();
                                        auto ckpt_status = cdb->putUtreexoCheckpoint(
                                            ckpt_token,
                                            static_cast<int>(*next_validate_height),
                                            forest_data,
                                            &ckpt_batch);
                                        persist_ready = persist_ready && (ckpt_status == Status::Ok);
                                    } else {
                                        persist_ready = false;
                                    }
                                    const auto& targets =
                                        use_transition_proof
                                            ? pending.transition_proof->deletion_targets
                                            : pending.proof_msg.proof_data.spend_proof.targets;
                                    std::string targets_blob;
                                    uint32_t tcount = static_cast<uint32_t>(targets.size());
                                    targets_blob.append(reinterpret_cast<const char*>(&tcount), 4);
                                    for (const auto& t : targets) {
                                        targets_blob.append(reinterpret_cast<const char*>(t.data()), t.size());
                                    }
                                    auto targets_status = cdb->putCSNSpendTargets(
                                        ckpt_token,
                                        pending.proof_msg.block_hash,
                                        targets_blob,
                                        &ckpt_batch);
                                    persist_ready = persist_ready && (targets_status == Status::Ok);

                                    if (persist_ready) {
                                        auto write_status = cdb->writeBatch(ckpt_token, std::move(ckpt_batch), true);
                                        if (write_status != Status::Ok) {
                                            g_logger.warning("[CSN] Failed to persist checkpoint+spend-targets atomically");
                                        }
                                    } else {
                                        g_logger.warning("[CSN] Failed preparing checkpoint+spend-target persistence batch");
                                    }
                                }

                                maybe_request_frontier_headers(pending.peer_addr, *next_validate_height);
                            } else {
                                uint32_t h = *next_validate_height;
                                auto& retries = (*retry_counts)[h];
                                retries++;
                                g_logger.error("[CSN] Invalid utreexo proof at height " +
                                              std::to_string(h) +
                                              " from " + pending.peer_addr +
                                              " (attempt " + std::to_string(retries) +
                                              "/" + std::to_string(MAX_RETRIES_PER_HEIGHT) + ")");
                                uint256 failed_hash = pending.proof_msg.block_hash;
                                pending_blocks->erase(it);
                                if (retries >= MAX_RETRIES_PER_HEIGHT) {
                                    g_logger.error("[CSN] Proof failed " + std::to_string(retries) +
                                                  "x at height " + std::to_string(h) +
                                                  " — halting CSN IBD to avoid infinite loop");
                                    if (block_download_for_csn) {
                                        block_download_for_csn->MarkBlockInvalid(failed_hash);
                                    }
                                } else if (block_download_for_csn) {
                                    block_download_for_csn->ReRequestBlock(failed_hash);
                                }
                                break;
                            }

                            pending_blocks->erase(it);
                            (*next_validate_height)++;
                        }
                        pending_count_for_scheduler->store(
                            pending_blocks->size(), std::memory_order_relaxed);

                        // Always tick after processing. Tick() applies external backpressure
                        // and keeps one request slot alive for gap recovery.
                        if (block_download_for_csn) {
                            uint32_t max_window = block_download_for_csn->GetMaxInFlight();
                            size_t pending_count = pending_blocks->size();
                            size_t inflight_count = block_download_for_csn->GetInFlightCount();
                            size_t total_outstanding = pending_count + inflight_count;
                            block_download_for_csn->Tick();
                            g_logger.debug("[CSN-IBD] pending=" + std::to_string(pending_count) +
                                          " inflight=" + std::to_string(inflight_count) +
                                          " total=" + std::to_string(total_outstanding) +
                                          " max=" + std::to_string(max_window) +
                                          " next_validate=" + std::to_string(*next_validate_height));
                        }
                    };

                    std::cout << "[DaemonApp] ✅ OnUtxoBlock handler wired for CSN (Phase P.3, windowed IBD)" << std::endl;

                    // ================================================================
                    // Phase #4: Wire OnUtxoTx handler (CSN receives tx + proofs)
                    // ================================================================
                    if (ctx_.tx_relay) {
                        auto tx_relay_for_utxotx = ctx_.tx_relay;
                        auto mempool_for_utxotx = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);

                        p2p_service->OnUtxoTx = [stateless_node, tx_relay_for_utxotx, mempool_for_utxotx](
                            const std::string& peer_addr,
                            const ::P2PMessage& msg
                        ) {
                            const auto& payload = msg.payload;

                            // ── Hardening: size bounds ──
                            constexpr size_t MAX_UTXOTX_SIZE = 2 * 1024 * 1024;
                            constexpr uint32_t MAX_TX_BYTES = 1 * 1024 * 1024;
                            constexpr uint32_t MAX_PROOF_BYTES = 256 * 1024;
                            constexpr uint32_t MAX_SCRIPT_BYTES = 10 * 1024;
                            constexpr uint32_t MAX_PROOFS = 1000;

                            if (payload.size() < 73) {
                                g_logger.error("[CSN-TX] utxotx payload too small (" +
                                              std::to_string(payload.size()) + " bytes)");
                                return;
                            }
                            if (payload.size() > MAX_UTXOTX_SIZE) {
                                g_logger.error("[CSN-TX] utxotx rejected: " +
                                              std::to_string(payload.size()) + " bytes exceeds limit");
                                return;
                            }

                            size_t pos = 0;

                            // 1. Version
                            uint8_t version = payload[pos++];
                            if (version != 0x01) {
                                g_logger.error("[CSN-TX] Unsupported utxotx version " + std::to_string(version));
                                return;
                            }

                            // 2. Txid (32 bytes)
                            if (32 > payload.size() - pos) return;
                            uint256 txid;
                            std::memcpy(txid.data, &payload[pos], 32);
                            pos += 32;
                            auto complete_refresh = [&]() {
                                if (tx_relay_for_utxotx) {
                                    tx_relay_for_utxotx->CompleteRefresh(txid);
                                }
                            };

                            // 3. TX size + data
                            if (4 > payload.size() - pos) return;
                            uint32_t tx_size = 0;
                            std::memcpy(&tx_size, &payload[pos], 4);
                            pos += 4;

                            if (tx_size == 0 || tx_size > MAX_TX_BYTES || tx_size > payload.size() - pos) {
                                complete_refresh();
                                g_logger.error("[CSN-TX] Invalid tx_size=" + std::to_string(tx_size));
                                return;
                            }

                            Transaction tx;
                            {
                                std::vector<uint8_t> tx_bytes(payload.begin() + pos,
                                                               payload.begin() + pos + tx_size);
                                if (!dinero::TransactionSerializer::Deserialize(tx, tx_bytes)) {
                                    complete_refresh();
                                    g_logger.error("[CSN-TX] TX deserialization failed");
                                    return;
                                }
                            }
                            pos += tx_size;

                            // 4. Num proofs
                            if (4 > payload.size() - pos) return;
                            uint32_t num_proofs = 0;
                            std::memcpy(&num_proofs, &payload[pos], 4);
                            pos += 4;

                            if (num_proofs > MAX_PROOFS) {
                                complete_refresh();
                                g_logger.error("[CSN-TX] Too many proofs: " + std::to_string(num_proofs));
                                return;
                            }

                            // 5. Parse per-input proofs
                            std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>> input_proofs;
                            input_proofs.reserve(num_proofs);

                            for (uint32_t i = 0; i < num_proofs; i++) {
                                if (4 > payload.size() - pos) {
                                    complete_refresh();
                                    g_logger.error("[CSN-TX] Truncated proof header at index " + std::to_string(i));
                                    return;
                                }
                                uint32_t proof_size = 0;
                                std::memcpy(&proof_size, &payload[pos], 4);
                                pos += 4;

                                if (proof_size > MAX_PROOF_BYTES || proof_size > payload.size() - pos) {
                                    complete_refresh();
                                    g_logger.error("[CSN-TX] Invalid proof_size=" + std::to_string(proof_size));
                                    return;
                                }

                                consensus::UtreexoProof proof;
                                try {
                                    std::vector<uint8_t> proof_bytes(payload.begin() + pos,
                                                                      payload.begin() + pos + proof_size);
                                    proof = consensus::UtreexoProof::deserialize(proof_bytes);
                                } catch (const std::exception& e) {
                                    complete_refresh();
                                    g_logger.error("[CSN-TX] Proof deserialization failed at index " + std::to_string(i));
                                    return;
                                }
                                pos += proof_size;

                                // Value (8 bytes LE)
                                if (8 > payload.size() - pos) return;
                                uint64_t value = 0;
                                for (int b = 0; b < 8; b++)
                                    value |= static_cast<uint64_t>(payload[pos + b]) << (b * 8);
                                pos += 8;

                                // Script size (4 bytes LE)
                                if (4 > payload.size() - pos) return;
                                uint32_t script_size = 0;
                                std::memcpy(&script_size, &payload[pos], 4);
                                pos += 4;

                                if (script_size > MAX_SCRIPT_BYTES || script_size > payload.size() - pos) {
                                    complete_refresh();
                                    g_logger.error("[CSN-TX] Invalid script_size=" + std::to_string(script_size));
                                    return;
                                }

                                std::vector<uint8_t> scriptPubKey(payload.begin() + pos,
                                                                   payload.begin() + pos + script_size);
                                pos += script_size;

                                consensus::SpentOutputData spent;
                                spent.value = value;
                                spent.scriptPubKey = std::move(scriptPubKey);

                                input_proofs.emplace_back(std::move(proof), std::move(spent));
                            }

                            // 6. Accumulator root (32 bytes)
                            if (32 > payload.size() - pos) {
                                complete_refresh();
                                g_logger.error("[CSN-TX] Missing accumulator root");
                                return;
                            }
                            consensus::UtreexoHash acc_root;
                            acc_root.assign(payload.begin() + pos, payload.begin() + pos + 32);
                            pos += 32;

                            // 7. Validate via StatelessNode
                            if (!stateless_node->ValidateUtreexoTx(tx, input_proofs, acc_root)) {
                                complete_refresh();
                                g_logger.warning("[CSN-TX] utxotx proof validation failed from " + peer_addr);
                                return;
                            }

                            // 8. Accept into mempool or refresh existing proof
                            if (mempool_for_utxotx) {
                                bool already_in_mempool = mempool_for_utxotx->hasTransaction(txid);

                                if (already_in_mempool) {
                                    // REFRESH PATH (#6): TX already in mempool, update proof + cache
                                    mempool_for_utxotx->mempool().refreshProof(
                                        txid, acc_root, stateless_node->GetSyncHeight());
                                    mempool_for_utxotx->mempool().setCachedUtxoTxPayload(
                                        txid, std::vector<uint8_t>(payload.begin(), payload.end()));
                                    if (tx_relay_for_utxotx) {
                                        tx_relay_for_utxotx->CompleteRefresh(txid);
                                        tx_relay_for_utxotx->RecordBridgeResponse(peer_addr);
                                    }
                                    g_logger.info("[CSN-TX] Refreshed proof for " + txid.GetHex().substr(0, 16) +
                                                 "... from " + peer_addr);
                                } else {
                                    // NEW TX PATH: accept into mempool
                                    bool accepted_new_tx = false;
                                    auto submit_result = mempool_for_utxotx->Submit(tx, TxOrigin::P2P);
                                    if (submit_result.accepted()) {
                                        mempool_for_utxotx->mempool().refreshProof(
                                            txid, acc_root, stateless_node->GetSyncHeight());
                                        mempool_for_utxotx->mempool().setCachedUtxoTxPayload(
                                            txid, std::vector<uint8_t>(payload.begin(), payload.end()));
                                        accepted_new_tx = true;
                                    } else {
                                        g_logger.warning(
                                            "[CSN-TX] Mempool rejection: " +
                                            std::string(TxRejectCodeToString(submit_result.code)) +
                                            ": " + submit_result.message);
                                    }
                                    if (tx_relay_for_utxotx) {
                                        tx_relay_for_utxotx->RecordBridgeResponse(peer_addr);
                                        if (accepted_new_tx) {
                                            tx_relay_for_utxotx->AnnounceTx(txid);
                                        } else {
                                            tx_relay_for_utxotx->CompleteRefresh(txid);
                                        }
                                    }
                                    if (accepted_new_tx) {
                                        g_logger.info("[CSN-TX] Accepted utxotx " + txid.GetHex().substr(0, 16) +
                                                     "... from " + peer_addr + " (" +
                                                     std::to_string(num_proofs) + " proofs)");
                                    }
                                }
                            }
                        };

                        // Phase #4: Enable CSN mode for getdata(MSG_UTREEXO_TX)
                        tx_relay_for_utxotx->SetCsnMode(true);
                        std::cout << "[DaemonApp] ✅ Phase #4 OnUtxoTx + CSN mode wired" << std::endl;
                    }
                }
            }

            // NOTE: OnGetHeaders still routes to ChainstateService (for responding to header requests)
            p2p_service->OnGetHeaders = [chainstate_service](const std::string& peer_addr, const ::P2PMessage& msg) {
                chainstate_service->OnGetHeaders(peer_addr, msg);
            };
        }
    }

    // Phase N: Wire Phase N components to P2PService callbacks
    if (ctx_.header_sync && ctx_.block_download && ctx_.p2p) {
        auto p2p_service = std::dynamic_pointer_cast<P2PService>(ctx_.p2p);
        if (p2p_service) {
            std::cout << "[DaemonApp] Wiring Phase N components to P2PService..." << std::endl;

            // Wire OnHeaders: Route incoming headers to HeaderSyncP2P
            // Phase N.3 Fix: Added truncation handling for batched header sync
            auto block_download_ptr = ctx_.block_download;
            auto header_chain_ptr = ctx_.header_chain;
            auto chainstate_ptr = ctx_.chainstate;
            auto p2p_weak = std::weak_ptr<P2PService>(p2p_service);  // Capture weak to avoid cycle
            constexpr size_t MAX_HEADERS_PER_MSG = 2000;  // Bitcoin standard
            auto stateless_cmpct_refresh_times =
                std::make_shared<std::unordered_map<std::string, std::chrono::steady_clock::time_point>>();
            auto stateless_cmpct_refresh_retry_armed =
                std::make_shared<std::unordered_set<std::string>>();
            auto stateless_cmpct_refresh_mutex = std::make_shared<std::mutex>();

            p2p_service->OnHeaders = [header_sync = ctx_.header_sync, block_download_ptr, header_chain_ptr,
                                      chainstate_ptr, p2p_weak, stateless_cmpct_refresh_times,
                                      stateless_cmpct_refresh_retry_armed, stateless_cmpct_refresh_mutex](
                const std::string& peer_addr,
                const ::P2PMessage& msg
            ) {
                try {
                    // Parse headers from P2P message
                    std::vector<BlockHeader> headers = ParseHeadersFromP2PMessage(msg);

                    if (headers.empty()) {
                        g_logger.info("[Phase N] Empty headers from " + peer_addr + " — peer at same tip, headers sync complete");
                        // Empty headers means peer has nothing new — we ARE synchronized.
                        // Signal OnHeadersProcessed so the IBD guard in OnInv unblocks
                        // and inv-based block relay can proceed.
                        {
                            std::lock_guard<std::mutex> lock(*stateless_cmpct_refresh_mutex);
                            stateless_cmpct_refresh_times->erase(peer_addr);
                            stateless_cmpct_refresh_retry_armed->insert(peer_addr);
                        }
                        if (block_download_ptr) {
                            block_download_ptr->OnHeadersProcessed();
                            block_download_ptr->Tick();  // Start downloading any queued blocks
                        }
                        return;
                    }

                    g_logger.info("[Phase N.3] Received " + std::to_string(headers.size()) +
                                 " headers from " + peer_addr);

                    // Convert peer address to ID
                    uint64_t peer_id = GetPeerID(peer_addr);

                    // Route to HeaderSyncP2P
                    bool success = header_sync->GetSyncManager()->ProcessHeaders(peer_id, headers);

                    // Phase N.5: Per-peer recovery attempt counter (shared across success/failure)
                    static std::map<std::string, int> header_recovery_attempts;

                    if (success) {
                        // Clear recovery counter on success — peer is healthy
                        header_recovery_attempts.erase(peer_addr);

                        g_logger.info("[Phase N] HeaderSyncP2P processed " +
                                     std::to_string(headers.size()) + " headers from " + peer_addr);

                        // Sync headers to HeaderChainSelector for BlockDownloadScheduler
                        int added = 0;
                        if (header_chain_ptr) {
                            for (const auto& header : headers) {
                                if (header_chain_ptr->AddHeader(header)) {
                                    added++;
                                }
                            }
                            if (added > 0) {
                                g_logger.info("[Phase N] Added " + std::to_string(added) +
                                             " headers to HeaderChainSelector");
                            }
                        }

                        if (chainstate_ptr && added > 0) {
                            chainstate_ptr->RecordHeaderAnnouncements(peer_addr, headers);
                        }

                        // Always refresh the block scheduler when we accepted new headers.
                        // This keeps block queueing live across long truncated header batches.
                        if (block_download_ptr && added > 0) {
                            block_download_ptr->OnHeadersProcessed();
                            block_download_ptr->Tick();
                            g_logger.info("[Phase N.3] Block download scheduler refreshed after header batch");
                        }

                        // P1 reorg fix: Trigger fork detection when new headers extend
                        // the best chain past our active tip. ActivateBestChain will detect
                        // the better header chain and call RequestBlocks() for missing bodies.
                        // Gate avoids thrash during bulk header batches.
                        if (chainstate_ptr && header_chain_ptr) {
                            auto* best = header_chain_ptr->GetBestHeader();
                            auto* active = chainstate_ptr->GetActiveTip();
                            if (best && active && best->height > active->height) {
                                g_logger.info("[P1] Headers reveal better chain (header=" +
                                             std::to_string(best->height) + " > active=" +
                                             std::to_string(active->height) +
                                             ") — triggering ActivateBestChain");
                                chainstate_ptr->ActivateBestChain();
                            }
                        }

                        // Phase N.3 Fix: Truncation handling - request next batch if full.
                        // Use >= not == because a partial last batch (< 2000) means peer
                        // reached its tip, but we always re-request if we got a full batch
                        // so we don't stop mid-chain on batch boundaries.
                        if (headers.size() >= MAX_HEADERS_PER_MSG) {
                            auto p2p_locked = p2p_weak.lock();
                            if (p2p_locked) {
                                // Use last header hash as locator for next batch
                                const auto& last_header = headers.back();
                                std::string last_hash = last_header.GetHash().GetHex();

                                g_logger.info("[Phase N.3] Headers batch full (" +
                                             std::to_string(headers.size()) +
                                             "), requesting next batch from " + peer_addr +
                                             " starting after " + last_hash.substr(0, 16) + "...");

                                // Send getheaders with last hash as locator
                                std::vector<std::string> locator_hex = { last_hash };
                                auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                                p2p_locked->get().send_to_peer(peer_addr, getheaders_msg);
                            }
                        } else {
                            // Partial batch — peer may be at its tip.
                            g_logger.info("[Phase N.3] Headers partial batch from " + peer_addr +
                                         " (received " + std::to_string(headers.size()) + ")");
                        }
                    } else {
                        // Phase N.5: Recovery — re-request with ChainDB locator
                        // The initial getheaders used ChainDB's locator, but validation
                        // runs against HeaderChainSelector which may have a different view.
                        // On missing parent, immediately retry using ChainDB's locator so
                        // the peer finds the correct common ancestor.

                        // Fix 3: Rate-limit recovery attempts per peer (max 5)
                        int& attempts = header_recovery_attempts[peer_addr];
                        attempts++;

                        // Diagnostic logging (peer, missing prev, locator info, attempt count)
                        std::string first_prev_hex = !headers.empty()
                            ? headers.front().prev_block_hash.GetHex().substr(0, 16) + "..."
                            : "N/A";
                        std::string local_tip_hex = "unknown";
                        std::string local_height_str = "unknown";

                        auto chainstate_service = chainstate_ptr
                            ? std::dynamic_pointer_cast<ChainstateService>(chainstate_ptr)
                            : nullptr;

                        if (chainstate_service && chainstate_service->GetChainDB()) {
                            auto tip_result = chainstate_service->GetChainDB()->getTip();
                            if (tip_result.status() == Status::Ok) {
                                local_tip_hex = tip_result.value().hash.GetHex().substr(0, 16) + "...";
                                local_height_str = std::to_string(tip_result.value().height);
                            }
                        }

                        g_logger.warning("[Phase N.5] Header rejection: peer=" + peer_addr +
                                         " first_prev=" + first_prev_hex +
                                         " local_tip=" + local_tip_hex +
                                         " local_height=" + local_height_str +
                                         " recovery_attempt=" + std::to_string(attempts));

                        constexpr int kMaxRecoveryAttempts = 5;
                        if (attempts >= kMaxRecoveryAttempts) {
                            // Locator includes genesis and peer still can't connect → incompatible.
                            // Mark as misbehaving so peer selection immediately switches away.
                            g_logger.error("[Phase N.5] Peer " + peer_addr +
                                           " incompatible after " + std::to_string(kMaxRecoveryAttempts) +
                                           " recovery attempts — giving up");
                            if (header_sync && header_sync->GetSyncManager()) {
                                header_sync->GetSyncManager()->MarkPeerMisbehaving(peer_id);
                            }
                            header_recovery_attempts.erase(peer_addr);
                            return;
                        }

                        // Recovery locator priority:
                        // 1) HeaderSyncManager locator (exact same view used for AddHeader validation)
                        // 2) ChainDB locator fallback
                        std::vector<std::string> locator_hex;
                        std::string locator_source = "HeaderSyncManager";

                        if (header_sync && header_sync->GetSyncManager()) {
                            auto sync_locator = header_sync->GetSyncManager()->GetHeaderLocator();
                            locator_hex.reserve(sync_locator.size());
                            for (const auto& hash : sync_locator) {
                                locator_hex.push_back(hash.GetHex());
                            }
                        }

                        if (locator_hex.empty()) {
                            locator_source = "ChainDB";
                            if (chainstate_service) {
                                auto locator = chainstate_service->GenerateBlockLocator();
                                locator_hex.reserve(locator.size());
                                for (const auto& hash : locator) {
                                    locator_hex.push_back(hash.GetHex());
                                }
                            }
                        }

                        if (!locator_hex.empty()) {
                            auto p2p_locked = p2p_weak.lock();
                            if (p2p_locked) {
                                auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                                p2p_locked->get().send_to_peer(peer_addr, getheaders_msg);
                                g_logger.info("[Phase N.5] Recovery: sent getheaders to " + peer_addr +
                                              " with " + locator_source + " locator (size=" +
                                              std::to_string(locator_hex.size()) +
                                              ", head=" + locator_hex.front().substr(0, 16) +
                                              "..., attempt=" + std::to_string(attempts) + ")");
                            }
                        } else {
                            g_logger.error("[Phase N.5] Recovery locator is empty (" + locator_source +
                                           ") — cannot recover");
                        }
                    }
                } catch (const std::exception& e) {
                    g_logger.error("[Phase N] Error processing headers from " + peer_addr + ": " + e.what());
                }
            };

            // Phase G.2: Wire OnNewBlock to process blocks and relay
            if (ctx_.block_relay) {
                auto block_relay = ctx_.block_relay;
                auto chainstate = ctx_.chainstate;
                auto block_download = ctx_.block_download;
                auto prune_service = ctx_.prune;  // Phase 34.8: Capture prune service

                p2p_service->OnNewBlock = [block_relay, chainstate, block_download, prune_service, p2p_service](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    try {
                        // DEBUG: Entry point
                        std::cout << "[P2P-DEBUG] >>> OnNewBlock ENTRY from " << peer_addr
                                  << " payload=" << msg.payload.size() << " bytes" << std::endl;

                        // Deserialize block from P2P message
                        Block block = DeserializeBlockFromP2PMessage(msg);

                        const bool stateless_mode = GetConfig().utreexo_stateless;

                        // DEBUG: Show deserialized block info + source classification
                        uint256 block_hash_raw = block.GetHash();
                        std::string block_hash = block_hash_raw.GetHex();
                        // Atomic check: is this block in-flight OR expected by the scheduler?
                        // Uses a single mutex acquisition to avoid TOCTOU races when
                        // ScanForMissingBlocks clears and rebuilds in_flight_blocks_.
                        bool is_known = block_download && block_download->IsBlockKnown(block_hash_raw);
                        bool is_in_flight = is_known;  // conservative: treat known as in-flight for routing
                        bool relay_in_flight = block_relay && block_relay->IsBlockDownloadInFlight(block_hash_raw);
                        const char* source =
                            is_known ? "REQUESTED" :
                            (relay_in_flight ? "RELAY-REQUESTED" : "UNSOLICITED");
                        std::cout << "[P2P-DEBUG] Block deserialized: hash=" << block_hash.substr(0, 16) << "..."
                                  << " prev=" << block.header.prev_block_hash.GetHex().substr(0, 16) << "..."
                                  << " txs=" << block.vtx.size()
                                  << " source=" << source << std::endl;

                        // In CSN/stateless mode, canonical ingestion must flow through
                        // utxoblk so the validated proof payload is persisted with the
                        // block before ChainDB acceptance. A raw "block" message never
                        // carries Block.utreexo, so accepting it here would poison
                        // completed_blocks_/ChainDB with proof-less bodies and cause
                        // later ConnectTip() replays to fail with missing-utreexo-data.
                        if (stateless_mode) {
                            std::cout << "[P2P-DEBUG] Ignoring raw block message in stateless mode; awaiting utxoblk"
                                      << std::endl;
                            return;
                        }

                        // While the scheduler is syncing, allow:
                        // 1) blocks known to the consensus scheduler (in-flight or expected), or
                        // 2) blocks explicitly requested by BlockRelayManager relay scheduler.
                        // Drop everything else as truly unsolicited.
                        bool scheduler_syncing = block_download && !block_download->IsFullySynchronized();
                        if (scheduler_syncing && !is_known && !relay_in_flight) {
                            std::cout << "[P2P-DEBUG] DROPPING unsolicited block " << block_hash.substr(0, 16)
                                      << "... (scheduler still syncing)" << std::endl;
                            return;
                        }

                        // Relay-requested blocks during sync should be routed through
                        // BlockRelayManager so relay scheduler bookkeeping stays coherent.
                        if (scheduler_syncing && relay_in_flight && !is_in_flight) {
                            std::cout << "[P2P-DEBUG] Routing relay-requested block through BlockRelayManager (sync in progress)" << std::endl;
                            block_relay->HandleBlock(peer_addr, block);

                            // Keep consensus scheduler tip hint aligned if chainstate advanced.
                            if (block_download && chainstate) {
                                if (auto* tip = chainstate->GetActiveTip()) {
                                    block_download->SetLocalTipHeight(static_cast<uint32_t>(tip->height));
                                }
                                block_download->Tick();
                            }

                            std::cout << "[P2P-DEBUG] <<< OnNewBlock EXIT success (relay path)" << std::endl;
                            return;
                        }

                        // Phase N.4: Notify scheduler that block was received (clears in_flight)
                        if (block_download) {
                            bool scheduler_accepted = block_download->OnBlockReceived(block);
                            if (scheduler_accepted) {
                                // Trigger next block download immediately
                                block_download->Tick();
                            }
                            // If scheduler doesn't recognize this block (inv-relayed, not
                            // from the scheduler's queue), fall through to ChainstateService.
                        }

                        // During IBD/catch-up, block connection is handled by the scheduler's
                        // TryConnectStoredBlocks() drainer (called from Tick()).
                        // Do NOT send out-of-order blocks to ChainstateService here —
                        // they would always fail with missing-parent since the chain
                        // tip is far behind the download frontier.
                        //
                        // Post-sync (fully synchronized), process blocks immediately
                        // through ChainstateService for instant chain extension.
                        bool scheduler_syncing_after_receive = block_download && !block_download->IsFullySynchronized();

                        // P1 reorg fix: Blocks explicitly requested by ActivateBestChain::RequestBlocks()
                        // are tracked in ChainstateService::in_flight_blocks_, NOT the scheduler's
                        // missing_blocks_ queue. Deferring them to the drain loop = dead end.
                        bool chainstate_requested = false;
                        if (scheduler_syncing_after_receive && chainstate) {
                            chainstate_requested = chainstate->IsBlockInFlight(block_hash);
                            if (chainstate_requested) {
                                std::cout << "[P1] Processing in-flight block " << block_hash.substr(0, 16)
                                          << "... immediately (bypassing scheduler defer)" << std::endl;
                            }
                        }

                        if (scheduler_syncing_after_receive && !chainstate_requested) {
                            std::cout << "[P1] Deferring block " << block_hash.substr(0, 16)
                                      << "... to scheduler (not in-flight)" << std::endl;
                        } else if (chainstate) {
                            std::cout << "[P2P-DEBUG] Processing through ChainstateService (post-IBD)..." << std::endl;
                            bool accepted = chainstate->ProcessIncomingBlock(block, peer_addr);
                            std::cout << "[P2P-DEBUG] ProcessIncomingBlock returned: " << (accepted ? "ACCEPTED" : "REJECTED") << std::endl;

                            // Keep consensus scheduler tip aligned after post-IBD block acceptance.
                            // Without this, local_tip_height_ goes stale while HeaderChainSelector
                            // advances, causing IsFullySynchronized() Gate 3 to permanently fail
                            // and all subsequent INV blocks to be dropped as "scheduler still syncing".
                            if (accepted && block_download && chainstate) {
                                if (auto* tip = chainstate->GetActiveTip()) {
                                    block_download->SetLocalTipHeight(static_cast<uint32_t>(tip->height));
                                }
                            }

                            if (accepted && p2p_service && chainstate && chainstate->GetChainDB()) {
                                auto height_result = chainstate->GetChainDB()->getBlockHeight(block_hash_raw);
                                if (height_result.status() == Status::Ok) {
                                    p2p_service->get().update_peer_synced_blocks(peer_addr, height_result.value());
                                }
                            }

                            // Phase 34.8: Trigger pruning after successful block acceptance
                            if (prune_service && accepted) {
                                prune_service->triggerPruneIfNeeded();
                            }
                        } else {
                            std::cout << "[P2P-DEBUG] ERROR: chainstate is null!" << std::endl;
                        }

                        // Route to BlockRelayManager for relay/peer bookkeeping only once
                        // scheduler synchronization has finished.
                        if (!scheduler_syncing_after_receive) {
                            block_relay->HandleBlock(peer_addr, block);
                        }
                        std::cout << "[P2P-DEBUG] <<< OnNewBlock EXIT success" << std::endl;

                    } catch (const std::exception& e) {
                        std::cout << "[P2P-DEBUG] <<< OnNewBlock EXCEPTION: " << e.what() << std::endl;
                        g_logger.error("[BlockRelay] Error processing block from " + peer_addr + ": " + e.what());
                    }
                };

                std::cout << "[DaemonApp] ✅ Phase G.2 OnNewBlock wired to ChainstateService + BlockRelayManager" << std::endl;

                const bool csn_mode_for_compact = GetConfig().utreexo_stateless;
                p2p_service->OnCompactBlock = [block_relay, csn_mode_for_compact, chainstate, p2p_service,
                                               stateless_cmpct_refresh_times,
                                               stateless_cmpct_refresh_retry_armed,
                                               stateless_cmpct_refresh_mutex](
                                                  const std::string& peer_addr,
                                                  const ::P2PMessage& msg) {
                    try {
                        if (csn_mode_for_compact) {
                            bool should_request_headers = false;
                            const auto now = std::chrono::steady_clock::now();
                            {
                                std::lock_guard<std::mutex> lock(*stateless_cmpct_refresh_mutex);
                                bool retry_armed =
                                    stateless_cmpct_refresh_retry_armed->erase(peer_addr) > 0;
                                auto& last_request = (*stateless_cmpct_refresh_times)[peer_addr];
                                if (retry_armed ||
                                    last_request.time_since_epoch().count() == 0 ||
                                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request).count() >= 1000) {
                                    last_request = now;
                                    should_request_headers = true;
                                }
                            }

                            if (should_request_headers && chainstate && p2p_service) {
                                auto locator = chainstate->GenerateBlockLocator();
                                std::vector<std::string> locator_hex;
                                locator_hex.reserve(locator.size());
                                for (const auto& hash : locator) {
                                    locator_hex.push_back(hash.GetHex());
                                }

                                auto getheaders_msg = ::P2PMessage::create_getheaders(locator_hex);
                                p2p_service->get().send_to_peer(peer_addr, getheaders_msg);
                                g_logger.info("[BlockRelay] Stateless cmpctblock hint from " + peer_addr +
                                              " — requested headers refresh");
                            } else {
                                g_logger.info("[BlockRelay] Stateless mode ignored cmpctblock from " +
                                              peer_addr + " (header refresh rate-limited)");
                            }
                            return;
                        }
                        CompactBlock compact = CompactBlock::Deserialize(msg.payload);
                        if (compact.GetTxCount() == 0) {
                            throw std::runtime_error("empty compact block payload");
                        }
                        block_relay->HandleCompactBlock(peer_addr, compact);
                    } catch (const std::exception& e) {
                        g_logger.error("[BlockRelay] Error processing cmpctblock from " + peer_addr +
                                       ": " + e.what());
                    }
                };

                p2p_service->OnGetBlockTxn = [block_relay](const std::string& peer_addr,
                                                           const ::P2PMessage& msg) {
                    try {
                        BlockTransactionsRequest request = BlockTransactionsRequest::Deserialize(msg.payload);
                        if (request.block_hash.IsNull()) {
                            throw std::runtime_error("invalid getblocktxn payload");
                        }
                        block_relay->HandleGetBlockTxn(peer_addr, request);
                    } catch (const std::exception& e) {
                        g_logger.error("[BlockRelay] Error processing getblocktxn from " + peer_addr +
                                       ": " + e.what());
                    }
                };

                p2p_service->OnBlockTxn = [block_relay](const std::string& peer_addr,
                                                        const ::P2PMessage& msg) {
                    try {
                        BlockTransactions response = BlockTransactions::Deserialize(msg.payload);
                        if (response.block_hash.IsNull()) {
                            throw std::runtime_error("invalid blocktxn payload");
                        }
                        block_relay->HandleBlockTxn(peer_addr, response);
                    } catch (const std::exception& e) {
                        g_logger.error("[BlockRelay] Error processing blocktxn from " + peer_addr +
                                       ": " + e.what());
                    }
                };
            }

            // Phase G.3: Wire OnNewTx to TxRelayManager (minimal relay)
            if (ctx_.tx_relay) {
                auto tx_relay = ctx_.tx_relay;

                p2p_service->OnNewTx = [tx_relay](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    try {
                        // Deserialize transaction from P2P message
                        Transaction tx = DeserializeTransactionFromP2PMessage(msg);

                        // Route to TxRelayManager
                        tx_relay->HandleTx(peer_addr, tx);

                    } catch (const std::exception& e) {
                        g_logger.error("[TxRelay] Error processing tx from " + peer_addr + ": " + e.what());
                    }
                };

                std::cout << "[DaemonApp] ✅ Phase G.3 OnNewTx wired to TxRelayManager" << std::endl;
            }

            // Phase G.2: Wire BlockRelayManager callbacks
            if (ctx_.block_relay) {
                auto block_relay = ctx_.block_relay;

                // Wire BlockRelayManager send callback (BlockRelay → P2P)
                block_relay->SetSendMessageCallback([p2p_service, chainstate](
                    const std::string& peer_address,
                    const std::string& command,
                    const std::vector<uint8_t>& payload
                ) {
                    ::P2PMessage msg;
                    msg.command = (command == "inv_all") ? "inv" : command;
                    msg.payload = payload;
                    msg.checksum = 0;  // P2PManager will calculate

                    if (peer_address.empty()) {
                        uint32_t local_tip_height = 0;
                        if (chainstate) {
                            if (const auto* tip = chainstate->GetActiveTip()) {
                                local_tip_height = static_cast<uint32_t>(tip->height);
                            }
                        }

                        const auto peer_negotiated_compact = [](const auto& peer) {
                            return peer.compact_blocks_enabled &&
                                   peer.compact_blocks_announce &&
                                   peer.compact_blocks_version >= 1;
                        };

                        const auto peer_sync_height = [](const auto& peer) {
                            return std::max(
                                std::max(peer.synced_blocks, peer.synced_headers),
                                std::max(peer.best_known_height, peer.start_height));
                        };

                        constexpr uint32_t kCompactRelayLagAllowance = 2;
                        const auto peer_ready_for_compact = [&](const auto& peer) {
                            if (!peer_negotiated_compact(peer)) {
                                return false;
                            }
                            if (local_tip_height == 0) {
                                return true;
                            }

                            const uint32_t known_height = peer_sync_height(peer);
                            return known_height + kCompactRelayLagAllowance >= local_tip_height;
                        };

                        if (command == "cmpctblock") {
                            int sent = 0;
                            int deferred = 0;
                            for (const auto& peer : p2p_service->get().get_connected_peers()) {
                                if (!peer_ready_for_compact(peer)) {
                                    if (peer_negotiated_compact(peer)) {
                                        ++deferred;
                                    }
                                    continue;
                                }

                                // CRITICAL: relay-virtual peers use just `address` as their map key
                                // (no :port suffix) — see PeerInfo::to_string(). Without this branch,
                                // every block broadcast silently skips relay-virtual peers because
                                // the wrong key (with trailing :0) doesn't match connected_peers_.
                                const std::string peer_key = peer.via_relay.has_value()
                                    ? peer.address
                                    : peer.to_string();
                                if (p2p_service->get().send_to_peer(peer_key, msg)) {
                                    ++sent;
                                }
                            }
                            g_logger.info("[BlockRelay] Broadcast cmpctblock to " + std::to_string(sent) +
                                          " near-tip peer(s), deferred " + std::to_string(deferred) +
                                          " lagging compact peer(s) to inv");
                            return;
                        }

                        if (command == "inv") {
                            int sent = 0;
                            for (const auto& peer : p2p_service->get().get_connected_peers()) {
                                // Send inv to ALL peers — compact-ready peers already have the block
                                // via cmpctblock and will ignore this, but it guarantees no peer
                                // is silently skipped (e.g. on reconnect where cmpctblock was missed).
                                // CRITICAL: relay-virtual peers use just `address` as their map key
                                // (no :port suffix) — see PeerInfo::to_string(). Without this branch,
                                // every block broadcast silently skips relay-virtual peers because
                                // the wrong key (with trailing :0) doesn't match connected_peers_.
                                const std::string peer_key = peer.via_relay.has_value()
                                    ? peer.address
                                    : peer.to_string();
                                if (p2p_service->get().send_to_peer(peer_key, msg)) {
                                    ++sent;
                                }
                            }
                            g_logger.info("[BlockRelay] Broadcast inv to " + std::to_string(sent) +
                                          " peer(s) (universal fallback)");
                            return;
                        }

                        // Broadcast to all peers
                        p2p_service->BroadcastMessage(msg);
                    } else {
                        // Send to specific peer
                        p2p_service->get().send_to_peer(peer_address, msg);
                    }
                });

                // Wire BlockRelayManager validate callback (BlockRelay → Consensus)
                auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
                auto* block_ingress = ctx_.block_ingress;
                if (chainstate) {
                    block_relay->SetValidateBlockCallback([block_ingress, chainstate, p2p_service, block_download](
                        const Block& block,
                        const std::string& peer_address
                    ) -> bool {
                        try {
                            if (!block_ingress) {
                                g_logger.error("[BlockRelay] Block ingress not available");
                                return false;
                            }

                            const auto accept_result = block_ingress->Submit(block, BlockOrigin::P2P);
                            if (!accept_result.accepted()) {
                                return false;
                            }

                            if (block_download) {
                                if (auto* tip = chainstate->GetActiveTip()) {
                                    block_download->SetLocalTipHeight(static_cast<uint32_t>(tip->height));
                                }
                            }

                            if (p2p_service && chainstate->GetChainDB()) {
                                auto height_result = chainstate->GetChainDB()->getBlockHeight(block.GetHash());
                                if (height_result.status() == Status::Ok) {
                                    p2p_service->get().update_peer_synced_blocks(peer_address, height_result.value());
                                }
                            }

                            return true;
                        } catch (const std::exception& e) {
                            g_logger.error("[BlockRelay] Block validation failed: " + std::string(e.what()));
                            return false;
                        }
                    });

                    // Wire BlockRelayManager retrieve callback (BlockRelay → ChainDB)
                    block_relay->SetRetrieveBlockCallback([chainstate](
                        const uint256& block_hash,
                        Block& out_block
                    ) -> bool {
                        try {
                            std::cout << "[BlockRelay-Retrieve] Looking for block: " << block_hash.GetHex() << std::endl;

                            // Retrieve block from ChainDB
                            ChainDB* chain_db = chainstate->GetChainDB();
                            if (!chain_db) {
                                std::cout << "[BlockRelay-Retrieve] ERROR: ChainDB not available!" << std::endl;
                                g_logger.error("[BlockRelay] ChainDB not available");
                                return false;
                            }

                            std::cout << "[BlockRelay-Retrieve] Calling chainstate->getBlockByHash()..." << std::endl;
                            auto result = chainstate->getBlockByHash(block_hash);
                            if (!result.ok()) {
                                std::cout << "[BlockRelay-Retrieve] Block NOT FOUND - status: " << static_cast<int>(result.status()) << std::endl;
                                g_logger.debug("[BlockRelay] Block not found: " +
                                             block_hash.GetHex().substr(0, 16) + "...");
                                return false;
                            }

                            std::cout << "[BlockRelay-Retrieve] Block FOUND!" << std::endl;
                            out_block = result.value();
                            return true;
                        } catch (const std::exception& e) {
                            std::cout << "[BlockRelay-Retrieve] EXCEPTION: " << e.what() << std::endl;
                            g_logger.error("[BlockRelay] Block retrieval failed: " + std::string(e.what()));
                            return false;
                        }
                    });

                    // Phase G.7: Wire BlockRelayManager has_block callback (orphan detection)
                    block_relay->SetHasBlockCallback([chainstate](const uint256& block_hash) -> bool {
                        try {
                            ChainDB* chain_db = chainstate->GetChainDB();
                            if (!chain_db) {
                                return false;
                            }
                            return chainstate->hasBlockByHash(block_hash);
                        } catch (...) {
                            return false;
                        }
                    });
                    std::cout << "[DaemonApp] ✅ Phase G.7 HasBlockCallback wired (orphan detection)" << std::endl;

                    // P2P FIX: Wire ChainDB to BlockRelayManager for parent-check in HandleInv
                    ChainDB* brm_chain_db = chainstate->GetChainDB();
                    if (brm_chain_db) {
                        block_relay->SetChainDB(brm_chain_db);
                        std::cout << "[DaemonApp] ✅ BlockRelayManager.SetChainDB() wired (P2P parent-check)" << std::endl;
                    } else {
                        std::cerr << "[DaemonApp] ⚠️ WARNING: ChainDB is NULL, P2P parent-check DISABLED!" << std::endl;
                    }

                    // Phase P.2: Wire BlockRelayManager block status callback (pruned vs unknown vs corrupted)
                    block_relay->SetGetBlockStatusCallback([chainstate](const uint256& block_hash)
                        -> BlockRelayManager::BlockDataStatus {
                        try {
                            ChainDB* chain_db = chainstate->GetChainDB();
                            if (!chain_db) {
                                return BlockRelayManager::BlockDataStatus::Unknown;
                            }

                            // Check if block index exists
                            CBlockIndex* pindex = chain_db->getBlockIndex(block_hash);
                            if (!pindex) {
                                // Block hash not in our index - never seen it
                                return BlockRelayManager::BlockDataStatus::Unknown;
                            }

                            // Block index exists - check if data is available
                            if (pindex->status & BLOCK_HAVE_DATA) {
                                // Flag says data should exist - if retrieval failed, it's corrupted
                                return BlockRelayManager::BlockDataStatus::Corrupted;
                            } else {
                                // Flag cleared - block was intentionally pruned
                                return BlockRelayManager::BlockDataStatus::Pruned;
                            }
                        } catch (...) {
                            return BlockRelayManager::BlockDataStatus::Unknown;
                        }
                    });
                    std::cout << "[DaemonApp] ✅ Phase P.2 GetBlockStatusCallback wired (pruned vs unknown)" << std::endl;
                }

                std::cout << "[DaemonApp] ✅ Phase G.2 BlockRelayManager callbacks wired" << std::endl;
            }

            // Phase G.3: Wire TxRelayManager callbacks
            if (ctx_.tx_relay) {
                auto tx_relay = ctx_.tx_relay;

                // Wire TxRelayManager send callback (TxRelay → P2P)
                tx_relay->SetSendMessageCallback([p2p_service](
                    const std::string& peer_address,
                    const std::string& command,
                    const std::vector<uint8_t>& payload
                ) {
                    ::P2PMessage msg;
                    msg.command = command;
                    msg.payload = payload;
                    msg.checksum = 0;  // P2PManager will calculate

                    if (peer_address.empty()) {
                        // Broadcast to all peers
                        p2p_service->BroadcastMessage(msg);
                    } else {
                        // Send to specific peer
                        p2p_service->get().send_to_peer(peer_address, msg);
                    }
                });

                // Wire TxRelayManager structured submit callback (TxRelay → MempoolService)
                // Uses submitTransaction() for structured results (enables orphan pool)
                auto mempool = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);
                if (mempool) {
                    tx_relay->SetSubmitTxCallback([mempool](
                        const Transaction& tx,
                        const std::string& peer_address
                    ) -> TxAcceptResult {
                        try {
                            // Route to MempoolService for mempool validation
                            // relay=false: TxRelayManager handles relay after acceptance
                            return mempool->submitTransaction(tx, "p2p:" + peer_address, false);
                        } catch (const std::exception& e) {
                            g_logger.error("[TxRelay] Transaction validation failed: " + std::string(e.what()));
                            return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX, e.what());
                        }
                    });

                    // Wire TxRelayManager retrieve callback (TxRelay → MempoolService)
                    tx_relay->SetRetrieveTxCallback([mempool](
                        const uint256& txid,
                        Transaction& out_tx
                    ) -> bool {
                        try {
                            // Retrieve transaction from mempool
                            auto tx_ptr = mempool->getTransaction(txid);
                            if (!tx_ptr) {
                                g_logger.debug("[TxRelay] Transaction not found in mempool: " +
                                             txid.GetHex().substr(0, 16) + "...");
                                return false;
                            }

                            out_tx = *tx_ptr;
                            return true;
                        } catch (const std::exception& e) {
                            g_logger.error("[TxRelay] Transaction retrieval failed: " + std::string(e.what()));
                            return false;
                        }
                    });
                }

                std::cout << "[DaemonApp] ✅ Phase G.3 TxRelayManager callbacks wired" << std::endl;
            }

            // ================================================================
            // Wire Phase N Send Callbacks (Phase N → P2PService)
            // ================================================================

            // HeaderSyncP2P → SendGetheaders
            header_sync->SetSendGetheadersCallback([p2p_service](
                uint64_t peer_id,
                const std::vector<uint256>& locator,
                const uint256& hash_stop
            ) {
                std::string peer_addr = GetPeerAddress(peer_id);
                if (peer_addr.empty()) {
                    g_logger.warning("[Phase N] Cannot send getheaders: Unknown peer ID " + std::to_string(peer_id));
                    return;
                }

                // Convert uint256 locator to hex strings
                std::vector<std::string> locator_hex;
                for (const auto& hash : locator) {
                    locator_hex.push_back(hash.GetHex());
                }

                ::P2PMessage msg = ::P2PMessage::create_getheaders(locator_hex);
                bool sent = p2p_service->get().send_to_peer(peer_addr, msg);

                if (sent) {
                    g_logger.info("[Phase N] Sent getheaders to " + peer_addr +
                                 " (locator size: " + std::to_string(locator.size()) + ")");
                } else {
                    g_logger.warning("[Phase N] Failed to send getheaders to " + peer_addr);
                }
            });

            // HeaderSyncP2P → SendHeaders (for responding to getheaders requests)
            header_sync->SetSendHeadersCallback([p2p_service](
                uint64_t peer_id,
                const std::vector<BlockHeader>& headers
            ) {
                std::string peer_addr = GetPeerAddress(peer_id);
                if (peer_addr.empty()) {
                    g_logger.warning("[Phase N] Cannot send headers: Unknown peer ID " + std::to_string(peer_id));
                    return;
                }

                // Serialize headers to hex strings
                std::vector<std::string> header_hexes;
                for (const auto& header : headers) {
                    header_hexes.push_back(header.Serialize());
                }

                ::P2PMessage msg = ::P2PMessage::create_headers(header_hexes);
                bool sent = p2p_service->get().send_to_peer(peer_addr, msg);

                if (sent) {
                    g_logger.info("[Phase N] Sent " + std::to_string(headers.size()) + " headers to " + peer_addr);
                } else {
                    g_logger.warning("[Phase N] Failed to send headers to " + peer_addr);
                }
            });

            // HeaderSyncP2P → DisconnectPeer (on peer switch reasons)
            header_sync->SetDisconnectPeerCallback([p2p_service](
                uint64_t peer_id,
                dinero::consensus::PeerSwitchReason reason
            ) {
                std::string peer_addr = GetPeerAddress(peer_id);
                if (peer_addr.empty()) {
                    g_logger.warning("[Phase N] Cannot disconnect peer: Unknown peer ID " + std::to_string(peer_id));
                    return;
                }

                std::string reason_str = "UNKNOWN";
                switch (reason) {
                    case dinero::consensus::PeerSwitchReason::STALL_TIMEOUT:
                        reason_str = "STALL_TIMEOUT";
                        break;
                    case dinero::consensus::PeerSwitchReason::INVALID_HEADERS:
                        reason_str = "INVALID_HEADERS";
                        break;
                    case dinero::consensus::PeerSwitchReason::PEER_DISCONNECT:
                        reason_str = "PEER_DISCONNECT";
                        break;
                    case dinero::consensus::PeerSwitchReason::SYNC_COMPLETE:
                        reason_str = "SYNC_COMPLETE";
                        break;
                    case dinero::consensus::PeerSwitchReason::NO_PROGRESS:
                        reason_str = "NO_PROGRESS";
                        break;
                }

                g_logger.info("[Phase N] Disconnecting peer " + peer_addr + " (reason: " + reason_str + ")");
                p2p_service->get().disconnect_peer(peer_addr);
            });

            // BlockDownloadScheduler → SendGetdata
            // Phase P.3: CSN requests MSG_UTREEXO_BLOCK (block + proof) instead of MSG_BLOCK
            bool csn_mode = GetConfig().utreexo_stateless;
            auto csn_bridge_rr_index = std::make_shared<std::atomic<size_t>>(0);
            block_download->SetSendGetDataCallback([p2p_service, csn_mode, csn_bridge_rr_index](const uint256& block_hash) {
                // Use binary format with raw bytes (not hex) to preserve correct byte order
                uint32_t inv_type = csn_mode
                    ? static_cast<uint32_t>(dinero::InventoryType::MSG_UTREEXO_BLOCK)
                    : static_cast<uint32_t>(dinero::InventoryType::MSG_BLOCK);
                ::P2PMessage msg = ::P2PMessage::create_getdata_binary(
                    block_hash.begin(), 32, inv_type
                );
                // Send to each connected peer directly (synchronous send_to_peer)
                // instead of broadcast_message (async outbox queue which may be
                // congested during startup and silently drop messages).
                auto peers = p2p_service->get().get_connected_peers();
                int sent = 0;
                int eligible = 0;

                if (csn_mode) {
                    std::vector<std::string> archival_bridge_peers;
                    std::vector<std::string> limited_bridge_peers;
                    archival_bridge_peers.reserve(peers.size());
                    limited_bridge_peers.reserve(peers.size());
                    for (const auto& peer : peers) {
                        std::string peer_key = peer.to_string();
                        if (!p2p_service->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_UTREEXO_BRIDGE)) {
                            continue;
                        }
                        if (p2p_service->get().peer_has_service_flags(peer_key, ServiceFlags::NODE_NETWORK)) {
                            archival_bridge_peers.push_back(std::move(peer_key));
                        } else {
                            limited_bridge_peers.push_back(std::move(peer_key));
                        }
                    }

                    const std::vector<std::string>* selected_bridge_peers = nullptr;
                    if (!archival_bridge_peers.empty()) {
                        selected_bridge_peers = &archival_bridge_peers;
                    } else if (!limited_bridge_peers.empty()) {
                        selected_bridge_peers = &limited_bridge_peers;
                        g_logger.warning("[CSN] No archival NODE_UTREEXO_BRIDGE peers available for block " +
                                         block_hash.GetHex().substr(0, 16) +
                                         "...; falling back to limited bridge peers");
                    }

                    if (selected_bridge_peers && !selected_bridge_peers->empty()) {
                        // For historical CSN catch-up, partial fanout can deadlock progress if the
                        // few selected peers are lagging, overloaded, or silently drop utxoblk.
                        // Prefer liveness over bandwidth here: send block requests to every
                        // eligible bridge peer and let duplicate responses be discarded.
                        size_t fanout = selected_bridge_peers->size();
                        size_t start = csn_bridge_rr_index->fetch_add(1, std::memory_order_relaxed);
                        eligible = static_cast<int>(selected_bridge_peers->size());
                        for (size_t i = 0; i < fanout; ++i) {
                            const std::string& peer_key =
                                (*selected_bridge_peers)[(start + i) % selected_bridge_peers->size()];
                            if (p2p_service->get().send_to_peer(peer_key, msg)) {
                                sent++;
                            }
                        }
                    } else {
                        // Backward-compatible fallback for mixed networks without bridge advertisements.
                        g_logger.warning("[CSN] No NODE_UTREEXO_BRIDGE peers available for block " +
                                         block_hash.GetHex().substr(0, 16) +
                                         "...; falling back to all peers");
                        eligible = static_cast<int>(peers.size());
                        for (const auto& peer : peers) {
                            std::string peer_key = peer.to_string();
                            if (p2p_service->get().send_to_peer(peer_key, msg)) {
                                sent++;
                            }
                        }
                    }
                } else {
                    eligible = static_cast<int>(peers.size());
                    for (const auto& peer : peers) {
                        std::string peer_key = peer.to_string();
                        if (p2p_service->get().send_to_peer(peer_key, msg)) {
                            sent++;
                        }
                    }
                }
                g_logger.info("[Phase N] Sent getdata(" +
                             std::string(csn_mode ? "MSG_UTREEXO_BLOCK" : "MSG_BLOCK") +
                             ") for block " + block_hash.GetHex() +
                             " to " + std::to_string(sent) + "/" +
                             std::to_string(eligible) + " eligible peers");
            });

            // BlockDownloadScheduler → ConnectBlock (drains stored blocks to chainstate in order)
            {
                auto chainstate_for_drain = ctx_.chainstate;
                auto prune_for_drain = ctx_.prune;
                block_download->SetConnectBlockCallback(
                    [chainstate_for_drain, prune_for_drain](const Block& block, const std::string& source) -> dinero::consensus::ConnectBlockResult {
                        if (!chainstate_for_drain) {
                            return dinero::consensus::ConnectBlockResult::TEMPORARY_FAIL;
                        }
                        auto result = chainstate_for_drain->ProcessIncomingStoredBlock(block, source);
                        if (result == dinero::consensus::ConnectBlockResult::CONNECTED && prune_for_drain) {
                            prune_for_drain->triggerPruneIfNeeded();
                        }
                        return result;
                    }
                );

                // BlockDownloadScheduler → GetTipHeight (queries actual chainstate tip for drain ordering)
                block_download->SetGetTipHeightCallback(
                    [chainstate_for_drain]() -> uint32_t {
                        if (!chainstate_for_drain) return 0;
                        auto* tip = chainstate_for_drain->GetActiveTip();
                        return tip ? tip->height : 0;
                    }
                );

                // In CSN/stateless mode, ordered proof validation drives
                // activation. The scheduler should download/store blocks, but
                // must not connect flat-file bodies directly.
                block_download->SetStatelessMode(GetConfig().utreexo_stateless);

                // BlockDownloadScheduler → GetBlockHashAtHeight (fork detection in ScanForMissingBlocks)
                block_download->SetGetBlockHashAtHeightCallback(
                    [chainstate_for_drain](uint32_t height, uint256& out_hash) -> bool {
                        if (!chainstate_for_drain) return false;
                        return consensus::GetActiveChainHashAtHeight(
                            chainstate_for_drain->GetActiveTip(),
                            height,
                            out_hash
                        );
                    }
                );
            }

            // BlockDownloadScheduler → DisconnectPeer (on invalid block)
            block_download->SetDisconnectPeerCallback([p2p_service](
                uint64_t peer_id,
                const std::string& reason
            ) {
                std::string peer_addr = GetPeerAddress(peer_id);
                if (peer_addr.empty()) {
                    g_logger.warning("[Phase N] Cannot disconnect peer: Unknown peer ID " + std::to_string(peer_id));
                    return;
                }

                g_logger.info("[Phase N] Disconnecting peer " + peer_addr + " (reason: " + reason + ")");
                p2p_service->get().disconnect_peer(peer_addr);
            });

            std::cout << "[DaemonApp] ✅ Phase N components wired to P2PService (bidirectional)" << std::endl;
            std::cout << "[DaemonApp]    - Receive: OnHeaders → HeaderSyncP2P, OnNewBlock → BlockDownloadScheduler" << std::endl;
            std::cout << "[DaemonApp]    - Send: HeaderSyncP2P/BlockDownloadScheduler → P2PService" << std::endl;

            if (ctx_.block_download && ctx_.header_chain && ctx_.chainstate) {
                auto* best = ctx_.header_chain->GetBestHeader();
                auto* active = ctx_.chainstate->GetActiveTip();
                const auto peers = p2p_service->get().get_connected_peers();
                if (best && active && best->height > active->height && !peers.empty()) {
                    std::cout << "[DaemonApp] Bootstrapping block download from persisted header backlog "
                              << "(header=" << best->height
                              << ", active=" << active->height
                              << ", peers=" << peers.size() << ")" << std::endl;
                    block_download->OnHeadersProcessed();
                    block_download->Tick();
                }
            }

            // ═══════════════════════════════════════════════════════════════════════════
            // Phase G: Wire Parallel Block Download Scheduler (10-20× IBD speedup)
            // ═══════════════════════════════════════════════════════════════════════════
            if (ctx_.parallel_block_download) {
                std::cout << "[DaemonApp] Wiring ParallelBlockDownloadScheduler..." << std::endl;

                auto parallel_download = ctx_.parallel_block_download;
                if (ctx_.peer_scoring) {
                    auto peer_scoring = ctx_.peer_scoring;
                    parallel_download->setPeerScoreProvider(
                        [peer_scoring](const peer_id_t& peer_id) -> double {
                            if (!peer_scoring) {
                                return -1.0;
                            }
                            const int32_t misbehavior_score = peer_scoring->getScore(peer_id);
                            return std::max(0.0, 100.0 - static_cast<double>(misbehavior_score));
                        }
                    );
                }
                std::vector<peer_id_t> connected_peer_ids;
                for (const auto& peer : p2p_service->get().get_connected_peers()) {
                    connected_peer_ids.push_back(peer.to_string());
                }
                if (!connected_peer_ids.empty()) {
                    parallel_download->registerPeers(connected_peer_ids);
                }

                // Wire block received notification
                // When a block is received, notify the parallel scheduler
                auto existing_on_block = p2p_service->OnNewBlock;
                p2p_service->OnNewBlock = [existing_on_block, parallel_download](
                    const std::string& peer_addr,
                    const ::P2PMessage& msg
                ) {
                    // Call existing handler first
                    if (existing_on_block) {
                        existing_on_block(peer_addr, msg);
                    }

                    // Notify parallel scheduler
                    if (parallel_download) {
                        try {
                            Block block = DeserializeBlockFromP2PMessage(msg);
                            parallel_download->notifyBlockReceived(block.GetHash());
                        } catch (...) {
                            // Block deserialization already logged by existing handler
                        }
                    }
                };

                // Process the scheduler queue periodically (via OnTick if available, or rely on block_download->Tick())
                std::cout << "[DaemonApp] ✅ ParallelBlockDownloadScheduler wired (Phase G)" << std::endl;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // CONSENSUS PARAMETERS LOG (runtime breadcrumb)
    // ═══════════════════════════════════════════════════════════════════
    {
        Consensus consensus;
        char pow_limit_hex[11], anchor_hex[11];
        std::snprintf(pow_limit_hex, sizeof(pow_limit_hex), "0x%08x", consensus.powLimitBits);
        std::snprintf(anchor_hex, sizeof(anchor_hex), "0x%08x", consensus.asertAnchorBits);
        std::cout << "\n[DaemonApp] ═══════════════════════════════════════════════════════" << std::endl;
        std::cout << "[DaemonApp] CONSENSUS DIFFICULTY PARAMETERS (ASERT from block 1)" << std::endl;
        std::cout << "[DaemonApp]   powLimitBits (floor):    " << pow_limit_hex << " (50× easier than Bitcoin)" << std::endl;
        std::cout << "[DaemonApp]   asertAnchorBits:         " << anchor_hex << std::endl;
        std::cout << "[DaemonApp]   asertAnchorHeight:       " << consensus.asertAnchorHeight << std::endl;
        std::cout << "[DaemonApp]   asertHalfLifeSec:        " << consensus.asertHalfLifeSec << " sec" << std::endl;
        std::cout << "[DaemonApp] ═══════════════════════════════════════════════════════\n" << std::endl;
    }

    std::cout << "[DaemonApp] All services initialized successfully" << std::endl;
    init_succeeded = true;
    return true;
}

bool DaemonApp::Start() {
    if (started_) {
        std::cerr << "[DaemonApp] Already started" << std::endl;
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.2: Load mining restart state at daemon startup
    // ═══════════════════════════════════════════════════════════════════
    //
    // Contract: E.3 - Restart Semantics
    // - Set is_fresh_start = true (this is a daemon restart)
    // - Load mining_was_active_before from persistence
    // - Do NOT auto-start mining (services will respect this flag)
    //
    // CRITICAL: Must happen BEFORE services start (mining needs this state)
    // ═══════════════════════════════════════════════════════════════════

    std::cout << "[DaemonApp] Loading mining restart state..." << std::endl;

    // Set is_fresh_start = true (this is a daemon restart)
    ctx_.is_fresh_start = true;

    // Load mining_was_active_before from persistence
    std::string mining_state_path = "mining_state.dat";
    if (ctx_.config) {
        auto config = std::dynamic_pointer_cast<ConfigService>(ctx_.config);
        if (config) {
            mining_state_path = config->DataDir() + "/mining_state.dat";
        }
    }

    // Best effort load - never blocks startup
    try {
        std::ifstream file(mining_state_path, std::ios::binary);
        if (file.is_open()) {
            // Simple binary format: 1 byte boolean
            uint8_t state = 0;
            file.read(reinterpret_cast<char*>(&state), sizeof(state));
            file.close();

            ctx_.mining_was_active_before = (state != 0);

            std::cout << "[DaemonApp] ✅ Mining restart state loaded: mining_was_active_before="
                      << (ctx_.mining_was_active_before ? "true" : "false") << std::endl;
        } else {
            // No persisted state (first run or clean state)
            ctx_.mining_was_active_before = false;
            std::cout << "[DaemonApp] ℹ️  No mining state file found (first run or clean state)" << std::endl;
        }
    } catch (const std::exception& e) {
        ctx_.mining_was_active_before = false;
        std::cout << "[DaemonApp] ⚠️  Mining state load error: " << e.what() << " (non-fatal, defaulting to false)" << std::endl;
    }

    std::cout << "[DaemonApp] ✅ Restart state initialized: is_fresh_start=true, mining_was_active_before="
              << (ctx_.mining_was_active_before ? "true" : "false") << std::endl;

    // ═══════════════════════════════════════════════════════════════════
    // Phase R.9: Wire Ring Signature DBs to BlockValidator and Mempool
    // ═══════════════════════════════════════════════════════════════════
    // Must be done AFTER services Init() (BlockValidator created during
    // ChainstateService::Init()) but BEFORE services Start() so that
    // ChainstateService::Start() → ActivateBestChain() → ConnectBlock()
    if (ctx_.mempool && ctx_.mempool->isInitialized() && ctx_.chainstate) {
        auto& mempool = ctx_.mempool->mempool();
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        if (chainstate) {
            mempool.setShieldedState(chainstate->GetShieldedCommitmentTree(),
                                     chainstate->GetShieldedNullifierSet());
            std::cout << "[DaemonApp] ✅ Shielded state wired to Mempool" << std::endl;
        }
    }

    std::cout << "[DaemonApp] Starting core services..." << std::endl;

    for (auto& service : services_) {
        if (service == ctx_.rpc) {
            std::cout << "[DaemonApp] Deferring " << service->Name()
                      << " listener start until startup recovery completes" << std::endl;
            continue;
        }
        std::cout << "[DaemonApp] Starting " << service->Name() << "..." << std::endl;
        if (!service->Start()) {
            std::cerr << "[DaemonApp] ❌ Failed to start " << service->Name() << std::endl;
            // Stop services that were already started
            Stop();
            return false;
        }
        std::cout << "[DaemonApp] ✅ " << service->Name() << " started" << std::endl;
    }

    std::cout << "[DaemonApp] ✅ Core services started" << std::endl;

    // Marker discipline: advisory recovery markers may survive a crash, but
    // they must not outlive a later healthy canonical startup forever.
    {
        const std::filesystem::path post_start_data_dir =
            ctx_.config ? std::filesystem::path(std::dynamic_pointer_cast<ConfigService>(ctx_.config)->DataDir())
                        : std::filesystem::path{};
        std::string marker_error;
        const auto recovery_marker =
            daemon::ReadChainstateRecoveryMarker(post_start_data_dir, &marker_error);
        if (recovery_marker.has_value()) {
            auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
            if (chainstate) {
                std::string alignment_reason;
                const auto undo_report =
                    chainstate->AuditUndoMetadataForRestamp(/*max_blocks_back=*/1024,
                                                            /*apply=*/false,
                                                            /*include_ok=*/false);
                if (undo_report.restampable > 0 || undo_report.failed > 0) {
                    std::cout << "[DaemonApp] ℹ️  Retaining chainstate recovery marker: undo metadata "
                                 "audit still reports restampable="
                              << undo_report.restampable
                              << " failed=" << undo_report.failed << "\n";
                } else if (chainstate->IsCanonicalStateAligned(&alignment_reason)) {
                    std::string clear_error;
                    if (!daemon::ClearChainstateRecoveryMarker(post_start_data_dir, &clear_error)) {
                        std::cerr << "[DaemonApp] ⚠️  Canonical state is healthy, but failed to clear stale "
                                     "chainstate recovery marker: "
                                  << clear_error << std::endl;
                    } else {
                        std::cout << "[DaemonApp] ✅ Cleared stale chainstate recovery marker after healthy startup\n";
                    }
                } else {
                    std::cout << "[DaemonApp] ℹ️  Retaining chainstate recovery marker: canonical state not yet "
                                 "aligned ("
                              << alignment_reason << ")\n";
                }
            }
        } else if (!marker_error.empty()) {
            std::cerr << "[DaemonApp] ⚠️  Failed to read chainstate recovery marker post-start: "
                      << marker_error << std::endl;
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Wire BlockAssembler to Mempool (deferred from InitializeServices)
    // MUST happen after services Init() because Mempool is created in Init()
    // ═══════════════════════════════════════════════════════════════════
    if (ctx_.block_assembler && ctx_.mempool && ctx_.mempool->isInitialized()) {
        ctx_.block_assembler->setMempool(&ctx_.mempool->mempool());
        std::cout << "[DaemonApp] ✅ BlockAssembler wired to Mempool" << std::endl;
    } else if (ctx_.block_assembler) {
        std::cerr << "[DaemonApp] ⚠️  BlockAssembler created but Mempool not available" << std::endl;
    }

    // Phase 2: Initialize ParallelBlockValidator with IConsensusUTXOSet directly (no adapter chain)
    if (ctx_.chainstate && ctx_.chainstate_guard) {
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        if (chainstate) {
            auto* utxo_set = chainstate->GetConsensusUTXOSet();
            if (utxo_set) {
                auto validator_config = dinero::consensus::ParallelBlockValidator::Config::forNormalOperation();
                validator_config.parallel_threshold = 10;

                std::shared_ptr<dinero::consensus::ParallelBlockValidator> parallel_validator(
                    new dinero::consensus::ParallelBlockValidator(
                        utxo_set,
                        ctx_.chainstate_guard.get(),
                        nullptr,  // block_storage (optional)
                        validator_config
                    )
                );

                ctx_.parallel_validator = parallel_validator;

                auto* forest = &utxo_set->GetForest();
                std::cout << "[DaemonApp] ParallelBlockValidator initialized (threshold: "
                          << validator_config.parallel_threshold << " tx)" << std::endl;
                std::cout << "[DaemonApp]    Forest leaves: " << forest->getNumLeaves() << std::endl;
                std::cout << "[DaemonApp]    Workers: " << (validator_config.worker_threads == 0 ? "auto" : std::to_string(validator_config.worker_threads))
                          << ", Parallel: " << (validator_config.enable_parallel ? "enabled" : "disabled") << std::endl;
            } else {
                std::cout << "[DaemonApp] ParallelBlockValidator skipped (ConsensusUTXOSet not available)" << std::endl;
            }
        }
    } else {
        std::cout << "[DaemonApp] ParallelBlockValidator skipped (chainstate not initialized)" << std::endl;
    }

    // Phase 6B: Instantiate ValidationQueue as the authoritative scheduler for
    // network block ingress. The queue owns ordering/backpressure/prevalidation,
    // but final acceptance still hands off to the canonical BlockAcceptor path.
    if (ctx_.chainstate && ctx_.chainstate_guard) {
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        if (chainstate) {
            auto* utxo_set = chainstate->GetConsensusUTXOSet();
            if (utxo_set) {
                auto queue_config = chainstate->IsInIBD()
                    ? dinero::consensus::ValidationQueue::Config::forIBD()
                    : dinero::consensus::ValidationQueue::Config::forNormalOperation();

                ctx_.validation_queue = std::make_shared<dinero::consensus::ValidationQueue>(
                    utxo_set,
                    ctx_.chainstate_guard.get(),
                    queue_config
                );

                ctx_.validation_queue->setParentReadyCallback([chainstate](const uint256& prev_hash) -> bool {
                    if (prev_hash.IsNull()) {
                        return true;
                    }
                    return chainstate && chainstate->hasBlockByHash(prev_hash);
                });

                ctx_.validation_queue->setBlockApplyCallback([](const Block& block) -> BlockAcceptResult {
                    return BlockAcceptor::AcceptBlockFromPeer(block, "validation-queue");
                });

                ctx_.validation_queue->start();
                std::cout << "[DaemonApp] ValidationQueue started (mode: "
                          << (chainstate->IsInIBD() ? "IBD" : "normal")
                          << ", max_in_flight=" << queue_config.max_in_flight_blocks
                          << ", max_queued=" << queue_config.max_queued_blocks << ")" << std::endl;
            } else {
                std::cout << "[DaemonApp] ValidationQueue skipped (ConsensusUTXOSet not available)" << std::endl;
            }
        }
    } else {
        std::cout << "[DaemonApp] ValidationQueue skipped (chainstate not initialized)" << std::endl;
    }

    // Phase 10e: Wire Utreexo forest and UTXO provider to BlockAssembler for block template commitments
    // This enables stratum-mined blocks to have valid Utreexo roots
    // Must be done AFTER chainstate->Init() because Utreexo forest is created during Init()
    if (ctx_.chainstate && ctx_.block_assembler) {
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_.chainstate);
        if (chainstate) {
            auto utreexo_forest = chainstate->utreexoForest();
            auto utxo_index = chainstate->utxoIndex();

            if (utreexo_forest && utxo_index) {
                ctx_.block_assembler->SetUtreexoForest(utreexo_forest);
                // v2.2.0: Create adapter to bridge wallet UTXOIndex to consensus IUTXOProvider
                // BlockAssembler takes shared_ptr<IUTXOProvider> - lifetime managed automatically
                auto assembler_utxo_adapter = std::make_shared<consensus::WalletUTXOAdapter>(utxo_index);
                ctx_.block_assembler->SetUTXOProvider(assembler_utxo_adapter);

                // Wire BlockValidator for Utreexo root computation (single source of truth)
                // This ensures BlockAssembler uses the same ComputeUtreexoRootPure as validation
                auto block_validator = chainstate->GetBlockValidator();
                if (block_validator) {
                    ctx_.block_assembler->SetBlockValidator(block_validator);
                    std::cout << "[DaemonApp] ✅ BlockValidator wired to BlockAssembler (Utreexo oracle)" << std::endl;
                }

                std::cout << "[DaemonApp] ✅ Utreexo forest and UTXO provider wired to BlockAssembler (Phase 10e)" << std::endl;
                // Note: BridgeNode for Phase P.2 is created earlier where OnGetData lambda is set up
            } else {
                if (!utreexo_forest) {
                    std::cout << "[DaemonApp] ⚠️  Utreexo forest not available - mining templates will have zero roots" << std::endl;
                }
                if (!utxo_index) {
                    std::cout << "[DaemonApp] ⚠️  UTXO index not available - mining templates will have zero roots" << std::endl;
                }
            }
        }
    } else {
        std::cout << "[DaemonApp] ⚠️  Phase 10e skipped (BlockAssembler or chainstate not initialized)" << std::endl;
    }

    // Step B: Create per-service JSON loggers for separate log files.
    // Bring these up before mempool reload so startup recovery is captured in
    // the per-service logs too.
    std::cout << "[DaemonApp] Initializing per-service JSON loggers..." << std::endl;

    // Get data directory for log file paths
    std::string datadir = ctx_.config ?
        std::dynamic_pointer_cast<ConfigService>(ctx_.config)->DataDir() :
        "~/.dinero";

    wallet_logger_ = std::make_unique<JsonLogger>(datadir + "/wallet.log", "wallet");
    p2p_logger_ = std::make_unique<JsonLogger>(datadir + "/p2p.log", "p2p");
    mining_logger_ = std::make_unique<JsonLogger>(datadir + "/mining.log", "mining");
    mempool_logger_ = std::make_unique<JsonLogger>(datadir + "/mempool.log", "mempool");

    // Wire up the per-service loggers to the context
    ctx_.wallet_logger = wallet_logger_.get();
    ctx_.p2p_logger = p2p_logger_.get();
    ctx_.mining_logger = mining_logger_.get();
    ctx_.mempool_logger = mempool_logger_.get();

    std::cout << "[DaemonApp] ✅ Per-service JSON loggers initialized:" << std::endl;
    std::cout << "[DaemonApp]   - wallet.log (structured JSON)" << std::endl;
    std::cout << "[DaemonApp]   - p2p.log (structured JSON)" << std::endl;
    std::cout << "[DaemonApp]   - mining.log (structured JSON)" << std::endl;
    std::cout << "[DaemonApp]   - mempool.log (structured JSON)" << std::endl;

    // Unified Log Aggregator: Create LoggerRouter for real-time log streaming
    std::cout << "[DaemonApp] Initializing unified log aggregator..." << std::endl;
    logger_router_ = std::make_unique<LoggerRouter>(datadir, 1000);  // 1000-entry ring buffer
    ctx_.logger_router = logger_router_.get();
    logger_router_->start();  // Start tailing log files in background thread
    std::cout << "[DaemonApp] ✅ Unified log aggregator started (LoggerRouter)" << std::endl;

    // v0.13.0.2 Step C: Load mempool from disk after startup recovery.
    if (ctx_.mempool) {
        auto mempool_service = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);
        if (mempool_service) {
            std::cout << "[DaemonApp] Loading mempool from disk..." << std::endl;

            Mempool& mempool = mempool_service->mempool();
            std::string mempool_path = mempool.getDefaultMempoolPath();

            // Get data directory for correct path
            if (ctx_.config) {
                auto config = std::dynamic_pointer_cast<ConfigService>(ctx_.config);
                if (config) {
                    mempool_path = config->DataDir() + "/mempool.dat";
                }
            }

            // Best effort load - revalidates all transactions against current policy
            if (mempool.loadFromDisk(mempool_path)) {
                std::cout << "[DaemonApp] ✅ Mempool loaded from " << mempool_path << std::endl;
            } else {
                std::cout << "[DaemonApp] ℹ️  Mempool load failed or file not found (starting with empty mempool)" << std::endl;
            }
        }
    }

    started_ = true;
    std::cout << "[DaemonApp] Startup recovery complete; starting external listeners..." << std::endl;

    if (ctx_.rpc) {
        std::cout << "[DaemonApp] Starting " << ctx_.rpc->Name() << "..." << std::endl;
        if (!ctx_.rpc->Start()) {
            std::cerr << "[DaemonApp] ❌ Failed to start " << ctx_.rpc->Name() << std::endl;
            Stop();
            return false;
        }
        std::cout << "[DaemonApp] ✅ " << ctx_.rpc->Name() << " started" << std::endl;
    } else {
        std::cerr << "[DaemonApp] ⚠️  RPC service unavailable during listener startup" << std::endl;
    }

    // ========================================================================
    // Phase 3D: Wallet event notifications — REMOVED synchronous path
    // ========================================================================
    // WalletManager::onBlockConnected was called synchronously on the ConnectTip
    // thread, but WalletManager has no mutex. This raced with the WalletWorker
    // background thread (catch-up scan), causing "double free or corruption".
    // All wallet notifications now route through WalletWorker exclusively:
    //   ConnectTip → notifyBlockConnected → WalletNotify::OnBlockConnected → WalletWorker
    // The registerWalletNotifier call has been removed.
    // ========================================================================

    // Phase 3F: Wire WebSocket subscription manager to context.
    // This depends on RPC startup because the HTTP/WebSocket server owns
    // g_subscriptions.
    ctx_.websocket_subscriptions = g_subscriptions;
    if (g_subscriptions) {
        std::cout << "[DaemonApp] ✅ WebSocket subscriptions wired to context" << std::endl;
    } else {
        std::cout << "[DaemonApp] ℹ️  WebSocket subscriptions not available (g_subscriptions is nullptr)" << std::endl;
    }

    // TODO Week 2: Wire RPC context for context-aware handlers
    // Once HttpRpcServer is integrated into RPCService, call:
    //
    // #include "daemon/rpc_context_wiring.h"
    // if (ctx_.rpc) {
    //     auto rpc_service = std::dynamic_pointer_cast<RPCService>(ctx_.rpc);
    //     if (rpc_service && rpc_service->GetHttpServer()) {
    //         if (!WireRpcContext(ctx_, rpc_service->GetHttpServer())) {
    //             std::cerr << "[DaemonApp] Warning: Failed to wire RPC context" << std::endl;
    //         }
    //     }
    // }

    // ═══════════════════════════════════════════════════════════════════════════
    // Stratum V1 Mining Server - REMOVED FROM DINEROD
    // ═══════════════════════════════════════════════════════════════════════════
    // Stratum is now a separate binary: dinero-stratum
    // This follows Bitcoin Core's multiprocess pattern (bitcoind / bitcoin-wallet)
    //
    // To run Stratum mining:
    //   1. Start dinerod normally
    //   2. Run: dinero-stratum --rpcport=20998 --stratumport=3333
    //
    // Benefits:
    //   - Security isolation (Stratum attack surface separated from consensus)
    //   - Deployment flexibility (run on different machine)
    //   - Resource isolation (Stratum threads don't compete with consensus)
    //   - Clean codebase (dinerod compiles without stratum dependencies)
    // ═══════════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════
    // Phase 3: Start gRPC Server (dev mode only)
    // Phase 5: Start Socket Wallet Server (always enabled)
    // ═══════════════════════════════════════════════════════════════════
    {
        #ifndef DISABLE_GRPC
        // Dev mode: Start gRPC server for fast iteration with rich tooling
        std::cout << "[DaemonApp] Starting gRPC server (dev mode)..." << std::endl;

        // Get dependencies from initialized services
        ChainDB* chain_db = ctx_.chainstate ? ctx_.chainstate->GetChainDB() : nullptr;
        Mempool* mempool_ptr = ctx_.mempool ? &ctx_.mempool->mempool() : nullptr;
        policy::FeeEstimator* fee_estimator = ctx_.mempool ? ctx_.mempool->getFeeEstimator().get() : nullptr;

        // Create and start gRPC server with wallet service
        grpc_server_ = std::make_unique<grpc_server::GrpcServer>(
            chain_db,
            ctx_.block_storage.get(),
            mempool_ptr,
            fee_estimator,
            &ctx_,   // DaemonContext for WalletService
            50052    // Using 50052 (50051 is stuck in LISTEN state)
        );

        if (grpc_server_->Start()) {
            std::cout << "[DaemonApp] ✅ gRPC server listening on " << grpc_server_->GetAddress() << " (dev mode)" << std::endl;
        } else {
            std::cerr << "[DaemonApp] ❌ Failed to start gRPC server" << std::endl;
            // Non-fatal - continue without gRPC (Lightning won't work but daemon can run)
        }
        #else
        std::cout << "[DaemonApp] gRPC server disabled (release mode: DINERO_RELEASE=ON)" << std::endl;
        #endif

        // Phase 5: Start socket wallet server (Lightning wire protocol)
        // Skip on iOS — TCP bind to localhost fails in app sandbox
#if defined(__APPLE__) && TARGET_OS_IPHONE
        std::cout << "[DaemonApp] Socket wallet server skipped (iOS)" << std::endl;
#else
        std::cout << "[DaemonApp] Starting socket wallet server..." << std::endl;

        // Construct socket address from configured port
        std::string socket_addr = "127.0.0.1:" + std::to_string(ctx_.wallet_socket_port);
        try {
            socket_wallet_server_ = std::make_unique<grpc_server::SocketWalletServer>(
                &ctx_,              // DaemonContext for WalletService
                socket_addr         // Socket address (configurable via CLI flag or env var)
            );

            if (socket_wallet_server_->Start()) {
                std::cout << "[DaemonApp] ✅ Socket wallet server listening on " << socket_wallet_server_->GetAddress()
                          << " (Bitcoin-style wire protocol)" << std::endl;
            } else {
                std::string startup_error = socket_wallet_server_->GetLastError();
                if (!startup_error.empty()) {
                    std::cerr << "[DaemonApp] ❌ Failed to start socket wallet server: "
                              << startup_error << std::endl;
                } else {
                    std::cerr << "[DaemonApp] ❌ Failed to start socket wallet server" << std::endl;
                }
                // Non-fatal - continue without socket server (Lightning won't work but daemon can run)
                socket_wallet_server_.reset();
            }
        } catch (const std::exception& e) {
            std::cerr << "[DaemonApp] ❌ Socket wallet server error: " << e.what() << " (non-fatal)" << std::endl;
            socket_wallet_server_.reset();
        }
#endif
    }

    std::cout << "[DaemonApp] All services started successfully" << std::endl;

    return true;
}

void DaemonApp::Stop() {
    if (!started_) {
        return;
    }

    const auto shutdown_start = ShutdownClock::now();
    LogShutdownPhase("interrupting", shutdown_start, "DaemonApp::Stop entered");

    // Stop wallet notifications first so no additional wallet jobs are queued
    // while services are being torn down.
    try {
        WalletNotify::Shutdown();
    } catch (const std::exception& e) {
        std::cerr << "[DaemonApp] ⚠️  WalletNotify::Shutdown error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[DaemonApp] ⚠️  WalletNotify::Shutdown unknown error" << std::endl;
    }
    LogShutdownPhase("callbacks_flushed", shutdown_start, "wallet notifications shut down");

    std::cerr << "[DaemonApp] Stopping services..." << std::endl;

    // Shutdown per-service JSON loggers
    if (wallet_logger_) {
        wallet_logger_->shutdown();
        std::cout << "[DaemonApp] ✅ Wallet logger shutdown" << std::endl;
    }
    if (p2p_logger_) {
        p2p_logger_->shutdown();
        std::cout << "[DaemonApp] ✅ P2P logger shutdown" << std::endl;
    }
    if (mining_logger_) {
        mining_logger_->shutdown();
        std::cout << "[DaemonApp] ✅ Mining logger shutdown" << std::endl;
    }
    if (mempool_logger_) {
        mempool_logger_->shutdown();
        std::cout << "[DaemonApp] ✅ Mempool logger shutdown" << std::endl;
    }

    // Stop LoggerRouter (after loggers are shut down, before services)
    if (logger_router_) {
        logger_router_->stop();
        std::cout << "[DaemonApp] ✅ Unified log aggregator stopped" << std::endl;
    }

    // NOTE: Stratum server removed - use separate dinero-stratum binary

    // Phase 5: Stop socket wallet server (before stopping services it depends on)
    if (socket_wallet_server_) {
        std::cerr << "[DaemonApp] Stopping socket wallet server..." << std::endl;
        socket_wallet_server_->Stop();
        socket_wallet_server_.reset();
        std::cerr << "[DaemonApp] Socket wallet server stopped" << std::endl;
    }

    // Phase 3: Stop gRPC server (dev mode only, before stopping services it depends on)
    #ifndef DISABLE_GRPC
    if (grpc_server_) {
        std::cout << "[DaemonApp] Stopping gRPC server..." << std::endl;
        grpc_server_->Stop();
        grpc_server_.reset();
        std::cout << "[DaemonApp] ✅ gRPC server stopped" << std::endl;
    }
    #endif
    LogShutdownPhase("external_surfaces_stopped", shutdown_start, "direct external servers stopped");

    // v0.13.0.2 Step B: Save mempool to disk before shutdown
    // CRITICAL: Must happen BEFORE services stop (mempool needs to be operational)
    if (ctx_.mempool) {
        auto mempool_service = std::dynamic_pointer_cast<MempoolService>(ctx_.mempool);
        if (mempool_service) {
            std::cout << "[DaemonApp] Saving mempool to disk..." << std::endl;

            Mempool& mempool = mempool_service->mempool();
            std::string mempool_path = mempool.getDefaultMempoolPath();

            // Get data directory for correct path
            if (ctx_.config) {
                auto config = std::dynamic_pointer_cast<ConfigService>(ctx_.config);
                if (config) {
                    mempool_path = config->DataDir() + "/mempool.dat";
                }
            }

            // Best effort save - never blocks shutdown
            if (mempool.saveToDisk(mempool_path)) {
                std::cout << "[DaemonApp] ✅ Mempool saved to " << mempool_path << std::endl;
            } else {
                std::cout << "[DaemonApp] ⚠️  Mempool save failed (non-fatal)" << std::endl;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.2: Persist mining restart state before shutdown
    // ═══════════════════════════════════════════════════════════════════
    //
    // Contract: E.3 - Restart Semantics
    // - CONFIG persists (mining address)
    // - STATE does not (mining enabled=false after restart)
    // - mining_was_active_before persists (for policy check)
    //
    // CRITICAL: Must happen BEFORE services stop (mining needs to be operational)
    // ═══════════════════════════════════════════════════════════════════

    if (ctx_.mining) {
        auto mining_service = std::dynamic_pointer_cast<MiningService>(ctx_.mining);
        if (mining_service) {
            std::cout << "[DaemonApp] Saving mining restart state..." << std::endl;

            // Capture current mining state (was mining active before shutdown?)
            bool was_mining = mining_service->isMiningEnabled();

            // Get data directory for correct path
            std::string mining_state_path = "mining_state.dat";
            if (ctx_.config) {
                auto config = std::dynamic_pointer_cast<ConfigService>(ctx_.config);
                if (config) {
                    mining_state_path = config->DataDir() + "/mining_state.dat";
                }
            }

            // Best effort save - never blocks shutdown
            try {
                std::ofstream file(mining_state_path, std::ios::binary);
                if (file.is_open()) {
                    // Simple binary format: 1 byte boolean
                    uint8_t state = was_mining ? 1 : 0;
                    file.write(reinterpret_cast<const char*>(&state), sizeof(state));
                    file.close();

                    std::cout << "[DaemonApp] ✅ Mining restart state saved: mining_was_active="
                              << (was_mining ? "true" : "false") << std::endl;
                } else {
                    std::cout << "[DaemonApp] ⚠️  Mining state save failed (non-fatal)" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "[DaemonApp] ⚠️  Mining state save error: " << e.what() << " (non-fatal)" << std::endl;
            }
        }
    }
    LogShutdownPhase("state_flushed", shutdown_start, "mempool and mining restart state persisted");

    if (ctx_.validation_queue) {
        if (ctx_.validation_queue->isRunning()) {
            std::cerr << "[DaemonApp] Stopping ValidationQueue..." << std::endl;
            ctx_.validation_queue->stop();
            std::cerr << "[DaemonApp] ValidationQueue stopped" << std::endl;
        }
        ctx_.validation_queue.reset();
    }

    if (ctx_.parallel_validator) {
        std::cerr << "[DaemonApp] Releasing ParallelBlockValidator..." << std::endl;
        ctx_.parallel_validator.reset();
    }

    // Stop in REVERSE order
    for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
        std::cerr << "[DaemonApp] Stopping " << (*it)->Name() << "..." << std::endl;
        try {
            (*it)->Stop();
            std::cerr << "[DaemonApp] " << (*it)->Name() << " stopped" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[DaemonApp] Exception while stopping " << (*it)->Name()
                      << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[DaemonApp] Unknown exception while stopping " << (*it)->Name() << std::endl;
        }
    }
    LogShutdownPhase("workers_joined", shutdown_start, "all registered services stopped");

    // Clear singleton AFTER all services stopped (not before)
    // so service destructors can still access DaemonContext if needed.
    DaemonContext::setInstance(nullptr);
    std::cout << "[DaemonApp] DaemonContext singleton cleared" << std::endl;
    LogShutdownPhase("objects_reset", shutdown_start, "daemon context singleton cleared");

    started_ = false;
    LogShutdownPhase("shutdown_complete", shutdown_start, "DaemonApp::Stop finished");
    std::cout << "[DaemonApp] All services stopped" << std::endl;
}

} // namespace dinero
