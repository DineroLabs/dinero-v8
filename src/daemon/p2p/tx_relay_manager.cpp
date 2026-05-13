#include "p2p/tx_relay_manager.h"
#include "common/logger.h"
#include <algorithm>
#include <random>
#include <chrono>

namespace dinero {

// Global instance
std::unique_ptr<TxRelayManager> g_tx_relay_manager;

// RollingFilter implementation
RollingFilter::RollingFilter(size_t max_size) : max_size_(max_size) {
    items_.reserve(max_size);
    insertion_order_.reserve(max_size);
}

bool RollingFilter::Contains(const std::string& item) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return items_.find(item) != items_.end();
}

void RollingFilter::Insert(const std::string& item) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (items_.find(item) != items_.end()) {
        return; // Already exists
    }
    
    // If at capacity, evict oldest item
    if (insertion_order_.size() >= max_size_) {
        const std::string& oldest = insertion_order_[next_evict_idx_];
        items_.erase(oldest);
        insertion_order_[next_evict_idx_] = item;
        next_evict_idx_ = (next_evict_idx_ + 1) % max_size_;
    } else {
        insertion_order_.push_back(item);
    }
    
    items_.insert(item);
}

void RollingFilter::Clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    items_.clear();
    insertion_order_.clear();
    next_evict_idx_ = 0;
}

// TxRelayManager implementation
TxRelayManager::TxRelayManager(TxMempool& mempool, const TxRelayConfig& config)
    : mempool_(mempool), config_(config), rng_(std::random_device{}()) {
}

TxRelayManager::~TxRelayManager() {
    Stop();
}

void TxRelayManager::Start() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_) return;
    
    running_ = true;
    dinero::g_logger.info("✅ Transaction relay manager started");
}

void TxRelayManager::Stop() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!running_) return;
    
    running_ = false;
    peers_.clear();
    trickle_queue_.clear();
    
    dinero::g_logger.info("Transaction relay manager stopped");
}

void TxRelayManager::AddPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (peers_.find(peer_id) != peers_.end()) {
        return; // Already exists
    }
    
    peers_[peer_id] = std::make_unique<PeerTxState>(peer_id);
    stats_.active_peers = peers_.size();
    
    dinero::g_logger.debug("Added peer to transaction relay: " + peer_id);
}

void TxRelayManager::RemovePeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        // Clean up pending requests for this peer
        stats_.pending_requests -= it->second->outstanding_getdata.size();
        peers_.erase(it);
        stats_.active_peers = peers_.size();
        
        dinero::g_logger.debug("Removed peer from transaction relay: " + peer_id);
    }
}

void TxRelayManager::SetPeerFeeFilter(const std::string& peer_id, uint64_t min_feerate) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second->fee_filter = min_feerate;
        dinero::g_logger.debug("Set fee filter for peer " + peer_id + ": " + std::to_string(min_feerate) + " sat/kB");
    }
}

void TxRelayManager::OnTransactionAccepted(const Transaction& tx, uint64_t fee) {
    if (!running_ || !config_.enabled) return;
    
    std::string txid = tx.GetTxId();
    
    // Calculate fee rate for filtering
    TxMempoolEntry temp_entry(tx, fee, 0);
    uint64_t feerate_sat_kb = (fee * 1000) / temp_entry.GetVSize();
    
    dinero::g_logger.debug("Announcing new transaction: " + txid + " (fee rate: " + std::to_string(feerate_sat_kb) + " sat/kB)");
    
    // Announce to peers with trickle delay for privacy
    if (config_.privacy_mode) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        auto target_peers = SelectAnnouncePeers(txid, feerate_sat_kb);
        if (!target_peers.empty()) {
            TrickleEntry entry;
            entry.txid = txid;
            entry.announce_time = std::chrono::steady_clock::now() + CalculateTrickleDelay();
            entry.target_peers = std::move(target_peers);
            
            trickle_queue_.push_back(std::move(entry));
        }
    } else {
        // Immediate announcement
        AnnounceToAllPeers(txid, feerate_sat_kb);
    }
    
    stats_.transactions_announced++;
}

void TxRelayManager::OnBlockConnected(const std::vector<std::string>& tx_hashes) {
    if (!running_) return;
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Remove confirmed transactions from all peer filters
    for (const auto& txid : tx_hashes) {
        for (auto& [peer_id, peer_state] : peers_) {
            peer_state->recently_announced.Insert(txid); // Mark as seen
            peer_state->outstanding_getdata.erase(txid);
        }
        
        // Remove from trickle queue
        trickle_queue_.erase(
            std::remove_if(trickle_queue_.begin(), trickle_queue_.end(),
                [&txid](const TrickleEntry& entry) { return entry.txid == txid; }),
            trickle_queue_.end()
        );
    }
    
    dinero::g_logger.debug("Processed block with " + std::to_string(tx_hashes.size()) + " transactions");
}

