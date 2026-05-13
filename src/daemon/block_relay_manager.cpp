/**
 * Phase G.2: Block Propagation & Sync - Implementation
 * Phase G.6.B: Integrated with BlockDownloadScheduler
 */

#include "daemon/block_relay_manager.h"
#include "daemon/header_serialization.h"  // Phase N.3: canonical 128-byte header wire format
#include "daemon/daemon_context.h"        // For peer height update via P2PService
#include "daemon/services/p2p_service.h"  // For P2PService peer height update
#include "p2p/block_download_scheduler.h"
#include "consensus/header_sync_manager.h"  // Phase G.8
#include "storage/chain_db.h"  // Phase W.1: For sync phase detection
#include "common/ilogger.h"
#include "primitives/block.h"
#include "util/ser.h"  // CompactSize (varint) encoding for P2P messages
#include <cstring>
#include <iostream>  // Debug logging

namespace dinero {

// Message type constants (Bitcoin-compatible)
static constexpr uint32_t MSG_BLOCK = 2;

// ============================================================================
// Constructor
// ============================================================================

BlockRelayManager::BlockRelayManager(ILogger* logger, std::shared_ptr<BlockDownloadScheduler> scheduler)
    : logger_(logger)
    , send_message_callback_(nullptr)
    , validate_block_callback_(nullptr)
    , retrieve_block_callback_(nullptr)
    , has_block_callback_(nullptr)
    , download_scheduler_(scheduler)
    , orphan_pool_(std::make_unique<p2p::OrphanBlockPool>())  // Phase G.7
    , header_sync_manager_(nullptr)  // Phase G.8
    , mempool_(nullptr)  // Phase G.13
    , chain_db_(nullptr)  // Phase W.1
    , current_sync_phase_(SyncPhase::STEADY_STATE) {  // Phase W.1: Default to synced

    if (logger_) {
        if (download_scheduler_) {
            logger_->info("[BlockRelayManager] Initialized (Phase G.2 + G.6.B scheduler + G.7 orphans + G.8 headers-first + G.13 compact blocks)");
        } else {
            logger_->info("[BlockRelayManager] Initialized (Phase G.2 - legacy mode)");
        }
    }

    // If scheduler is provided, set up its callback to send GETDATA
    if (download_scheduler_) {
        download_scheduler_->setMaxInFlight(16);  // Allow 16 concurrent downloads
        download_scheduler_->setTimeout(60);       // 60 second timeout
        download_scheduler_->setMaxRetries(3);     // 3 retries before giving up
    }
}

// ============================================================================
// Block Announcement (Outbound)
// ============================================================================

void BlockRelayManager::AnnounceBlock(const uint256& block_hash) {
    if (!send_message_callback_) {
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot announce block: send callback not set");
        }
        return;
    }

    // Mark as seen (prevent re-announcement)
    MarkBlockAsSeen(block_hash);

    bool sent_compact = false;
    if (retrieve_block_callback_ && mempool_ && GetCurrentSyncPhase() != SyncPhase::IBD) {
        Block block;
        if (retrieve_block_callback_(block_hash, block)) {
            bool has_confidential_transactions = false;
            for (const auto& tx : block.vtx) {
                if (tx.HasConfidentialOutputs()) {
                    has_confidential_transactions = true;
                    break;
                }
            }

            if (has_confidential_transactions) {
                if (logger_) {
                    logger_->info("[BlockRelayManager] Compact relay disabled for CT-bearing block " +
                                  block_hash.GetHex().substr(0, 16) + "..., using inv/getdata fallback");
                }
            } else {
                CompactBlock compact = CompactBlockCodec::CreateCompactBlock(block);
                auto compact_payload = SerializeCompactBlock(compact);
                if (!compact_payload.empty()) {
                    send_message_callback_("", "cmpctblock", compact_payload);
                    sent_compact = true;
                }
            }
        } else if (logger_) {
            logger_->warning("[BlockRelayManager] Compact announce fallback: could not retrieve block " +
                             block_hash.GetHex().substr(0, 16) + "...");
        }
    }

    // Serialize inv message
    auto inv_payload = SerializeInv(block_hash);

    // When compact relay fired, inv is for the remaining peers only.
    // Otherwise broadcast inv to everyone as the primary fallback path.
    send_message_callback_("", sent_compact ? "inv" : "inv_all", inv_payload);

    // Phase G.9: Track blocks relayed
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.blocks_relayed++;
    }

    if (logger_) {
        logger_->info("[BlockRelayManager] Announced block: " + block_hash.GetHex().substr(0, 16) +
                      "... (compact=" + std::string(sent_compact ? "yes" : "no") + ")");
    }
}

void BlockRelayManager::AnnounceTip() {
    // Get current best block hash
    if (!get_best_block_hash_callback_) {
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot announce tip: get_best_block_hash callback not set");
        }
        return;
    }

    if (!send_message_callback_) {
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot announce tip: send callback not set");
        }
        return;
    }

    uint256 tip_hash = get_best_block_hash_callback_();

    if (tip_hash.IsNull()) {
        if (logger_) {
            logger_->debug("[BlockRelayManager] No tip to announce (genesis only)");
        }
        return;
    }

    // Serialize inv message for tip
    auto inv_payload = SerializeInv(tip_hash);

    // Broadcast to all peers
    send_message_callback_("", "inv", inv_payload);

    if (logger_) {
        logger_->info("[BlockRelayManager] Announced current tip: " + tip_hash.GetHex().substr(0, 16) + "...");
    }
}

// ============================================================================
// Block Request Handling (Inbound)
// ============================================================================

