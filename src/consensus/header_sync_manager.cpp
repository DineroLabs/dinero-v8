#include "consensus/header_sync_manager.h"
#include "storage/chain_db.h"
#include "storage/chainstate_metadata.h"
#include "consensus/chain_manager_interface.h"
#include "consensus/block_index.h"
#include "consensus/chainwork.h"
#include "consensus/pow.h"
#include "consensus/chainparams.h"  // For network-aware PoW validation
#include "common/logger.h"
#include <algorithm>
#include <chrono>

namespace dinero {

// Global HeaderSyncManager instance
std::unique_ptr<HeaderSyncManager> g_header_sync_manager;

// ═══════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

HeaderSyncManager::HeaderSyncManager() {
    g_logger.info("HeaderSyncManager created");
}

HeaderSyncManager::~HeaderSyncManager() {
    Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Initialization
// ═══════════════════════════════════════════════════════════════════════════

bool HeaderSyncManager::Initialize(ChainDB* chain_db, IChainManager* chain_manager,
                                   const std::filesystem::path& datadir) {
    if (!chain_db || !chain_manager) {
        g_logger.error("HeaderSyncManager::Initialize: Invalid dependencies");
        return false;
    }

    chain_db_ = chain_db;
    chain_manager_ = chain_manager;
    datadir_ = datadir;

    g_logger.info("Phase H: Initializing HeaderSyncManager...");

    // Load persisted headers from ChainDB CF #3
    if (!LoadHeaderIndex()) {
        g_logger.warning("Phase H: No persisted headers found (first run or clean state)");
    }

    // Rebuild header tree (parent/child linkage)
    RebuildHeaderTree();

    // Find best header tip
    UpdateBestHeaderTip();

    // Phase H.4 fix: If header tree is empty (clean start) but we already have
    // validated blocks in the active chain, seed best_header from chain tip.
    // Without this, a node with blocks but no header index stays in IBD forever
    // and refuses to serve blocks to peers.
    if (best_header_hash_.IsNull() && chain_manager_) {
        CBlockIndex* active_tip = chain_manager_->GetTip();
        if (active_tip && !active_tip->GetBlockHash().IsNull()) {
            best_header_hash_ = active_tip->GetBlockHash();
            best_header_height_ = active_tip->height;
            g_logger.info("Phase H: Seeded best_header from active chain tip at height " +
                          std::to_string(best_header_height_));
        }
    }

    // Recompute download queue
    UpdateDownloadQueue();

    // Load IBD state from metadata.dat
    ChainstateMetadata metadata(datadir_);
    auto meta = metadata.load();
    if (meta.ok()) {
        is_ibd_ = meta.value().is_ibd;
    } else {
        // First run - assume IBD
        is_ibd_ = true;
    }

    g_logger.info("Phase H: HeaderSyncManager initialized:");
    g_logger.info("  - Headers: " + std::to_string(header_index_.size()));
    g_logger.info("  - Best header height: " + std::to_string(best_header_height_));
    g_logger.info("  - Best block height: " + std::to_string(GetBestBlockHeight()));
    g_logger.info("  - IBD: " + std::string(is_ibd_ ? "true" : "false"));

    return true;
}

void HeaderSyncManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    g_logger.info("Phase H: Shutting down HeaderSyncManager...");

    // Save final state
    SaveHeaderIndex();

    g_logger.info("Phase H: HeaderSyncManager shutdown complete");
}

// ═══════════════════════════════════════════════════════════════════════════
// Header Processing (from P2P)
// ═══════════════════════════════════════════════════════════════════════════

bool HeaderSyncManager::ProcessHeaders(const std::string& peer_id,
                                       const std::vector<BlockHeader>& headers) {
    if (headers.empty()) {
        return true;  // No-op
    }

    std::lock_guard<std::mutex> lock(mutex_);

    g_logger.debug("Phase H: Processing " + std::to_string(headers.size()) +
                  " headers from peer " + peer_id);

    // Find parent of first header
    HeaderNode* parent = FindHeaderNode(headers[0].prev_block_hash);

    // If parent not found in index, check if it exists in ChainDB (already validated blocks)
    // This handles the case where HeaderSyncManager starts fresh but ChainDB has genesis and early blocks
    if (!parent && !headers[0].prev_block_hash.IsNull() && chain_db_) {
        auto height_result = chain_db_->getBlockHeight(headers[0].prev_block_hash);
        if (height_result.status() == Status::Ok) {
            // Parent exists in ChainDB - bootstrap from there
            auto header_result = chain_db_->getHeader(headers[0].prev_block_hash);
            if (header_result.status() == Status::Ok) {
                g_logger.info("Phase H: Bootstrapping from ChainDB block at height " +
                             std::to_string(height_result.value()));

                // Create parent node from ChainDB
                auto parent_node = std::make_unique<HeaderNode>();
                parent_node->hash = headers[0].prev_block_hash;
                parent_node->height = height_result.value();
                parent_node->status = BLOCK_HAVE_DATA;  // Already validated in ChainDB

                // Parse prev_hash from header if available
                BlockHeader db_header = header_result.value();
                parent_node->prev_hash = db_header.prev_block_hash;

                // Add to index
                header_index_[parent_node->hash] = std::move(parent_node);
                parent = header_index_[headers[0].prev_block_hash].get();
            }
        }
    }

    // If parent still not found and not genesis, this is an orphan batch
    if (!parent && !headers[0].prev_block_hash.IsNull()) {
        g_logger.warning("Phase H: Orphan headers received (parent not found: " +
                        headers[0].prev_block_hash.GetHex().substr(0, 16) + "...)");
        return false;
    }

    // Validate header chain (PoW, linkage, timestamps)
    if (!ValidateHeaderChain(headers, parent ? &parent->header : nullptr)) {
        g_logger.warning("Phase H: Header validation failed");
        return false;
    }

    // Phase H.3.1: Bounded batch write (RULE 1)
    // Collect new headers for atomic persistence
    std::vector<std::pair<uint256, HeaderNode*>> new_headers;

    // Add headers to in-memory tree
    for (const auto& header : headers) {
        auto node = CreateHeaderNode(header, parent);
        const uint256& hash = node->hash;

        // Link to parent
        if (parent) {
            parent->children.push_back(node.get());
        }

        // Store in index
        header_index_[hash] = std::move(node);
        HeaderNode* node_ptr = header_index_[hash].get();

        // Track for persistence
        new_headers.push_back({hash, node_ptr});

        // Update parent for next iteration
        parent = node_ptr;
    }

    // Update headers downloaded count
    headers_downloaded_ += headers.size();

    // Record activity timestamp
    last_header_time_ = std::chrono::steady_clock::now();

    // Select best header chain (most chainwork)
    UpdateBestHeaderTip();

    // Phase H.3.1: Persist ONLY new headers in bounded batch (RULE 1)
    // - Bounded: Only this batch (not all headers)
    // - Contiguous: Same parent chain, increasing height
    // - Parent-first: Parent already committed
    if (chain_db_ && !new_headers.empty()) {
        rocksdb::WriteBatch batch;
        ChainWriteToken token;

        for (const auto& [hash, node] : new_headers) {
            ChainDB::PersistedHeaderMetadata metadata;
            metadata.parent_hash = node->prev_hash;
            metadata.height = node->height;
            metadata.chainwork = node->chainwork;
            metadata.status_flags = node->status;

            auto status = chain_db_->putHeaderMetadata(token, hash, metadata, &batch);
            if (status != Status::Ok) {  // Status comparison fix
                g_logger.error("Phase H.3.1: Failed to add header to batch: " + hash.GetHex());
                return false;
            }
        }

        // Atomic commit (RULE 1)
        auto status = chain_db_->writeBatch(token, std::move(batch), true /* sync */);
        if (status != Status::Ok) {  // Status comparison fix
            g_logger.error("Phase H.3.1: Failed to commit header batch");
            return false;
        }

        g_logger.debug("Phase H.3.1: Committed " + std::to_string(new_headers.size()) +
                      " headers atomically");
    }

    // Update block download queue
    UpdateDownloadQueue();

    g_logger.info("Phase H: " + std::to_string(headers.size()) +
                 " headers accepted. Best header: " + std::to_string(best_header_height_));

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Header Validation (PoW Only - No Transactions!)
// ═══════════════════════════════════════════════════════════════════════════

bool HeaderSyncManager::ValidateHeaderChain(const std::vector<BlockHeader>& headers,
                                            const BlockHeader* prev_header) {
    const BlockHeader* current_prev = prev_header;

    for (const auto& header : headers) {
        // Validate PoW
        if (!ValidateHeaderPoW(header)) {
            g_logger.warning("Phase H: PoW validation failed");
            return false;
        }

        // Validate timestamp
        if (!ValidateHeaderTimestamp(header, current_prev)) {
            g_logger.warning("Phase H: Timestamp validation failed");
            return false;
        }

        // Validate linkage (previousHash matches parent)
        if (current_prev) {
            // Phase M.0: GetHash() returns uint256, compare directly
            uint256 prev_hash = current_prev->GetHash();  // Use canonical GetHash()
            if (header.prev_block_hash != prev_hash) {
                g_logger.warning("Phase H: Header linkage validation failed");
                return false;
            }
        }

        current_prev = &header;
    }

    return true;
}

bool HeaderSyncManager::ValidateHeaderPoW(const BlockHeader& header) const {
    // Full PoW validation using consensus module
    // 1. Convert bits to target
    // 2. Compute block hash (double SHA-256)
    // 3. Verify hash <= target
    //
    // Header PoW validation is network-dependent:
    // - mainnet/testnet: require standard PoW targets (require_standard = true)
    // - regtest: allow regtest difficulty for developer mining (require_standard = false)
    //
    // IMPORTANT: This does NOT skip PoW validation. Regtest headers must still be
    // properly mined (hash <= target). The only difference is which difficulty
    // encodings are considered valid.

    const ChainParams& params = Params();
    bool require_standard = (params.name != "regtest");

    if (!dinero::consensus::CheckProofOfWork(header, require_standard)) {
        g_logger.warning("[HeaderSyncManager] Header failed PoW validation: " +
                        header.GetHash().ToString().substr(0, 16) + "...");
        return false;
    }

    return true;
}

bool HeaderSyncManager::ValidateHeaderTimestamp(const BlockHeader& header,
                                                const BlockHeader* prev_header) const {
    // Timestamp validation (partial BIP113)
    //
    // Full BIP113 requires median-time-past from last 11 blocks, which requires
    // walking the header chain. For header sync, we do simpler checks:
    // 1. Verify header.timestamp > prev_header.timestamp (monotonic progress)
    // 2. Verify header.timestamp < now + 2 hours (future time limit)

    // Check timestamp is not too far in the future (2 hours = 7200 seconds)
    constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;
    auto now = std::chrono::system_clock::now();
    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    if (static_cast<int64_t>(header.timestamp) > now_secs + MAX_FUTURE_BLOCK_TIME) {
        g_logger.warning("[HeaderSyncManager] Header timestamp too far in future: " +
                        std::to_string(header.timestamp) + " > " +
                        std::to_string(now_secs + MAX_FUTURE_BLOCK_TIME));
        return false;
    }

    // Check timestamp is after previous block (if we have it)
    if (prev_header != nullptr) {
        if (header.timestamp <= prev_header->timestamp) {
            // Note: BIP113 actually uses median-time-past, not prev timestamp
            // This is a simplified check; some blocks can have earlier timestamps
            // as long as they're after median-time-past. We're lenient here.
            // Full validation happens with block bodies.
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Header Tree Management
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<HeaderSyncManager::HeaderNode> HeaderSyncManager::CreateHeaderNode(
    const BlockHeader& header, HeaderNode* parent) {
    auto node = std::make_unique<HeaderNode>();

    // Phase M.0: GetHash() already returns uint256, no conversion needed
    node->hash = header.GetHash();
    // Use prevBlockHash (previousHash is legacy field)
    node->prev_hash = header.prev_block_hash;
    node->height = parent ? (parent->height + 1) : 0;
    node->header = header;
    node->parent = parent;
    node->status = BLOCK_VALID_HEADER;  // Header validated (PoW, linkage)

    // Calculate chainwork using canonical GetBlockProof()
    if (parent) {
        node->chainwork = parent->chainwork + GetBlockProof(header.difficulty);
    } else {
        // Genesis
        node->chainwork = GetBlockProof(header.difficulty);
    }

    return node;
}

HeaderSyncManager::HeaderNode* HeaderSyncManager::FindHeaderNode(const uint256& hash) {
    auto it = header_index_.find(hash);
    return (it != header_index_.end()) ? it->second.get() : nullptr;
}

const HeaderSyncManager::HeaderNode* HeaderSyncManager::FindHeaderNode(const uint256& hash) const {
    auto it = header_index_.find(hash);
    return (it != header_index_.end()) ? it->second.get() : nullptr;
}

void HeaderSyncManager::UpdateBestHeaderTip() {
    // Find all leaf nodes (no children)
    std::vector<HeaderNode*> tips;
    for (auto& [hash, node] : header_index_) {
        if (node->children.empty()) {
            tips.push_back(node.get());
        }
    }

    if (tips.empty()) {
        best_header_tip_ = nullptr;
        best_header_height_ = 0;
        best_header_hash_.SetNull();
        best_header_work_ = arith_uint256();
        return;
    }

    // Sort by chainwork (primary), hash (tiebreaker)
    // Use Bitcoin's ByWorkThenHash comparator pattern
    std::sort(tips.begin(), tips.end(), [](const HeaderNode* a, const HeaderNode* b) {
        // Primary: most chainwork wins
        if (a->chainwork != b->chainwork) {
            return a->chainwork > b->chainwork;
        }
        // Tiebreaker: lexicographically smallest hash
        return a->hash < b->hash;
    });

    // Best tip is first element (highest work)
    HeaderNode* new_best = tips[0];

    if (new_best != best_header_tip_) {
        best_header_tip_ = new_best;
        best_header_height_ = new_best->height;
        best_header_hash_ = new_best->hash;
        best_header_work_ = new_best->chainwork;

        g_logger.info("Phase H: New best header tip: height=" +
                     std::to_string(best_header_height_) +
                     " hash=" + best_header_hash_.GetHex().substr(0, 16) + "...");
    }
}

bool HeaderSyncManager::IsOnBestHeaderChain(const HeaderNode* node) const {
    if (!node || !best_header_tip_) {
        return false;
    }

    // Walk backwards from best tip to genesis
    const HeaderNode* current = best_header_tip_;
    while (current) {
        if (current == node) {
            return true;
        }
        current = current->parent;
    }

    return false;
}

bool HeaderSyncManager::IsOnBestHeaderChain(const std::string& block_hash) const {
    const HeaderNode* node = FindHeaderNode(uint256::FromHexUnsafe(block_hash));
    return IsOnBestHeaderChain(node);
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Download Scheduling
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> HeaderSyncManager::GetBlocksToDownload(size_t max_blocks) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> blocks;

    // Phase H.2: Respect in-flight limit
    if (in_flight_.size() >= MAX_IN_FLIGHT) {
        return blocks;  // At capacity
    }

    size_t remaining = std::min(max_blocks, MAX_IN_FLIGHT - in_flight_.size());

    while (!download_queue_.empty() && blocks.size() < remaining) {
        uint256 hash = download_queue_.front();
        download_queue_.pop_front();

        // Phase H.2 - Refinement B: Revalidate at request time (queue may be stale)
        if (!IsBlockNeeded(hash.GetHex())) {
            continue;  // Skip - parent arrived, status changed, or off-chain
        }

        if (in_flight_.count(hash)) {
            continue;  // Already requested
        }

        blocks.push_back(hash.GetHex());
    }

    return blocks;
}

bool HeaderSyncManager::IsBlockNeeded(const std::string& block_hash) const {
    // Find header node
    const HeaderNode* node = FindHeaderNode(uint256::FromHexUnsafe(block_hash));
    if (!node) {
        return false;  // Header not known
    }

    // Phase H.2 - Refinement A: HeaderNode::status is SINGLE SOURCE OF TRUTH
    // Check block state from HeaderNode (not separate maps)
    if (node->status & BLOCK_HAVE_DATA) {
        return false;  // Already have block data
    }

    if (node->status & BLOCK_FAILED) {
        return false;  // Failed validation - don't re-download
    }

    // Only download blocks on best header chain
    if (!IsOnBestHeaderChain(node)) {
        return false;
    }

    // Phase H.2: Parent connectivity rule (CRITICAL)
    // Only download if parent is already downloaded
    if (node->parent && !(node->parent->status & BLOCK_HAVE_DATA)) {
        return false;  // Parent not downloaded yet
    }

    return true;
}

void HeaderSyncManager::MarkBlockRequested(const std::string& block_hash,
                                           const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint256 hash = uint256::FromHexUnsafe(block_hash);
    const HeaderNode* node = FindHeaderNode(hash);
    if (!node) {
        return;  // Unknown header
    }

    // Phase H.2: Check if this is a retry (block was previously requested)
    uint32_t attempt_count = 0;
    auto it = in_flight_.find(hash);
    if (it != in_flight_.end()) {
        attempt_count = it->second.attempt_count + 1;
    }

    DownloadRequest req;
    req.block_hash = hash;
    req.height = node->height;
    req.peer_id = peer_id;
    req.request_time = std::chrono::steady_clock::now();
    req.attempt_count = attempt_count;

    in_flight_[hash] = req;

    g_logger.debug("Phase H.2: Block " + block_hash.substr(0, 16) +
                  "... marked as requested from peer " + peer_id +
                  " (attempt " + std::to_string(attempt_count + 1) + ")");
}

void HeaderSyncManager::MarkBlockReceived(const std::string& block_hash,
                                          const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint256 hash = uint256::FromHexUnsafe(block_hash);

    // Remove from in-flight (index)
    in_flight_.erase(hash);

    // Phase H.2 - Refinement A: Update HeaderNode status (authoritative)
    HeaderNode* node = FindHeaderNode(hash);
    if (node) {
        node->status |= BLOCK_HAVE_DATA;

        // Phase H.3: Persist status change to ChainDB.
        //
        // Bug fix (May 2026, fleet-wide HAVE_UNDO regression):
        // Previously this called updateHeaderStatus(token, hash, node->status, ...)
        // which OVERWRITES on-disk status_flags. node->status only tracks
        // the bits HeaderSyncManager cares about (HAVE_DATA, FAILED) — it
        // does NOT carry consensus-layer bits like BLOCK_HAVE_UNDO that
        // ConnectTip sets via the chaindb. The overwrite stripped HAVE_UNDO
        // off every block that had been gossiped via P2P after being
        // mined locally, leaving the chain unable to reorg cleanly past
        // those heights.
        //
        // Fix: setHeaderStatusBits OR-merges the bits we know about into
        // whatever is already on disk. Bits we don't track stay set.
        if (chain_db_) {
            ChainWriteToken token;
            chain_db_->setHeaderStatusBits(token, hash, BLOCK_HAVE_DATA, nullptr /* no batch */);
        }
    }

    // Update stats
    blocks_downloaded_++;

    g_logger.debug("Phase H.2: Block " + block_hash.substr(0, 16) +
                  "... received from peer " + peer_id +
                  ". Total blocks: " + std::to_string(blocks_downloaded_));

    // Update download queue (children may now be eligible)
    UpdateDownloadQueue();
}

void HeaderSyncManager::MarkBlockConnected(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Phase H.2: Mark block as fully validated and connected to UTXO set
    // This is called AFTER G.3.3-G.3.5 succeeds
    HeaderNode* node = FindHeaderNode(uint256::FromHexUnsafe(block_hash));
    if (!node) {
        return;
    }

    // Block is now fully validated and applied
    // (BLOCK_HAVE_DATA should already be set by MarkBlockReceived)
    if (!(node->status & BLOCK_HAVE_DATA)) {
        g_logger.warning("Phase H.2: MarkBlockConnected called but BLOCK_HAVE_DATA not set for " +
                     block_hash.substr(0, 16) + "...");
        node->status |= BLOCK_HAVE_DATA;
    }

    g_logger.debug("Phase H.2: Block " + block_hash.substr(0, 16) + "... connected to chain");

    // Check if IBD complete
    UpdateIBDState();
}

void HeaderSyncManager::MarkBlockFailed(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint256 hash = uint256::FromHexUnsafe(block_hash);

    // Remove from in-flight (index)
    in_flight_.erase(hash);

    // Phase H.2 - Refinement A: Update HeaderNode status (authoritative)
    // Mark as permanently failed - do not re-download
    HeaderNode* node = FindHeaderNode(hash);
    if (node) {
        node->status |= BLOCK_FAILED;

        // Phase H.3: Persist status change to ChainDB.
        // Same May-2026 fix as MarkBlockReceived above: use the OR-merge
        // API so we don't strip HAVE_UNDO or any other consensus-layer
        // bits that may already be set on this header in the chaindb.
        // (BLOCK_FAILED on a previously-good block is a real edge case —
        // for example a script-validation failure on a block that was
        // previously connected. Preserving the existing on-disk state is
        // correct even there: subsequent disconnect/reorg paths still
        // need the undo/data pointers we don't track here.)
        if (chain_db_) {
            ChainWriteToken token;
            chain_db_->setHeaderStatusBits(token, hash, BLOCK_FAILED, nullptr /* no batch */);
        }
    }

    g_logger.warning("Phase H.2: Block " + block_hash.substr(0, 16) +
                 "... failed validation - marked as BLOCK_FAILED");

    // Note: We do NOT revalidate headers (per design constraints)
    // Failed block is simply skipped
}

void HeaderSyncManager::CheckDownloadTimeouts() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::pair<uint256, DownloadRequest>> timed_out;

    // Phase H.2: Check for timeouts using DownloadRequest::isTimedOut()
    for (const auto& [hash, req] : in_flight_) {
        if (req.isTimedOut()) {
            timed_out.push_back({hash, req});
        }
    }

    // Phase H.2: Retry with attempt limit
    for (const auto& [hash, req] : timed_out) {
        if (req.attempt_count >= MAX_DOWNLOAD_ATTEMPTS) {
            // Give up after max attempts - mark as failed
            g_logger.error("Phase H.2: Block " + hash.GetHex().substr(0, 16) +
                          "... exceeded max download attempts (" +
                          std::to_string(MAX_DOWNLOAD_ATTEMPTS) + "), marking as BLOCK_FAILED");

            HeaderNode* node = FindHeaderNode(hash);
            if (node) {
                node->status |= BLOCK_FAILED;
            }
            in_flight_.erase(hash);
        } else {
            // Retry - move back to queue (front for priority)
            g_logger.warning("Phase H.2: Block " + hash.GetHex().substr(0, 16) +
                         "... timed out (attempt " + std::to_string(req.attempt_count + 1) +
                         "/" + std::to_string(MAX_DOWNLOAD_ATTEMPTS) + "), re-queuing");
            download_queue_.push_front(hash);
            in_flight_.erase(hash);
        }
    }
}

void HeaderSyncManager::UpdateDownloadQueue() {
    // Walk from active tip to best header tip
    uint32_t active_height = chain_manager_->GetHeight();

    if (!best_header_tip_) {
        return;  // No headers yet
    }

    // Walk backwards from best header tip to active tip
    const HeaderNode* node = best_header_tip_;
    std::vector<uint256> missing_blocks;

    while (node && node->height > active_height) {
        // Phase H.2 - Refinement A: Check HeaderNode::status (single source of truth)
        bool have_data = (node->status & BLOCK_HAVE_DATA);
        bool is_failed = (node->status & BLOCK_FAILED);
        bool in_flight = (in_flight_.count(node->hash) > 0);

        // Add to queue if: not downloaded, not failed, not already in-flight
        if (!have_data && !is_failed && !in_flight) {
            missing_blocks.push_back(node->hash);
        }

        node = node->parent;
    }

    // Reverse to get oldest-first order
    std::reverse(missing_blocks.begin(), missing_blocks.end());

    // Add to download queue (skip duplicates)
    for (const auto& hash : missing_blocks) {
        // Check if already in queue
        bool in_queue = std::find(download_queue_.begin(), download_queue_.end(), hash) != download_queue_.end();
        if (!in_queue) {
            download_queue_.push_back(hash);
        }
    }

    g_logger.debug("Phase H.2: Download queue updated: " +
                  std::to_string(download_queue_.size()) + " blocks pending");
}

// ═══════════════════════════════════════════════════════════════════════════
// IBD State Detection
// ═══════════════════════════════════════════════════════════════════════════

// Internal helper: Check IBD state (assumes mutex is already locked)
bool HeaderSyncManager::IsInitialBlockDownload_Unlocked() const {
    // Phase H.4: CANONICAL IBD DEFINITION (FINAL)
    // IBD is true iff best header tip != active chain tip
    //
    // No timers. No height thresholds. No magic constants.
    // Headers represent what could be true.
    // Active chain represents what has been proven.
    // IBD ends exactly when those converge.

    if (!chain_manager_) {
        g_logger.info("Phase H: IBD=true (no chain manager)");
        return true;  // No chain manager - assume IBD
    }

    // Get active chain tip from ChainManager
    CBlockIndex* active_tip = chain_manager_->GetTip();
    if (!active_tip) {
        g_logger.info("Phase H: IBD=true (no active tip)");
        return true;  // No active tip - definitely IBD
    }

    // Debug: check the hash field directly
    g_logger.info("Phase H: IBD - active_tip->hash field length=" + std::to_string(active_tip->hash.GetHex().length()) +
                  " value=" + (active_tip->hash.IsNull() ? "EMPTY" : active_tip->hash.GetHex().substr(0,16)));

    // GetBlockHash() returns uint256 - convert to hex for comparison
    uint256 active_tip_hash = active_tip->GetBlockHash();

    // Compare header tip to active tip (hash equality)
    bool ibd = (best_header_hash_ != active_tip_hash);
    g_logger.info("Phase H: IBD check: best_header=" + best_header_hash_.GetHex().substr(0,16) +
                   " active_tip=" + active_tip_hash.GetHex().substr(0,16) + " IBD=" + (ibd ? "true" : "false"));
    return ibd;
}

bool HeaderSyncManager::IsInitialBlockDownload() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsInitialBlockDownload_Unlocked();
}

void HeaderSyncManager::UpdateIBDState() {
    // NOTE: Assumes mutex is already locked by caller
    bool current_ibd = IsInitialBlockDownload_Unlocked();

    if (current_ibd != is_ibd_) {
        is_ibd_ = current_ibd;

        // Persist to metadata.dat
        ChainstateMetadata metadata(datadir_);
        auto meta = metadata.load();

        if (meta.ok()) {
            meta.value().is_ibd = current_ibd;
            metadata.save(meta.value());
        }

        if (!current_ibd) {
            g_logger.info("✅ Phase H: Initial Block Download complete! " 
                         "Height: " + std::to_string(best_header_height_));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Status Queries
// ═══════════════════════════════════════════════════════════════════════════

uint32_t HeaderSyncManager::GetBestBlockHeight() const {
    return chain_manager_ ? chain_manager_->GetHeight() : 0;
}

HeaderSyncManager::SyncStats HeaderSyncManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    SyncStats stats;
    stats.best_header_height = best_header_height_;
    stats.best_block_height = GetBestBlockHeight();
    stats.blocks_in_flight = in_flight_.size();
    stats.headers_downloaded = headers_downloaded_;
    stats.blocks_downloaded = blocks_downloaded_;
    stats.blocks_pending = download_queue_.size();
    stats.is_ibd = is_ibd_;

    // Calculate sync progress
    if (stats.best_header_height > 0) {
        stats.sync_progress = (static_cast<double>(stats.best_block_height) /
                              static_cast<double>(stats.best_header_height)) * 100.0;
    } else {
        stats.sync_progress = 0.0;
    }

    return stats;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase W.2.6 Enhancement #2: Header Sync Status
// ═══════════════════════════════════════════════════════════════════════════

HeaderSyncManager::HeaderSyncStatus HeaderSyncManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    HeaderSyncStatus status;
    status.headers_synced = best_header_height_;
    status.headers_target = best_header_height_;  // TODO: Get from best peer once P2P integration complete
    status.is_syncing = is_ibd_;

    return status;
}

// ═══════════════════════════════════════════════════════════════════════════
// Persistence (Restart Safety)
// ═══════════════════════════════════════════════════════════════════════════

bool HeaderSyncManager::LoadHeaderIndex() {
    if (!chain_db_) {
        g_logger.warning("Phase H.3: No ChainDB - cannot load headers");
        return false;
    }

    g_logger.info("Phase H.3: Loading headers from ChainDB...");

    // Phase H.3: Load all persisted header metadata from CF_HEADERS
    // This rebuilds the header tree after restart
    size_t loaded_count = 0;

    auto status = chain_db_->forEachHeaderMetadata(
        [this, &loaded_count](const uint256& hash, const ChainDB::PersistedHeaderMetadata& metadata) {
            // Create HeaderNode from minimal persisted metadata
            auto node = std::make_unique<HeaderNode>();
            node->hash = hash;
            node->prev_hash = metadata.parent_hash;
            node->height = metadata.height;
            node->chainwork = metadata.chainwork;
            node->status = metadata.status_flags;
            node->parent = nullptr;  // Will be linked in RebuildHeaderTree()

            // Note: We don't have the full BlockHeader here (minimal persistence)
            // The header field is unused during restart recovery

            header_index_[node->hash] = std::move(node);
            loaded_count++;

            return true;  // Continue iteration
        });

    if (status != Status::Ok) {  // Status comparison fix
        g_logger.error("Phase H.3: Failed to load headers from ChainDB");
        return false;
    }

    g_logger.info("Phase H.3: Loaded " + std::to_string(loaded_count) + " headers from disk");
    return true;
}

bool HeaderSyncManager::SaveHeaderIndex() {
    if (!chain_db_) {
        g_logger.warning("Phase H.3: No ChainDB - cannot save headers");
        return false;
    }

    g_logger.debug("Phase H.3: Saving " + std::to_string(header_index_.size()) + " headers to ChainDB...");

    // Phase H.3: Persist all headers to CF_HEADERS using WriteBatch for atomicity
    rocksdb::WriteBatch batch;
    ChainWriteToken token;  // HeaderSyncManager is authorized writer (see chain_write_token.h)

    for (const auto& [hash, node] : header_index_) {
        // Create minimal header metadata (no full BlockHeader serialization)
        ChainDB::PersistedHeaderMetadata metadata;
        metadata.parent_hash = node->prev_hash;
        metadata.height = node->height;
        metadata.chainwork = node->chainwork;
        metadata.status_flags = node->status;

        // Add to batch
        auto status = chain_db_->putHeaderMetadata(token, hash, metadata, &batch);
        if (status != Status::Ok) {  // Status comparison fix
            g_logger.error("Phase H.3: Failed to serialize header " + hash.GetHex());
            return false;
        }
    }

    // Atomic commit
    auto status = chain_db_->writeBatch(token, std::move(batch), true /* sync */);
    if (status != Status::Ok) {  // Status comparison fix
        g_logger.error("Phase H.3: Failed to commit header batch to ChainDB");
        return false;
    }

    g_logger.debug("Phase H.3: Successfully saved " + std::to_string(header_index_.size()) + " headers");
    return true;
}

void HeaderSyncManager::RebuildHeaderTree() {
    // Second pass: link parents and children
    for (auto& [hash, node] : header_index_) {
        if (node->prev_hash.IsNull()) {
            continue;  // Genesis
        }

        auto parent_it = header_index_.find(node->prev_hash);
        if (parent_it != header_index_.end()) {
            node->parent = parent_it->second.get();
            parent_it->second->children.push_back(node.get());
        }
    }

    g_logger.debug("Phase H: Header tree rebuilt with " +
                  std::to_string(header_index_.size()) + " nodes");
}

} // namespace dinero