void TxRelayManager::OnPeerInv(const std::string& peer_id, const std::vector<InvVector>& invs) {
    if (!running_) return;
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return;
    
    auto& peer_state = *peer_it->second;
    std::vector<std::string> to_request;
    
    for (const auto& inv : invs) {
        if (inv.type != P2PMessageType::TX) continue;
        
        const std::string& txid = inv.hash;
        
        // Check if we should request this transaction
        if (ShouldRequestFromPeer(peer_id, txid)) {
            to_request.push_back(txid);
            peer_state.recently_requested.Insert(txid);
            peer_state.outstanding_getdata.insert(txid);
        }
    }
    
    // Send getdata for requested transactions
    if (!to_request.empty()) {
        for (const auto& txid : to_request) {
            RequestTransaction(peer_id, txid);
        }
        
        stats_.transactions_requested += to_request.size();
        stats_.pending_requests += to_request.size();
        
        dinero::g_logger.debug("Requested " + std::to_string(to_request.size()) + " transactions from peer " + peer_id);
    }
}

void TxRelayManager::OnPeerGetData(const std::string& peer_id, const std::vector<std::string>& tx_hashes) {
    if (!running_) return;
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return;
    
    for (const auto& txid : tx_hashes) {
        // Check if we have this transaction in mempool
        const auto* entry = mempool_.Get(txid);
        if (entry) {
            // Check rate limits
            TxMempoolEntry temp_entry(entry->tx, entry->fee, 0);
            size_t tx_size = temp_entry.GetSize();
            
            if (CheckRateLimit(peer_id, tx_size)) {
                SendTransaction(peer_id, entry->tx);
                UpdateBandwidthUsage(peer_id, tx_size, true);
                stats_.transactions_relayed++;
                stats_.bytes_relayed += tx_size;
            } else {
                dinero::g_logger.debug("Rate limit exceeded for peer " + peer_id + ", dropping tx " + txid);
                stats_.transactions_dropped++;
            }
        } else {
            dinero::g_logger.debug("Transaction not found in mempool: " + txid);
            // Could send NOTFOUND message here
        }
    }
}

void TxRelayManager::OnPeerTx(const std::string& peer_id, const Transaction& tx) {
    if (!running_) return;
    
    std::string txid = tx.GetTxId();
    
    // Update bandwidth usage
    TxMempoolEntry temp_entry(tx, 0, 0);
    size_t tx_size = temp_entry.GetSize();
    UpdateBandwidthUsage(peer_id, tx_size, false);
    
    // Try to accept to mempool (would need actual UTXO view)
    // auto outcome = AcceptToMemoryPool(tx, mempool_, mempool_.GetPolicy(), utxo_view);
    // For now, just log the transaction
    dinero::g_logger.debug("Received transaction from peer " + peer_id + ": " + txid);
    
    // Mark as received from this peer to avoid echoing back
    std::lock_guard<std::mutex> lock(mtx_);
    auto peer_it = peers_.find(peer_id);
    if (peer_it != peers_.end()) {
        peer_it->second->recently_announced.Insert(txid);
        peer_it->second->outstanding_getdata.erase(txid);
        if (stats_.pending_requests > 0) stats_.pending_requests--;
        
        // Relay to other peers (will be handled by OnTransactionAccepted)
    }
}

void TxRelayManager::OnPeerFeeFilter(const std::string& peer_id, uint64_t min_feerate) {
    SetPeerFeeFilter(peer_id, min_feerate);
}

void TxRelayManager::OnPeerMempoolRequest(const std::string& peer_id) {
    if (!running_) return;
    
    // Rate limit mempool requests
    std::lock_guard<std::mutex> lock(mtx_);
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return;
    
    auto& peer_state = *peer_it->second;
    auto now = std::chrono::steady_clock::now();
    
    // Allow one mempool request per 10 minutes
    if (now - peer_state.last_inv_time < std::chrono::minutes(10)) {
        dinero::g_logger.debug("Rate limiting mempool request from peer " + peer_id);
        return;
    }
    
    peer_state.last_inv_time = now;
    
    // Send inventory of all mempool transactions that pass fee filter
    auto entries = mempool_.GetEntries();
    std::vector<InvVector> invs;
    
    for (const auto& entry : entries) {
        uint64_t feerate_sat_kb = (entry.fee * 1000) / entry.vsize;
        if (feerate_sat_kb >= peer_state.fee_filter) {
            invs.emplace_back(P2PMessageType::TX, entry.txid);
        }
    }
    
    if (!invs.empty()) {
        SendInvBatch(peer_id, invs);
        dinero::g_logger.debug("Sent mempool inventory to peer " + peer_id + ": " + std::to_string(invs.size()) + " transactions");
    }
}