void BlockRelayManager::HandleInv(const std::string& peer_address, const uint256& block_hash) {
    // Debug: Log entry into HandleInv
    std::cout << "[BlockRelayManager::HandleInv] ENTRY - peer=" << peer_address
              << " hash=" << block_hash.GetHex().substr(0, 16) << "..." << std::endl;

    // Check if we already have this block
    if (IsBlockSeen(block_hash)) {
        std::cout << "[BlockRelayManager::HandleInv] Block already SEEN, returning" << std::endl;
        if (logger_) {
            logger_->debug("[BlockRelayManager] Ignoring known block inv from " +
                          peer_address + ": " + block_hash.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    std::cout << "[BlockRelayManager::HandleInv] Block NOT seen, will request" << std::endl;
    if (logger_) {
        logger_->info("[BlockRelayManager] Received inv for unknown block from " + peer_address +
                     ": " + block_hash.GetHex().substr(0, 16) + "...");
    }

    // If scheduler is enabled, use it for coordinated downloads
    if (download_scheduler_) {
        // Register the announcing peer as available for downloads
        download_scheduler_->registerPeers({peer_address});

        // Schedule block with height 0 (unknown - would come from headers-first in production)
        download_scheduler_->scheduleBlock(block_hash, 0, peer_address);

        // Process queue immediately to send getdata request
        download_scheduler_->processQueue();

        if (logger_) {
            logger_->info("[BlockRelayManager] Scheduled block download from " + peer_address);
        }
        return;
    }

    // Legacy path: request block immediately
    if (!send_message_callback_) {
        std::cout << "[BlockRelayManager::HandleInv] ERROR: send_message_callback_ not set!" << std::endl;
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot request block: send callback not set");
        }
        return;
    }

    std::cout << "[BlockRelayManager::HandleInv] Sending GETDATA for block " << block_hash.GetHex().substr(0, 16) << "..." << std::endl;
    auto getdata_payload = SerializeGetData(block_hash);
    MarkBlockRequested(block_hash);
    send_message_callback_(peer_address, "getdata", getdata_payload);

    std::cout << "[BlockRelayManager::HandleInv] GETDATA sent to " << peer_address << std::endl;
    if (logger_) {
        logger_->info("[BlockRelayManager] Requested block from " + peer_address);
    }
}

void BlockRelayManager::HandleGetData(const std::string& peer_address, const uint256& block_hash) {
    std::cout << "[BlockRelayManager::HandleGetData] ENTRY - peer=" << peer_address
              << " hash=" << block_hash.GetHex().substr(0, 16) << "..." << std::endl;

    if (logger_) {
        logger_->info("[BlockRelayManager] Received getdata request from " + peer_address +
                     " for block: " + block_hash.GetHex().substr(0, 16) + "...");
    }

    // Check if retrieve callback is set
    if (!retrieve_block_callback_) {
        std::cout << "[BlockRelayManager::HandleGetData] ERROR: retrieve_block_callback_ not set!" << std::endl;
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot retrieve block: retrieve callback not set");
        }
        return;
    }

    // Check if send callback is set
    if (!send_message_callback_) {
        std::cout << "[BlockRelayManager::HandleGetData] ERROR: send_message_callback_ not set!" << std::endl;
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot send block: send callback not set");
        }
        return;
    }

    // Retrieve block from ChainDB
    std::cout << "[BlockRelayManager::HandleGetData] Retrieving block from ChainDB..." << std::endl;
    Block block;
    if (!retrieve_block_callback_(block_hash, block)) {
        // Phase P.2: Block not found - determine why using status callback
        std::cout << "[BlockRelayManager::HandleGetData] Block NOT FOUND - checking status..." << std::endl;

        BlockDataStatus status = BlockDataStatus::Unknown;
        if (get_block_status_callback_) {
            status = get_block_status_callback_(block_hash);
        }

        // Log based on status - this is the key distinction
        switch (status) {
            case BlockDataStatus::Pruned:
                // Expected case: block was intentionally pruned
                if (logger_) {
                    logger_->info("[BlockRelayManager] Block PRUNED: " +
                                 block_hash.GetHex().substr(0, 16) + "... - sending NOTFOUND");
                }
                break;

            case BlockDataStatus::Unknown:
                // Block hash not in our index - never seen it
                if (logger_) {
                    logger_->warning("[BlockRelayManager] Block UNKNOWN: " +
                                    block_hash.GetHex().substr(0, 16) + "... - sending NOTFOUND");
                }
                break;

            case BlockDataStatus::Corrupted:
                // Error case: block should exist but data is bad
                if (logger_) {
                    logger_->error("[BlockRelayManager] Block CORRUPTED: " +
                                  block_hash.GetHex().substr(0, 16) + "... - data unreadable!");
                }
                // TODO: Could trigger re-download or alert operator
                break;

            case BlockDataStatus::Available:
                // Should not happen - retrieval should have succeeded
                if (logger_) {
                    logger_->error("[BlockRelayManager] Block status AVAILABLE but retrieval failed: " +
                                  block_hash.GetHex().substr(0, 16) + "...");
                }
                break;
        }

        // Send NOTFOUND message to peer (same for all unavailable cases)
        auto notfound_payload = SerializeNotFound(block_hash);
        send_message_callback_(peer_address, "notfound", notfound_payload);

        if (logger_) {
            logger_->debug("[BlockRelayManager] Sent NOTFOUND to " + peer_address +
                          " for block: " + block_hash.GetHex().substr(0, 16) + "...");
        }
        return;
    }
    std::cout << "[BlockRelayManager::HandleGetData] Block retrieved successfully" << std::endl;

    // Serialize and send block to requesting peer
    std::cout << "[BlockRelayManager::HandleGetData] Serializing block..." << std::endl;
    auto block_payload = SerializeBlock(block);
    if (block_payload.empty()) {
        std::cout << "[BlockRelayManager::HandleGetData] ERROR: Serialization failed!" << std::endl;
        if (logger_) {
            logger_->error("[BlockRelayManager] Failed to serialize block: " +
                          block_hash.GetHex().substr(0, 16) + "...");
        }
        return;
    }
    std::cout << "[BlockRelayManager::HandleGetData] Serialized " << block_payload.size() << " bytes" << std::endl;

    std::cout << "[BlockRelayManager::HandleGetData] Sending block to " << peer_address << "..." << std::endl;
    send_message_callback_(peer_address, "block", block_payload);

    std::cout << "[BlockRelayManager::HandleGetData] Block SENT to " << peer_address << std::endl;
    if (logger_) {
        logger_->info("[BlockRelayManager] Sent block to " + peer_address +
                     ": " + block_hash.GetHex().substr(0, 16) + "...");
    }

    // Peer requested this block and we sent it — update their tracked height.
    if (chain_db_) {
        auto h = chain_db_->getBlockHeight(block_hash);
        if (h.status() == Status::Ok) {
            uint32_t bh = static_cast<uint32_t>(h.value());
            if (auto* ctx = DaemonContext::instance()) {
                if (ctx->p2p) {
                    auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx->p2p);
                    if (p2p) {
                        p2p->get().update_peer_synced_blocks(peer_address, bh);
                    }
                }
            }
        }
    }
}

