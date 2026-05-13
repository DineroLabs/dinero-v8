#include "daemon/services/fast_sync_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"
#include "common/status.h"
#include "primitives/block.h"
#include <sstream>
#include <filesystem>
#include <optional>
#include <cctype>
#include <algorithm>

namespace dinero {
namespace daemon {

namespace {

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseFixedHexBytes(const std::string& hex, size_t expected_bytes, std::vector<uint8_t>& out) {
    if (hex.size() != expected_bytes * 2) {
        return false;
    }
    out.clear();
    out.reserve(expected_bytes);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

}  // namespace

FastSyncService::FastSyncService() = default;
FastSyncService::~FastSyncService() = default;

bool FastSyncService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    g_logger.info("[FastSyncService] Initialized");
    return true;
}

bool FastSyncService::Start() {
    g_logger.info("[FastSyncService] Started");
    return true;
}

void FastSyncService::Stop() {
    if (isSyncing()) {
        g_logger.info("[FastSyncService] Stopping fast sync in progress");
        transitionState(State::Idle);
    }
    g_logger.info("[FastSyncService] Stopped");
}

bool FastSyncService::IsHealthy() const {
    return state_.load() != State::Failed;
}

std::string FastSyncService::GetMetrics() const {
    std::ostringstream oss;
    oss << "{"
        << "\"state\": \"" << getStateString() << "\","
        << "\"current_height\": " << current_height_.load() << ","
        << "\"target_height\": " << target_height_.load() << ","
        << "\"headers_received\": " << headers_received_.load() << ","
        << "\"progress\": " << getProgress()
        << "}";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════════════
// Fast Sync Logic
// ═══════════════════════════════════════════════════════════════════════════

bool FastSyncService::shouldFastSync() const {
    // Check if --fastsync flag was passed
    if (force_fast_sync_) {
        g_logger.info("[FastSyncService] Fast sync enabled via --fastsync flag");
        return true;
    }

    // Check if this is a fresh node (no blockchain data)
    if (ctx_ && ctx_->chainstate) {
        uint32_t current_height = ctx_->chainstate->getBlockHeight();
        if (current_height == 0) {
            g_logger.info("[FastSyncService] Fresh node detected - fast sync recommended");
            return enabled_;
        }
    }

    return false;
}

bool FastSyncService::startFastSync(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (isSyncing()) {
        g_logger.warning("[FastSyncService] Already syncing");
        return false;
    }

    g_logger.info("[FastSyncService] Starting fast sync from peer: " + peer_id);

    sync_peer_id_ = peer_id;
    current_height_ = 0;
    target_height_ = 0;
    headers_received_ = 0;

    // Start by requesting headers
    transitionState(State::RequestingHeaders);
    requestHeaders(peer_id);

    return true;
}

bool FastSyncService::handleHeaders(const std::string& peer_id,
                                    const HeadersMessage& headers) {
    if (state_.load() != State::RequestingHeaders) {
        return false;
    }

    if (peer_id != sync_peer_id_) {
        g_logger.warning("[FastSyncService] Headers from unexpected peer: " + peer_id);
        return false;
    }

    g_logger.info("[FastSyncService] Received " + std::to_string(headers.headers.size()) +
                 " headers from " + peer_id);

    if (headers.headers.size() > MAX_HEADERS_SIZE) {
        g_logger.error("[FastSyncService] Rejecting header batch larger than protocol limit: " +
                       std::to_string(headers.headers.size()));
        return false;
    }

    // Validate basic header integrity and in-batch linkage before requesting more data.
    std::optional<uint256> expected_prev;
    for (size_t i = 0; i < headers.headers.size(); ++i) {
        const auto& header_bytes = headers.headers[i];
        auto parsed = BlockHeader::Deserialize(header_bytes);
        if (!parsed.has_value()) {
            g_logger.error("[FastSyncService] Failed to deserialize header #" + std::to_string(i));
            return false;
        }

        if (expected_prev.has_value() && parsed->prev_block_hash != *expected_prev) {
            g_logger.error("[FastSyncService] Header chain linkage failure at index " +
                           std::to_string(i));
            return false;
        }
        expected_prev = parsed->GetHash();
    }

    headers_received_ += headers.headers.size();
    target_height_ = std::max(target_height_.load(),
                              current_height_.load() + headers_received_.load());

    if (headers.headers.empty()) {
        // No more headers - we've reached the tip
        // Now request Utreexo state
        g_logger.info("[FastSyncService] All headers received, requesting Utreexo state");
        transitionState(State::RequestingState);
        requestUtreexoState(peer_id);
    } else {
        // Request more headers
        requestHeaders(peer_id);
    }

    return true;
}

bool FastSyncService::handleUtreexoState(const std::string& peer_id,
                                         const UtreexoStateMessage& state) {
    if (state_.load() != State::RequestingState) {
        return false;
    }

    if (peer_id != sync_peer_id_) {
        g_logger.warning("[FastSyncService] State from unexpected peer: " + peer_id);
        return false;
    }

    g_logger.info("[FastSyncService] Received Utreexo state at height " +
                 std::to_string(state.height) + " with " +
                 std::to_string(state.roots.size()) + " roots");

    transitionState(State::ValidatingState);

    if (verifyAndImportState(state)) {
        transitionState(State::Completed);
        onSyncComplete(true);
        return true;
    } else {
        transitionState(State::Failed);
        onSyncComplete(false);
        return false;
    }
}

void FastSyncService::requestHeaders(const std::string& peer_id) {
    if (!ctx_ || !ctx_->p2p) {
        g_logger.error("[FastSyncService] P2P service not available");
        return;
    }

    // Create getheaders message
    GetheadersMessage getheaders;
    getheaders.version = 70015;

    // Use block locator from current best header
    // For fresh node, this will be genesis
    getheaders.block_locator_hashes.push_back(
        "0000000000000000000000000000000000000000000000000000000000000000");

    getheaders.hash_stop = std::string(64, '0');  // No stop hash

    g_logger.info("[FastSyncService] Requesting headers from " + peer_id);

    // Send via P2P service
    // ctx_->p2p->sendMessage(peer_id, getheaders);
}

void FastSyncService::requestUtreexoState(const std::string& peer_id) {
    if (!ctx_ || !ctx_->p2p) {
        g_logger.error("[FastSyncService] P2P service not available");
        return;
    }

    // Create getutreexostate message
    GetUtreexoStateMessage request;
    request.block_hash = "";  // Empty = request current tip

    g_logger.info("[FastSyncService] Requesting Utreexo state from " + peer_id);

    // Send via P2P service
    // ctx_->p2p->sendMessage(peer_id, request);
}

bool FastSyncService::verifyAndImportState(const UtreexoStateMessage& state) {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.error("[FastSyncService] Chainstate service not available");
        return false;
    }

    // 1. Verify the state commitment matches what we computed from roots
    if (!state.verifyCommitment()) {
        g_logger.error("[FastSyncService] State commitment verification failed");
        return false;
    }

    // 2. Verify commitment matches the block header's Utreexo root in ChainDB.
    auto* chain_db = ctx_->chainstate->GetChainDB();
    if (!chain_db) {
        g_logger.error("[FastSyncService] ChainDB not available for state verification");
        return false;
    }

    uint256 state_block_hash;
    if (!uint256::FromHex(state.block_hash, state_block_hash)) {
        g_logger.error("[FastSyncService] Invalid state block hash: " + state.block_hash);
        return false;
    }

    auto header_result = chain_db->getHeader(state_block_hash);
    if (!header_result.ok()) {
        g_logger.error("[FastSyncService] Missing header for state block hash (" +
                       std::string(StatusToString(header_result.status())) + ")");
        return false;
    }

    uint256 state_commitment_hash;
    if (!uint256::FromHex(state.commitment, state_commitment_hash)) {
        g_logger.error("[FastSyncService] Invalid state commitment hex");
        return false;
    }

    if (header_result.value().utreexo_root != state_commitment_hash) {
        g_logger.error("[FastSyncService] Utreexo commitment mismatch: state does not match header");
        return false;
    }

    const std::string block_hash_prefix =
        state.block_hash.size() > 16 ? state.block_hash.substr(0, 16) : state.block_hash;
    g_logger.info("[FastSyncService] Verified Utreexo commitment against header " +
                 block_hash_prefix + "...");

    // 3. Import the state into GlobalUTXOSet
    consensus::GlobalUTXOSet::UtreexoSnapshot snapshot;
    snapshot.block_hash = state.block_hash;
    snapshot.height = state.height;
    snapshot.num_leaves = state.num_leaves;

    // Convert string roots to Hash256
    for (const auto& root_hex : state.roots) {
        consensus::UtreexoHash root;
        if (!ParseFixedHexBytes(root_hex, 32, root)) {
            g_logger.error("[FastSyncService] Invalid Utreexo root hex in snapshot");
            return false;
        }
        snapshot.roots.push_back(std::move(root));
    }

    // Convert commitment
    if (!ParseFixedHexBytes(state.commitment, 32, snapshot.commitment)) {
        g_logger.error("[FastSyncService] Invalid commitment hex length/content");
        return false;
    }

    // Import into UTXO set
    if (!ctx_->chainstate->getGlobalUTXOSet()->importUtreexoSnapshot(snapshot)) {
        g_logger.error("[FastSyncService] Failed to import Utreexo snapshot");
        return false;
    }

    // Update heights
    current_height_ = state.height;
    target_height_ = state.height;

    g_logger.info("[FastSyncService] Successfully imported Utreexo state at height " +
                 std::to_string(state.height));
    g_logger.info("[FastSyncService] Node is now in stateless mode");

    return true;
}

void FastSyncService::transitionState(State new_state) {
    State old_state = state_.exchange(new_state);
    g_logger.info("[FastSyncService] State: " + getStateString());
    (void)old_state;  // Suppress unused variable warning
}

void FastSyncService::onSyncComplete(bool success) {
    if (success) {
        g_logger.info("[FastSyncService] Fast sync completed successfully!");
    } else {
        g_logger.error("[FastSyncService] Fast sync failed");
    }

    if (completion_callback_) {
        completion_callback_(success);
    }
}

void FastSyncService::setCompletionCallback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

double FastSyncService::getProgress() const {
    uint32_t target = target_height_.load();
    if (target == 0) return 0.0;

    uint32_t current = current_height_.load();
    return static_cast<double>(current) / static_cast<double>(target);
}

std::string FastSyncService::getStateString() const {
    switch (state_.load()) {
        case State::Idle: return "Idle";
        case State::RequestingHeaders: return "RequestingHeaders";
        case State::RequestingState: return "RequestingState";
        case State::ValidatingState: return "ValidatingState";
        case State::Completed: return "Completed";
        case State::Failed: return "Failed";
        default: return "Unknown";
    }
}

} // namespace daemon
} // namespace dinero