void TxRelayManager::ProcessTrickleQueue() {
    if (!running_ || !config_.privacy_mode) return;
    
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    
    // Process entries ready for announcement
    auto it = trickle_queue_.begin();
    while (it != trickle_queue_.end()) {
        if (now >= it->announce_time) {
            AnnounceToSpecificPeers(it->txid, it->target_peers);
            it = trickle_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

void TxRelayManager::ProcessTimeouts() {
    if (!running_) return;
    
    CleanupExpiredRequests();
    CleanupOldBandwidthData();
}

void TxRelayManager::UpdateMetrics() {
    if (!running_) return;
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Update peer count
    stats_.active_peers = peers_.size();
    
    // Count pending requests
    size_t total_pending = 0;
    for (const auto& [peer_id, peer_state] : peers_) {
        total_pending += peer_state->outstanding_getdata.size();
    }
    stats_.pending_requests = total_pending;
    
    // Log periodic stats
    static auto last_log = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::minutes(1)) {
        dinero::g_logger.info("TX Relay Stats - Peers: " + std::to_string(stats_.active_peers) + 
                             ", Announced: " + std::to_string(stats_.transactions_announced) +
                             ", Relayed: " + std::to_string(stats_.transactions_relayed) +
                             ", Pending: " + std::to_string(stats_.pending_requests));
        last_log = now;
    }
}

TxRelayManager::Stats TxRelayManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

std::vector<std::string> TxRelayManager::GetConnectedPeers() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<std::string> peer_ids;
    peer_ids.reserve(peers_.size());
    
    for (const auto& [peer_id, peer_state] : peers_) {
        peer_ids.push_back(peer_id);
    }
    
    return peer_ids;
}

void TxRelayManager::UpdateConfig(const TxRelayConfig& config) {
    std::lock_guard<std::mutex> lock(mtx_);
    config_ = config;
}

// Private methods
void TxRelayManager::AnnounceToAllPeers(const std::string& txid, uint64_t fee) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto target_peers = SelectAnnouncePeers(txid, fee);
    AnnounceToSpecificPeers(txid, target_peers);
}

void TxRelayManager::AnnounceToSpecificPeers(const std::string& txid, const std::vector<std::string>& peer_ids) {
    for (const auto& peer_id : peer_ids) {
        auto peer_it = peers_.find(peer_id);
        if (peer_it != peers_.end()) {
            auto& peer_state = *peer_it->second;
            
            // Add to pending invs
            peer_state.pending_invs.emplace_back(P2PMessageType::TX, txid);
            peer_state.recently_announced.Insert(txid);
            
            // Send batch if ready
            if (peer_state.pending_invs.size() >= config_.max_inv_batch_size) {
                SendInvBatch(peer_id, peer_state.pending_invs);
                peer_state.pending_invs.clear();
            }
        }
    }
}

void TxRelayManager::RequestTransaction(const std::string& peer_id, const std::string& txid) {
    // Implementation would send getdata message to peer
    dinero::g_logger.debug("Requesting transaction " + txid + " from peer " + peer_id);
}

void TxRelayManager::SendTransaction(const std::string& peer_id, const Transaction& tx) {
    // Implementation would send tx message to peer
    dinero::g_logger.debug("Sending transaction " + tx.GetTxId() + " to peer " + peer_id);
}

void TxRelayManager::SendInvBatch(const std::string& peer_id, const std::vector<InvVector>& invs) {
    // Implementation would send inv message to peer
    stats_.inv_messages_sent++;
    dinero::g_logger.debug("Sending " + std::to_string(invs.size()) + " invs to peer " + peer_id);
}

bool TxRelayManager::ShouldAnnounceToPeer(const std::string& peer_id, const std::string& txid, uint64_t fee) const {
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return false;
    
    const auto& peer_state = *peer_it->second;
    
    // Check fee filter
    if (fee < peer_state.fee_filter) return false;
    
    // Check if already announced
    if (peer_state.recently_announced.Contains(txid)) return false;
    
    // Check misbehavior score
    if (peer_state.misbehavior_score > 100) return false;
    
    return true;
}