void BlockRelayManager::HandleBlock(const std::string& peer_address, const Block& block) {
    uint256 block_hash = block.GetHash();

    // This block payload arrived; clear any pending explicit-request marker.
    ConsumeBlockRequest(block_hash);

    if (logger_) {
        logger_->info("[BlockRelayManager] Received block from " + peer_address +
                     ": " + block_hash.GetHex().substr(0, 16) + "...");
    }

    // Phase G.9: Track blocks seen
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.blocks_seen++;
    }

    // Check if we already processed this block
    if (IsBlockSeen(block_hash)) {
        if (logger_) {
            logger_->debug("[BlockRelayManager] Ignoring duplicate block: " +
                          block_hash.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    // Phase G.7: Check if parent exists (orphan detection)
    // Phase 3: prev_block_hash is uint256, convert to hex
    std::string parent_hex = block.header.prev_block_hash.GetHex();

    // Genesis block check (parent = all zeros)
    bool is_genesis = (parent_hex == std::string(64, '0'));

    // Only check for orphans if not genesis and callback exists
    bool is_orphan = false;
    if (!is_genesis && has_block_callback_) {
        // Convert hex string to uint256
        uint256 parent_hash = uint256::FromHexUnsafe(parent_hex);

        bool parent_exists = has_block_callback_(parent_hash);
        is_orphan = !parent_exists;
    }

    if (is_orphan) {
        // Parent unknown → this is an orphan block
        uint256 parent_hash = uint256::FromHexUnsafe(parent_hex);

        if (logger_) {
            logger_->warning("[BlockRelayManager] Block " + block_hash.GetHex().substr(0, 16) +
                           "... is orphan (parent " + parent_hash.GetHex().substr(0, 16) + "... unknown)");
        }

        // Add to orphan pool
        if (orphan_pool_) {
            bool added = orphan_pool_->addOrphan(block, block_hash.GetHex(), parent_hash.GetHex(), peer_address);
            if (added) {
                if (logger_) {
                    logger_->info("[BlockRelayManager] Added orphan to pool (waiting for parent)");
                }
                // Schedule download of missing parent
                ScheduleParentDownload(parent_hash, peer_address);
            } else {
                if (logger_) {
                    logger_->warning("[BlockRelayManager] Orphan pool rejected block (pool full or DoS limit)");
                }
            }
        }
        return;
    }

    // Route to consensus validation
    if (!validate_block_callback_) {
        if (logger_) {
            logger_->error("[BlockRelayManager] Cannot validate block: callback not set");
        }
        return;
    }

    bool accepted = validate_block_callback_(block, peer_address);

    if (accepted) {
        // Phase G.9: Track validated blocks
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.blocks_validated++;
        }

        // Phase G.10: Record successful delivery from peer
        RecordBlockDelivery(peer_address);

        // Mark as seen (prevent reprocessing)
        MarkBlockAsSeen(block_hash);

        // Notify scheduler that block was received successfully
        if (download_scheduler_) {
            download_scheduler_->notifyBlockReceived(block_hash);
        }

        if (logger_) {
            logger_->info("[BlockRelayManager] Block accepted: " +
                         block_hash.GetHex().substr(0, 16) + "...");
        }

        // Phase G.7: Process orphans that were waiting for this block
        ProcessOrphans(block_hash);
    } else {
        // Phase G.9: Track rejected blocks
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.blocks_rejected++;
        }

        // Phase G.10: Record block failure (invalid block from peer)
        RecordBlockFailure(peer_address);

        if (logger_) {
            logger_->warning("[BlockRelayManager] Block rejected: " +
                            block_hash.GetHex().substr(0, 16) + "...");
        }
    }
}

// ============================================================================
// State Queries
// ============================================================================

bool BlockRelayManager::IsBlockSeen(const uint256& block_hash) const {
    std::lock_guard<std::mutex> lock(seen_blocks_mutex_);
    return seen_blocks_.count(block_hash) > 0;
}

bool BlockRelayManager::IsBlockDownloadInFlight(const uint256& block_hash) const {
    if (!download_scheduler_) {
        return IsLegacyBlockRequested(block_hash);
    }
    return download_scheduler_->isInFlight(block_hash) || IsLegacyBlockRequested(block_hash);
}

size_t BlockRelayManager::GetSeenBlockCount() const {
    std::lock_guard<std::mutex> lock(seen_blocks_mutex_);
    return seen_blocks_.size();
}

void BlockRelayManager::ProcessDownloadQueue() {
    if (!download_scheduler_) {
        return;  // Scheduler not enabled
    }

    // Process the download queue
    // The scheduler was already configured with a callback in the constructor
    download_scheduler_->processQueue();
}

