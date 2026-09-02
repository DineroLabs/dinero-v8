#include "p2p/headers_first_sync.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace p2p {

// Architecture V3: Global removed - use HeadersSyncService instead

din::Json BlockHeader::toJson() const {
    din::Json json;
    json["version"] = version;
    json["prev_block_hash"] = prev_block_hash;
    json["merkle_root"] = merkle_root;
    json["timestamp"] = timestamp;
    json["bits"] = bits;
    json["nonce"] = nonce;
    json["hash"] = hash;
    json["height"] = height;
    return json;
}

BlockHeader BlockHeader::fromJson(const din::Json& json) {
    BlockHeader header;
    header.version = json["version"].asUInt();
    header.prev_block_hash = json["prev_block_hash"].asString();
    header.merkle_root = json["merkle_root"].asString();
    header.timestamp = json["timestamp"].asUInt();
    header.bits = json["bits"].asUInt();  // p2p::BlockHeader uses 'bits', not 'difficulty'
    header.nonce = json["nonce"].asUInt();
    header.hash = json["hash"].asString();
    header.height = json["height"].asUInt();
    return header;
}

din::Json HeadersRequest::toJson() const {
    din::Json json;
    json["start_hash"] = start_hash;
    json["stop_hash"] = stop_hash;
    json["max_headers"] = max_headers;
    return json;
}

din::Json HeadersResponse::toJson() const {
    din::Json json;
    json["headers"] = Json::Value(Json::arrayValue);
    for (const auto& header : headers) {
        json["headers"].append(header.toJson());
    }
    json["more_available"] = more_available;
    return json;
}

HeadersResponse HeadersResponse::fromJson(const din::Json& json) {
    HeadersResponse response;
    if (json.isMember("headers") && json["headers"].isArray()) {
        for (const auto& header_json : json["headers"]) {
            response.headers.push_back(BlockHeader::fromJson(header_json));
        }
    }
    if (json.isMember("more_available")) {
        response.more_available = json["more_available"].asBool();
    }
    return response;
}

HeadersFirstSync::HeadersFirstSync()
    : state_(SyncState::IDLE)
    , timeout_(std::chrono::seconds(30))
    , best_height_(0)
    , blocks_downloaded_(0)
    , max_headers_per_request_(2000)
    , validation_enabled_(true)
    , total_headers_received_(0)
    , total_blocks_requested_(0)
{
}

HeadersFirstSync::~HeadersFirstSync() {
    stopSync();
}

void HeadersFirstSync::startSync(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != SyncState::IDLE) {
        std::cout << "[P2P] Sync already in progress with peer: " << active_peer_ << std::endl;
        return;
    }
    
    active_peer_ = peer_id;
    state_ = SyncState::REQUESTING_HEADERS;
    last_activity_ = std::chrono::steady_clock::now();
    sync_start_time_ = last_activity_;
    
    std::cout << "[P2P] Starting headers-first sync with peer: " << peer_id << std::endl;
    
    // Request initial headers from genesis or current tip
    requestNextHeaders(peer_id);
}

void HeadersFirstSync::stopSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ == SyncState::IDLE) {
        return;
    }
    
    std::cout << "[P2P] Stopping sync with peer: " << active_peer_ << std::endl;
    
    state_ = SyncState::IDLE;
    active_peer_.clear();
    pending_blocks_.clear();
}

bool HeadersFirstSync::isSyncing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ != SyncState::IDLE && state_ != SyncState::SYNCED && state_ != SyncState::ERROR;
}