bool TxRelayManager::ShouldRequestFromPeer(const std::string& peer_id, const std::string& txid) const {
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return false;
    
    const auto& peer_state = *peer_it->second;
    
    // Check if already have or requested
    if (mempool_.Exists(txid)) return false;
    if (peer_state.recently_requested.Contains(txid)) return false;
    if (peer_state.outstanding_getdata.count(txid)) return false;
    
    // Check outstanding request limit
    if (peer_state.outstanding_getdata.size() >= peer_state.max_outstanding) return false;
    
    return true;
}

std::vector<std::string> TxRelayManager::SelectAnnouncePeers(const std::string& txid, uint64_t fee) const {
    std::vector<std::string> candidates;
    
    for (const auto& [peer_id, peer_state] : peers_) {
        if (ShouldAnnounceToPeer(peer_id, txid, fee)) {
            candidates.push_back(peer_id);
        }
    }
    
    // Limit to max peers and shuffle for privacy
    if (candidates.size() > config_.max_peers_announce) {
        ShufflePeers(candidates);
        candidates.resize(config_.max_peers_announce);
    }
    
    return candidates;
}

bool TxRelayManager::CheckRateLimit(const std::string& peer_id, size_t bytes) const {
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return false;
    
    const auto& peer_state = *peer_it->second;
    return peer_state.bytes_sent_today + bytes <= config_.max_bytes_per_peer_day;
}

void TxRelayManager::UpdateBandwidthUsage(const std::string& peer_id, size_t bytes, bool outbound) {
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return;
    
    auto& peer_state = *peer_it->second;
    
    if (outbound) {
        peer_state.bytes_sent_today += bytes;
    } else {
        peer_state.bytes_received_today += bytes;
    }
}

void TxRelayManager::IncrementMisbehavior(const std::string& peer_id, uint32_t score) {
    auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end()) return;
    
    auto& peer_state = *peer_it->second;
    peer_state.misbehavior_score += score;
    peer_state.last_misbehavior = std::chrono::steady_clock::now();
    
    if (peer_state.misbehavior_score > 100) {
        dinero::g_logger.warning("Peer " + peer_id + " misbehavior score: " + std::to_string(peer_state.misbehavior_score));
    }
}

std::chrono::milliseconds TxRelayManager::CalculateTrickleDelay() const {
    // Exponential distribution for privacy
    std::exponential_distribution<double> dist(1.0 / config_.trickle_delay_ms);
    return std::chrono::milliseconds(static_cast<uint32_t>(dist(rng_)));
}

void TxRelayManager::ShufflePeers(std::vector<std::string>& peers) const {
    std::shuffle(peers.begin(), peers.end(), rng_);
}

void TxRelayManager::CleanupExpiredRequests() {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [peer_id, peer_state] : peers_) {
        // Remove requests older than 60 seconds
        // This would need actual timestamp tracking per request
        if (now - peer_state->last_tx_time > std::chrono::seconds(60)) {
            size_t old_size = peer_state->outstanding_getdata.size();
            peer_state->outstanding_getdata.clear();
            stats_.pending_requests -= old_size;
        }
    }
}

void TxRelayManager::CleanupOldBandwidthData() {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [peer_id, peer_state] : peers_) {
        // Reset daily bandwidth counters
        if (now - peer_state->day_start > std::chrono::hours(24)) {
            peer_state->bytes_sent_today = 0;
            peer_state->bytes_received_today = 0;
            peer_state->day_start = now;
        }
        
        // Decay misbehavior score
        if (now - peer_state->last_misbehavior > std::chrono::hours(1)) {
            peer_state->misbehavior_score = std::max(0u, peer_state->misbehavior_score - 1);
        }
    }
}

// Global functions
void InitializeTxRelay(TxMempool& mempool, const TxRelayConfig& config) {
    if (g_tx_relay_manager) {
        dinero::g_logger.warning("Transaction relay manager already initialized");
        return;
    }
    
    g_tx_relay_manager = std::make_unique<TxRelayManager>(mempool, config);
    g_tx_relay_manager->Start();
    
    dinero::g_logger.info("✅ Transaction relay manager initialized");
}

void ShutdownTxRelay() {
    if (g_tx_relay_manager) {
        g_tx_relay_manager->Stop();
        g_tx_relay_manager.reset();
        dinero::g_logger.info("Transaction relay manager shutdown");
    }
}

} // namespace dinero