// ============================================================================
// Private Helpers
// ============================================================================

void BlockRelayManager::MarkBlockAsSeen(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(seen_blocks_mutex_);
    seen_blocks_.insert(block_hash);
}

void BlockRelayManager::MarkBlockRequested(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(requested_blocks_mutex_);
    requested_blocks_[block_hash] = std::chrono::steady_clock::now();
}

void BlockRelayManager::ConsumeBlockRequest(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(requested_blocks_mutex_);
    requested_blocks_.erase(block_hash);
}

bool BlockRelayManager::IsLegacyBlockRequested(const uint256& block_hash) const {
    std::lock_guard<std::mutex> lock(requested_blocks_mutex_);
    const auto now = std::chrono::steady_clock::now();

    // Opportunistic pruning to keep bounded memory under adversarial INV spam.
    if (requested_blocks_.size() > 8192) {
        for (auto it = requested_blocks_.begin(); it != requested_blocks_.end();) {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (age > REQUEST_TRACK_TIMEOUT_SECONDS) {
                it = requested_blocks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto it = requested_blocks_.find(block_hash);
    if (it == requested_blocks_.end()) {
        return false;
    }

    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
    if (age > REQUEST_TRACK_TIMEOUT_SECONDS) {
        requested_blocks_.erase(it);
        return false;
    }

    return true;
}

std::vector<uint8_t> BlockRelayManager::SerializeInv(const uint256& block_hash) const {
    std::vector<uint8_t> payload;

    // Count (1 inventory item)
    payload.push_back(1);

    // Type (MSG_BLOCK = 2)
    for (int i = 0; i < 4; i++) {
        payload.push_back((MSG_BLOCK >> (i * 8)) & 0xFF);
    }

    // Block hash (32 bytes)
    payload.insert(payload.end(), block_hash.begin(), block_hash.end());

    return payload;
}

std::vector<uint8_t> BlockRelayManager::SerializeGetData(const uint256& block_hash) const {
    // getdata has same format as inv
    return SerializeInv(block_hash);
}

std::vector<uint8_t> BlockRelayManager::SerializeBlock(const Block& block) const {
    // Serialize full block using Block::Serialize()
    // Format: 128-byte header (BlockHeader v1) + varint tx_count + transactions (without witness)
    try {
        std::string serialized = block.Serialize();

        // Convert string to vector<uint8_t>
        std::vector<uint8_t> payload(serialized.begin(), serialized.end());

        if (logger_) {
            logger_->debug("[BlockRelayManager] Serialized block: " +
                          std::to_string(payload.size()) + " bytes");
        }

        return payload;
    } catch (const std::exception& e) {
        if (logger_) {
            logger_->error("[BlockRelayManager] Block serialization failed: " +
                          std::string(e.what()));
        }
        return std::vector<uint8_t>();  // Return empty on error
    }
}

// Phase P.2: Serialize notfound message for unavailable blocks (e.g., pruned)
std::vector<uint8_t> BlockRelayManager::SerializeNotFound(const uint256& block_hash) const {
    // notfound message format (Bitcoin-compatible):
    // - count: varint (number of inventory entries)
    // - inventory: array of (type:uint32, hash:32bytes)
    //
    // For simplicity, we use a single-entry notfound.

    std::vector<uint8_t> payload;

    // Count = 1 (single item)
    payload.push_back(0x01);

    // Type = MSG_BLOCK (2) as uint32_t little-endian
    payload.push_back(MSG_BLOCK & 0xFF);
    payload.push_back((MSG_BLOCK >> 8) & 0xFF);
    payload.push_back((MSG_BLOCK >> 16) & 0xFF);
    payload.push_back((MSG_BLOCK >> 24) & 0xFF);

    // Hash (32 bytes, internal byte order - reversed from display hex)
    payload.insert(payload.end(), block_hash.begin(), block_hash.begin() + 32);

    if (logger_) {
        logger_->debug("[BlockRelayManager] Serialized notfound for block: " +
                      block_hash.GetHex().substr(0, 16) + "...");
    }

    return payload;
}

// ============================================================================
// Phase G.7: Orphan Handling
// ============================================================================

void BlockRelayManager::ProcessOrphans(const uint256& parent_hash) {
    if (!orphan_pool_) {
        return;  // Orphan pool not initialized
    }

    // Get all orphans waiting for this parent
    auto orphans = orphan_pool_->getOrphansForParent(parent_hash.GetHex());

    if (orphans.empty()) {
        return;  // No orphans waiting
    }

    if (logger_) {
        logger_->info("[BlockRelayManager] Processing " + std::to_string(orphans.size()) +
                     " orphan(s) waiting for parent " + parent_hash.GetHex().substr(0, 16) + "...");
    }

    // Process each orphan recursively
    for (const auto& orphan : orphans) {
        if (logger_) {
            logger_->info("[BlockRelayManager] Processing orphan " + orphan->block_hash.substr(0, 16) + "...");
        }

        // Remove from orphan pool before processing
        orphan_pool_->removeOrphan(orphan->block_hash);

        // Validate the orphan block
        // This will recursively call ProcessOrphans if this orphan has children
        HandleBlock(orphan->peer_id, orphan->block);
    }
}

void BlockRelayManager::ScheduleParentDownload(const uint256& parent_hash, const std::string& peer_address) {
    if (!download_scheduler_) {
        // No scheduler → request immediately via GETDATA
        if (logger_) {
            logger_->info("[BlockRelayManager] Requesting parent " + parent_hash.GetHex().substr(0, 16) +
                         "... from " + peer_address);
        }

        if (send_message_callback_) {
            auto getdata_payload = SerializeGetData(parent_hash);
            MarkBlockRequested(parent_hash);
            send_message_callback_(peer_address, "getdata", getdata_payload);
        }
        return;
    }

    // Use scheduler for coordinated download
    if (logger_) {
        logger_->info("[BlockRelayManager] Scheduling parent " + parent_hash.GetHex().substr(0, 16) +
                     "... download from " + peer_address);
    }

    // Schedule the parent block (height unknown, use 0)
    download_scheduler_->scheduleBlock(parent_hash, 0, peer_address);

    // Process queue to start download immediately
    download_scheduler_->processQueue();
}

// ============================================================================
// Phase G.8: Headers-First Sync
// ============================================================================

void BlockRelayManager::HandleGetHeaders(const std::string& peer_address,
                                        const uint256& start_hash,
                                        const uint256& stop_hash) {
    if (logger_) {
        logger_->info("[BlockRelayManager] Received getheaders from " + peer_address +
                     " (start: " + start_hash.GetHex().substr(0, 16) + "...)");
    }

    // This would typically retrieve headers from ChainDB/HeaderSyncManager
    // For now, we'll implement a minimal response
    // In production, this should:
    // 1. Look up start_hash in chain
    // 2. Collect up to 2000 headers following it
    // 3. Stop at stop_hash if provided
    // 4. Send headers message back to peer

    if (!send_message_callback_) {
        if (logger_) {
            logger_->error("[BlockRelayManager] Cannot send headers: callback not set");
        }
        return;
    }

    // TODO: Retrieve headers from storage
    std::vector<BlockHeader> headers;

    // Send headers response
    auto headers_payload = SerializeHeaders(headers);
    send_message_callback_(peer_address, "headers", headers_payload);

    if (logger_) {
        logger_->debug("[BlockRelayManager] Sent " + std::to_string(headers.size()) +
                      " headers to " + peer_address);
    }
}

void BlockRelayManager::HandleHeaders(const std::string& peer_address,
                                      const std::vector<BlockHeader>& headers) {
    if (logger_) {
        logger_->info("[BlockRelayManager] Received " + std::to_string(headers.size()) +
                     " headers from " + peer_address);
    }

    // Phase G.9: Track headers received
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.headers_received++;
    }

    if (headers.empty()) {
        if (logger_) {
            logger_->debug("[BlockRelayManager] Empty headers message, sync may be complete");
        }
        return;
    }

    // Route to HeaderSyncManager for validation and chain selection
    if (header_sync_manager_) {
        bool accepted = header_sync_manager_->ProcessHeaders(peer_address, headers);

        if (accepted) {
            // Phase G.9: Track headers accepted
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.headers_accepted++;
            }

            // Phase G.10: Record successful headers delivery
            RecordHeadersDelivery(peer_address);

            if (logger_) {
                logger_->info("[BlockRelayManager] Headers accepted by HeaderSyncManager");
            }

            // Schedule block downloads for validated headers
            ScheduleBlocksFromHeaders();
        } else {
            // Phase G.9: Track headers rejected
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.headers_rejected++;
            }

            // Phase G.10: Record headers failure
            RecordHeadersFailure(peer_address);

            if (logger_) {
                logger_->warning("[BlockRelayManager] Headers rejected by HeaderSyncManager");
            }
        }
    } else {
        if (logger_) {
            logger_->warning("[BlockRelayManager] HeaderSyncManager not configured, headers ignored");
        }
    }
}