SyncState HeadersFirstSync::getCurrentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool HeadersFirstSync::processHeaders(const std::string& peer_id, const HeadersResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (peer_id != active_peer_ || state_ != SyncState::REQUESTING_HEADERS) {
        std::cout << "[P2P] Ignoring headers from inactive peer: " << peer_id << std::endl;
        return false;
    }
    
    last_activity_ = std::chrono::steady_clock::now();
    
    if (response.headers.empty()) {
        std::cout << "[P2P] Received empty headers response, sync complete" << std::endl;
        transitionToState(SyncState::SYNCED);
        return true;
    }
    
    // Validate headers chain
    if (validation_enabled_ && !validateHeadersChain(response.headers)) {
        std::cout << "[P2P] Header validation failed, stopping sync" << std::endl;
        transitionToState(SyncState::ERROR);
        return false;
    }
    
    // Add headers to chain
    for (const auto& header : response.headers) {
        headers_chain_.push_back(header);
        hash_to_index_[header.hash] = headers_chain_.size() - 1;
        total_headers_received_++;
    }
    
    updateBestChain();
    logSyncProgress();
    
    // Request more headers if available
    if (response.more_available) {
        requestNextHeaders(peer_id);
    } else {
        // Headers complete, start requesting blocks
        std::cout << "[P2P] Headers sync complete, requesting blocks" << std::endl;
        transitionToState(SyncState::REQUESTING_BLOCKS);
        
        // Request blocks for all headers
        std::vector<std::string> block_hashes;
        for (const auto& header : headers_chain_) {
            block_hashes.push_back(header.hash);
        }
        requestBlocks(block_hashes);
    }
    
    return true;
}

void HeadersFirstSync::requestNextHeaders(const std::string& peer_id) {
    // This would normally send a network message to the peer
    // For now, we'll log the request
    
    std::string start_hash = "0000000000000000000000000000000000000000000000000000000000000000"; // Genesis
    if (!headers_chain_.empty()) {
        start_hash = headers_chain_.back().hash;
    }
    
    HeadersRequest request;
    request.start_hash = start_hash;
    request.max_headers = max_headers_per_request_;
    
    std::cout << "[P2P] Requesting headers from " << start_hash.substr(0, 8) 
              << "... (max: " << request.max_headers << ")" << std::endl;
    
    // In real implementation, this would send the request over P2P network
    // For testing, we can simulate with stub data
}

void HeadersFirstSync::requestBlocks(const std::vector<std::string>& block_hashes) {
    pending_blocks_ = block_hashes;
    total_blocks_requested_ += block_hashes.size();
    
    std::cout << "[P2P] Requesting " << block_hashes.size() << " blocks" << std::endl;
    
    // In real implementation, this would send block requests over P2P network
}

bool HeadersFirstSync::processBlock(const std::string& block_hash, const std::string& block_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != SyncState::REQUESTING_BLOCKS) {
        return false;
    }
    
    // Mark block as downloaded
    downloaded_blocks_[block_hash] = true;
    blocks_downloaded_++;
    
    // Remove from pending
    auto it = std::find(pending_blocks_.begin(), pending_blocks_.end(), block_hash);
    if (it != pending_blocks_.end()) {
        pending_blocks_.erase(it);
    }
    
    std::cout << "[P2P] Downloaded block " << block_hash.substr(0, 8) 
              << "... (" << blocks_downloaded_ << "/" << headers_chain_.size() << ")" << std::endl;
    
    // Check if all blocks downloaded
    if (pending_blocks_.empty()) {
        std::cout << "[P2P] All blocks downloaded, sync complete!" << std::endl;
        transitionToState(SyncState::SYNCED);
    }
    
    return true;
}

uint32_t HeadersFirstSync::getBestHeight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return best_height_;
}

std::string HeadersFirstSync::getBestBlockHash() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return best_block_hash_;
}

uint32_t HeadersFirstSync::getHeadersCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headers_chain_.size();
}

uint32_t HeadersFirstSync::getBlocksDownloaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_downloaded_;
}

void HeadersFirstSync::setMaxHeadersPerRequest(uint32_t max_headers) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_headers_per_request_ = max_headers;
}

void HeadersFirstSync::setValidationEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    validation_enabled_ = enabled;
}

void HeadersFirstSync::setTimeout(std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_ = timeout;
}