void BlockRelayManager::RequestHeaders(const std::string& peer_address,
                                       const uint256& start_hash) {
    if (logger_) {
        logger_->info("[BlockRelayManager] Requesting headers from " + peer_address +
                     " (start: " + start_hash.GetHex().substr(0, 16) + "...)");
    }

    if (!send_message_callback_) {
        if (logger_) {
            logger_->error("[BlockRelayManager] Cannot request headers: callback not set");
        }
        return;
    }

    // Send getheaders message
    uint256 stop_hash;  // Empty = request to tip
    auto getheaders_payload = SerializeGetHeaders(start_hash, stop_hash);
    send_message_callback_(peer_address, "getheaders", getheaders_payload);

    // Phase G.9: Track headers requested
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.headers_requested++;
    }

    if (logger_) {
        logger_->debug("[BlockRelayManager] Sent getheaders to " + peer_address);
    }
}

void BlockRelayManager::ScheduleBlocksFromHeaders() {
    if (!header_sync_manager_ || !download_scheduler_) {
        return;  // Not configured for headers-first sync
    }

    // Get blocks to download from HeaderSyncManager
    auto blocks_to_download = header_sync_manager_->GetBlocksToDownload(16);

    if (blocks_to_download.empty()) {
        return;  // No blocks needed
    }

    if (logger_) {
        logger_->info("[BlockRelayManager] Scheduling " +
                     std::to_string(blocks_to_download.size()) +
                     " blocks from validated headers");
    }

    // Schedule each block for download
    for (const auto& block_hash_hex : blocks_to_download) {
        uint256 block_hash = uint256::FromHexUnsafe(block_hash_hex);

        // Schedule block (height unknown, use 0, no preferred peer)
        download_scheduler_->scheduleBlock(block_hash, 0, "");
    }

    // Process queue to start downloads
    download_scheduler_->processQueue();
}

// ============================================================================
// Phase G.8: Serialization Helpers
// ============================================================================

std::vector<uint8_t> BlockRelayManager::SerializeGetHeaders(const uint256& start_hash,
                                                             const uint256& stop_hash) const {
    std::vector<uint8_t> payload;

    // Version (4 bytes)
    uint32_t version = 1;
    for (int i = 0; i < 4; i++) {
        payload.push_back((version >> (i * 8)) & 0xFF);
    }

    // Locator count (1 byte) - simplified: just one hash
    payload.push_back(1);

    // Start hash (32 bytes)
    payload.insert(payload.end(), start_hash.begin(), start_hash.end());

    // Stop hash (32 bytes)
    payload.insert(payload.end(), stop_hash.begin(), stop_hash.end());

    return payload;
}

std::vector<uint8_t> BlockRelayManager::SerializeHeaders(const std::vector<BlockHeader>& headers) const {
    std::vector<uint8_t> payload;

    // Count (CompactSize / Bitcoin-style varint)
    ser::writeCompactSize(headers.size(), payload);

    // Serialize each header as Dinero BlockHeader v1 wire bytes (128 bytes),
    // followed by tx count (CompactSize=0), matching HeadersMessage::deserialize().
    for (const auto& header : headers) {
        const auto header_bytes = serializeHeaderForWire(header);  // 128 bytes
        payload.insert(payload.end(), header_bytes.begin(), header_bytes.end());
        payload.push_back(0);  // tx count = 0
    }

    return payload;
}

// ============================================================================
// Phase G.9: Telemetry & Debug Visibility
// ============================================================================

BlockRelayManager::Stats BlockRelayManager::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    // Create a copy of stats to return
    Stats result = stats_;

    // Get scheduler stats if available
    if (download_scheduler_) {
        auto sched_stats = download_scheduler_->getStats();
        result.downloads_queued = sched_stats.queued_blocks;
        result.downloads_in_flight = sched_stats.in_flight_blocks;
        result.downloads_completed = sched_stats.completed_blocks;
        result.downloads_failed = sched_stats.failed_blocks;
    }

    // Get orphan pool stats if available
    if (orphan_pool_) {
        auto orphan_stats = orphan_pool_->getStats();
        result.orphans_current = orphan_stats.total_orphans;
        result.orphans_added = orphan_stats.orphans_added;
        result.orphans_resolved = orphan_stats.orphans_resolved;
        result.orphans_evicted = orphan_stats.orphans_evicted;
        result.orphan_pool_bytes = orphan_stats.total_size_bytes;
    }

    return result;
}

// ============================================================================
// Phase G.10: Peer-Aware Intelligence
// ============================================================================

BlockRelayManager::PeerPerformance BlockRelayManager::GetPeerPerformance(const std::string& peer_address) const {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    auto it = peer_performance_.find(peer_address);
    if (it != peer_performance_.end()) {
        // Return copy with updated metrics
        PeerPerformance perf = it->second;
        perf.UpdateMetrics();
        return perf;
    }

    // Return empty performance if peer not tracked
    PeerPerformance empty;
    empty.peer_address = peer_address;
    return empty;
}

std::map<std::string, BlockRelayManager::PeerPerformance> BlockRelayManager::GetAllPeerPerformance() const {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    std::map<std::string, PeerPerformance> result;
    for (const auto& pair : peer_performance_) {
        PeerPerformance perf = pair.second;
        perf.UpdateMetrics();
        result[pair.first] = perf;
    }

    return result;
}

void BlockRelayManager::RecordBlockDelivery(const std::string& peer_address, uint64_t response_time_ms) {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    auto& perf = peer_performance_[peer_address];
    perf.peer_address = peer_address;
    perf.blocks_delivered++;

    if (response_time_ms > 0) {
        perf.total_response_time_ms += response_time_ms;
    }

    perf.last_success_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (logger_) {
        logger_->debug("[BlockRelayManager] Peer " + peer_address +
                      " delivered block (total: " + std::to_string(perf.blocks_delivered) + ")");
    }
}

void BlockRelayManager::RecordBlockFailure(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    auto& perf = peer_performance_[peer_address];
    perf.peer_address = peer_address;
    perf.blocks_failed++;

    perf.last_failure_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (logger_) {
        logger_->warning("[BlockRelayManager] Peer " + peer_address +
                        " failed to deliver block (total failures: " + std::to_string(perf.blocks_failed) + ")");
    }
}

void BlockRelayManager::RecordHeadersDelivery(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    auto& perf = peer_performance_[peer_address];
    perf.peer_address = peer_address;
    perf.headers_delivered++;

    perf.last_success_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (logger_) {
        logger_->debug("[BlockRelayManager] Peer " + peer_address +
                      " delivered headers (total: " + std::to_string(perf.headers_delivered) + ")");
    }
}

void BlockRelayManager::RecordHeadersFailure(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(peer_perf_mutex_);

    auto& perf = peer_performance_[peer_address];
    perf.peer_address = peer_address;
    perf.headers_failed++;

    perf.last_failure_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (logger_) {
        logger_->warning("[BlockRelayManager] Peer " + peer_address +
                        " failed to deliver headers (total failures: " + std::to_string(perf.headers_failed) + ")");
    }
}

// ============================================================================
// Phase G.13: Compact Block Relay
// ============================================================================