din::Json HeadersFirstSync::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    din::Json status;
    status["state"] = static_cast<int>(state_);
    status["state_name"] = [this]() {
        switch (state_) {
            case SyncState::IDLE: return "idle";
            case SyncState::REQUESTING_HEADERS: return "requesting_headers";
            case SyncState::VALIDATING_HEADERS: return "validating_headers";
            case SyncState::REQUESTING_BLOCKS: return "requesting_blocks";
            case SyncState::SYNCED: return "synced";
            case SyncState::ERROR: return "error";
            default: return "unknown";
        }
    }();
    status["active_peer"] = active_peer_;
    status["best_height"] = best_height_;
    status["best_block_hash"] = best_block_hash_;
    status["headers_count"] = static_cast<uint32_t>(headers_chain_.size());
    status["blocks_downloaded"] = blocks_downloaded_;
    status["pending_blocks"] = static_cast<uint32_t>(pending_blocks_.size());
    status["rpc_schema"] = "din.rpc.v1";
    
    return status;
}

din::Json HeadersFirstSync::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    din::Json metrics;
    metrics["total_headers_received"] = total_headers_received_;
    metrics["total_blocks_requested"] = total_blocks_requested_;
    metrics["blocks_downloaded"] = blocks_downloaded_;
    
    if (sync_start_time_.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::steady_clock::now() - sync_start_time_;
        metrics["sync_duration_seconds"] = Json::Int64(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    }
    
    metrics["rpc_schema"] = "din.rpc.v1";
    return metrics;
}

bool HeadersFirstSync::validateHeadersChain(const std::vector<BlockHeader>& headers) {
    if (headers.empty()) {
        return true;
    }
    
    const BlockHeader* prev_header = nullptr;
    if (!headers_chain_.empty()) {
        prev_header = &headers_chain_.back();
    }
    
    for (const auto& header : headers) {
        if (!validateHeader(header, prev_header)) {
            return false;
        }
        prev_header = &header;
    }
    
    return true;
}

bool HeadersFirstSync::validateHeader(const BlockHeader& header, const BlockHeader* prev_header) {
    // Basic validation
    if (header.hash.empty() || header.hash.length() != 64) {
        std::cout << "[P2P] Invalid header hash length" << std::endl;
        return false;
    }
    
    if (prev_header) {
        // Check height sequence
        if (header.height != prev_header->height + 1) {
            std::cout << "[P2P] Invalid header height sequence" << std::endl;
            return false;
        }
        
        // Check previous block hash
        if (header.prev_block_hash != prev_header->hash) {
            std::cout << "[P2P] Invalid previous block hash" << std::endl;
            return false;
        }
        
        // Check timestamp (must be greater than previous)
        if (header.timestamp <= prev_header->timestamp) {
            std::cout << "[P2P] Invalid header timestamp" << std::endl;
            return false;
        }
    }
    
    return true;
}

void HeadersFirstSync::updateBestChain() {
    if (!headers_chain_.empty()) {
        const auto& best_header = headers_chain_.back();
        best_height_ = best_header.height;
        best_block_hash_ = best_header.hash;
    }
}

void HeadersFirstSync::transitionToState(SyncState new_state) {
    if (state_ != new_state) {
        std::cout << "[P2P] State transition: " << static_cast<int>(state_) 
                  << " -> " << static_cast<int>(new_state) << std::endl;
        state_ = new_state;
    }
}

bool HeadersFirstSync::isTimeout() const {
    auto now = std::chrono::steady_clock::now();
    return (now - last_activity_) > timeout_;
}

void HeadersFirstSync::logSyncProgress() {
    if (headers_chain_.size() % 1000 == 0) {
        std::cout << "[P2P] Sync progress: " << headers_chain_.size() 
                  << " headers, height " << best_height_ << std::endl;
    }
}

// Architecture V3: Init/Shutdown removed - use HeadersSyncService lifecycle methods
// Deprecated functions - use HeadersSyncService::Init/Start/Stop instead

} // namespace p2p
} // namespace dinero