void BlockRelayManager::HandleCompactBlock(const std::string& peer_address, const CompactBlock& compact) {
    uint256 block_hash = compact.header.GetHash();

    if (logger_) {
        logger_->debug("[BlockRelayManager] Received compact block " + block_hash.GetHex() +
                      " from " + peer_address +
                      " (short_txids: " + std::to_string(compact.short_txids.size()) +
                      ", prefilled: " + std::to_string(compact.prefilled.size()) + ")");
    }

    // Check if already seen
    if (IsBlockSeen(block_hash)) {
        if (logger_) {
            logger_->debug("[BlockRelayManager] Compact block " + block_hash.GetHex() + " already seen, ignoring");
        }
        return;
    }

    // Attempt reconstruction from mempool
    Block partial_block;
    std::vector<uint32_t> missing_indexes;
    if (!CompactBlockCodec::ReconstructPartialBlock(compact, mempool_, partial_block, missing_indexes)) {
        if (logger_) {
            logger_->warning("[BlockRelayManager] Compact block " + block_hash.GetHex() +
                            " is malformed and could not be partially reconstructed");
        }
        RecordBlockFailure(peer_address);
        return;
    }

    if (missing_indexes.empty()) {
        // Reconstruction succeeded - validate block normally
        if (logger_) {
            logger_->info("[BlockRelayManager] Successfully reconstructed compact block " +
                         block_hash.GetHex() + " from mempool");
        }

        // Phase G.14: Record compact block success (no round trip needed)
        RecordCompactBlockSuccess(peer_address);

        // Update telemetry
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.blocks_seen++;
        }

        // Route to validation
        if (validate_block_callback_) {
            bool accepted = validate_block_callback_(partial_block, peer_address);
            if (accepted) {
                MarkBlockAsSeen(block_hash);
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.blocks_validated++;
                }
                RecordBlockDelivery(peer_address);  // Successful compact block delivery

                if (download_scheduler_) {
                    download_scheduler_->notifyBlockReceived(block_hash);
                }

                ProcessOrphans(block_hash);
            } else {
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.blocks_rejected++;
                }
                RecordBlockFailure(peer_address);
            }
        }
    } else {
        // Missing transactions - request them
        if (logger_) {
            logger_->info("[BlockRelayManager] Compact block " + block_hash.GetHex() +
                         " missing " + std::to_string(missing_indexes.size()) +
                         " transactions, requesting via getblocktxn");
        }

        // Phase G.14: Record compact block failure (needs round trip)
        RecordCompactBlockFailure(peer_address);

        // Phase G.14: Update telemetry for missing transactions
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.compact_txns_requested += missing_indexes.size();
        }

        // Store compact block for later completion
        {
            std::lock_guard<std::mutex> lock(compact_block_mutex_);
            pending_compact_blocks_[block_hash] = PendingCompactBlockState{
                compact,
                partial_block,
                missing_indexes
            };
        }

        // Send getblocktxn request
        BlockTransactionsRequest request(block_hash, missing_indexes);
        if (send_message_callback_) {
            std::vector<uint8_t> payload = SerializeGetBlockTxn(request);
            send_message_callback_(peer_address, "getblocktxn", payload);
        }
    }
}

void BlockRelayManager::HandleGetBlockTxn(const std::string& peer_address, const BlockTransactionsRequest& request) {
    if (logger_) {
        logger_->debug("[BlockRelayManager] Peer " + peer_address +
                      " requests " + std::to_string(request.indexes.size()) +
                      " transactions for block " + request.block_hash.GetHex());
    }

    // Retrieve full block
    Block block;
    if (!retrieve_block_callback_ || !retrieve_block_callback_(request.block_hash, block)) {
        if (logger_) {
            logger_->warning("[BlockRelayManager] Cannot retrieve block " +
                            request.block_hash.GetHex() + " for getblocktxn request");
        }
        return;
    }

    // Extract requested transactions
    std::vector<Transaction> missing_txs;
    for (uint32_t index : request.indexes) {
        if (index >= block.vtx.size()) {
            if (logger_) {
                logger_->warning("[BlockRelayManager] Invalid transaction index " +
                                std::to_string(index) + " in getblocktxn (block has " +
                                std::to_string(block.vtx.size()) + " txs)");
            }
            return;
        }
        missing_txs.push_back(block.vtx[index]);
    }

    // Send blocktxn response
    BlockTransactions response(request.block_hash, missing_txs);
    if (send_message_callback_) {
        std::vector<uint8_t> payload = SerializeBlockTxn(response);
        send_message_callback_(peer_address, "blocktxn", payload);

        if (logger_) {
            logger_->debug("[BlockRelayManager] Sent " + std::to_string(missing_txs.size()) +
                          " missing transactions to " + peer_address);
        }
    }
}

void BlockRelayManager::HandleBlockTxn(const std::string& peer_address, const BlockTransactions& response) {
    if (logger_) {
        logger_->debug("[BlockRelayManager] Received blocktxn from " + peer_address +
                      " with " + std::to_string(response.transactions.size()) +
                      " transactions for block " + response.block_hash.GetHex());
    }

    // Phase G.14: Update telemetry for received transactions
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.compact_txns_received += response.transactions.size();
    }

    // Retrieve pending compact block
    PendingCompactBlockState pending;
    {
        std::lock_guard<std::mutex> lock(compact_block_mutex_);
        auto it = pending_compact_blocks_.find(response.block_hash);
        if (it == pending_compact_blocks_.end()) {
            if (logger_) {
                logger_->warning("[BlockRelayManager] Received blocktxn for unknown block " +
                                response.block_hash.GetHex());
            }
            return;
        }
        pending = std::move(it->second);
        pending_compact_blocks_.erase(it);
    }

    if (response.transactions.size() != pending.missing_indexes.size()) {
        if (logger_) {
            logger_->error("[BlockRelayManager] blocktxn size mismatch for " +
                          response.block_hash.GetHex() + ": expected " +
                          std::to_string(pending.missing_indexes.size()) + ", got " +
                          std::to_string(response.transactions.size()));
        }
        RecordBlockFailure(peer_address);
        return;
    }

    // Complete reconstruction with missing transactions
    auto reconstructed = CompactBlockCodec::CompleteReconstruction(
        pending.partial_block,
        response.transactions,
        pending.missing_indexes
    );

    if (!reconstructed.has_value()) {
        if (logger_) {
            logger_->error("[BlockRelayManager] Failed to complete block reconstruction for " +
                          response.block_hash.GetHex());
        }
        RecordBlockFailure(peer_address);
        return;
    }

    if (logger_) {
        logger_->info("[BlockRelayManager] Completed reconstruction of block " +
                     response.block_hash.GetHex() + " after receiving missing transactions");
    }

    // Update telemetry
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.blocks_seen++;
    }

    // Validate completed block
    if (validate_block_callback_) {
        bool accepted = validate_block_callback_(reconstructed.value(), peer_address);
        if (accepted) {
            MarkBlockAsSeen(response.block_hash);
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.blocks_validated++;
            }
            RecordBlockDelivery(peer_address);  // Successful compact block delivery (2-round-trip)

            if (download_scheduler_) {
                download_scheduler_->notifyBlockReceived(response.block_hash);
            }

            ProcessOrphans(response.block_hash);
        } else {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.blocks_rejected++;
            }
            RecordBlockFailure(peer_address);
        }
    }
}

// ============================================================================
// Phase G.13: Serialization Helpers
// ============================================================================

std::vector<uint8_t> BlockRelayManager::SerializeCompactBlock(const CompactBlock& compact) const {
    // TODO: Implement proper serialization
    return compact.Serialize();
}

std::vector<uint8_t> BlockRelayManager::SerializeGetBlockTxn(const BlockTransactionsRequest& request) const {
    // TODO: Implement proper serialization
    return request.Serialize();
}

std::vector<uint8_t> BlockRelayManager::SerializeBlockTxn(const BlockTransactions& response) const {
    // TODO: Implement proper serialization
    return response.Serialize();
}

// ============================================================================
// Phase G.14: Compact Block Telemetry
// ============================================================================

void BlockRelayManager::RecordCompactBlockSuccess(const std::string& peer_address) {
    // Update global stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.compact_blocks_received++;
        stats_.compact_blocks_reconstructed++;
    }

    // Update per-peer performance
    {
        std::lock_guard<std::mutex> lock(peer_perf_mutex_);
        auto& perf = peer_performance_[peer_address];
        perf.peer_address = peer_address;
        perf.compact_blocks_received++;
        perf.compact_blocks_succeeded++;
        perf.UpdateMetrics();  // Recalculate scores

        if (logger_) {
            logger_->debug("[BlockRelayManager] Compact block from " + peer_address +
                          " reconstructed successfully (success_rate: " +
                          std::to_string(static_cast<int>(perf.compact_success_rate * 100)) + "%)");
        }
    }
}

void BlockRelayManager::RecordCompactBlockFailure(const std::string& peer_address) {
    // Update global stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.compact_blocks_received++;
        stats_.compact_blocks_failed++;
    }

    // Update per-peer performance
    {
        std::lock_guard<std::mutex> lock(peer_perf_mutex_);
        auto& perf = peer_performance_[peer_address];
        perf.peer_address = peer_address;
        perf.compact_blocks_received++;
        perf.compact_blocks_failed++;
        perf.UpdateMetrics();  // Recalculate scores (may apply penalty)

        size_t compact_total = perf.compact_blocks_succeeded + perf.compact_blocks_failed;

        if (logger_) {
            logger_->warning("[BlockRelayManager] Compact block from " + peer_address +
                            " failed reconstruction, needed round trip (success_rate: " +
                            std::to_string(static_cast<int>(perf.compact_success_rate * 100)) +
                            "%, total: " + std::to_string(compact_total) + ")");
        }

        // Phase G.14: Auto-demotion check
        // If peer has low compact success rate after enough samples, warn about it
        if (compact_total >= 10 && perf.compact_success_rate < 0.3) {
            if (logger_) {
                logger_->warning("[BlockRelayManager] Peer " + peer_address +
                                " has very low compact block success rate (" +
                                std::to_string(static_cast<int>(perf.compact_success_rate * 100)) +
                                "%), score penalized. Consider avoiding compact blocks for this peer.");
            }
        }
    }
}

// ============================================================================
// Phase W.1: Mining Intelligence Signals
// ============================================================================

SyncPhase BlockRelayManager::GetCurrentSyncPhase() const {
    // Simple heuristic for sync phase detection:
    // - If we have no ChainDB, assume STEADY_STATE (safe default)
    // - Calculate blocks behind tip based on headers vs blocks
    // - IBD:          1000+ blocks behind
    // - CATCHING_UP:  1-1000 blocks behind
    // - STEADY_STATE: 0-1 blocks behind

    if (!chain_db_) {
        // No ChainDB available, assume synced
        return SyncPhase::STEADY_STATE;
    }

    // Get current tip from ChainDB
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != dinero::Status::Ok) {
        // Failed to get tip, assume synced as safe default
        return SyncPhase::STEADY_STATE;
    }

    uint32_t block_height = tip_result.value().height;

    // Get best known header height from HeaderSyncManager
    uint32_t header_height = block_height;  // Default: assume headers == blocks
    if (header_sync_manager_) {
        // TODO: Add GetBestHeaderHeight() to HeaderSyncManager
        // For now, assume headers are same as blocks
        // header_height = header_sync_manager_->GetBestHeaderHeight();
    }

    // Calculate blocks behind
    uint32_t blocks_behind = (header_height > block_height) ?
                             (header_height - block_height) : 0;

    // Determine sync phase based on blocks behind
    if (blocks_behind >= 1000) {
        return SyncPhase::IBD;  // Far behind
    } else if (blocks_behind > 1) {
        return SyncPhase::CATCHING_UP;  // Close but not synced
    } else {
        return SyncPhase::STEADY_STATE;  // Fully synced
    }
}

double BlockRelayManager::GetCompactBlockSuccessRate() const {
    // Calculate overall compact block reconstruction success rate
    // from global statistics

    std::lock_guard<std::mutex> lock(stats_mutex_);

    size_t total_compact = stats_.compact_blocks_reconstructed + stats_.compact_blocks_failed;

    if (total_compact == 0) {
        // No compact blocks yet, return default 0.5 (neutral)
        return 0.5;
    }

    // Success rate = successful reconstructions / total compact blocks
    return static_cast<double>(stats_.compact_blocks_reconstructed) / total_compact;
}

} // namespace dinero
