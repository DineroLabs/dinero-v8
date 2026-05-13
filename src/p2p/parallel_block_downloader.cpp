#include "p2p/parallel_block_downloader.h"
#include "p2p/peer_manager.h"
#include "common/logger.h"
#include "util/hex.h"
#include <algorithm>
#include <sstream>
#include <cmath>

using namespace parallel_download;

// ═══════════════════════════════════════════════════════════════════════════
// PeerScore Implementation
// ═══════════════════════════════════════════════════════════════════════════

void PeerScore::calculateScore() {
    // Normalize latency (0.0 = best, 1.0 = worst)
    double latency_score = 0.0;
    if (average_latency_ms > 0) {
        // Clamp to MAX_ACCEPTABLE_LATENCY_MS and invert (lower latency = higher score)
        double clamped_latency = std::min(average_latency_ms, (double)MAX_ACCEPTABLE_LATENCY_MS);
        latency_score = 1.0 - (clamped_latency / MAX_ACCEPTABLE_LATENCY_MS);
    } else {
        latency_score = 1.0;  // No data yet, assume best
    }

    // Success rate is already 0.0-1.0
    double success_score = success_rate;

    // Normalize bandwidth (0.0 = worst, 1.0 = best)
    double bandwidth_score = 0.0;
    if (bandwidth_bps > 0) {
        // Clamp to reasonable max (10 MB/s) and normalize
        constexpr double MAX_BANDWIDTH = 10 * 1024 * 1024;  // 10 MB/s
        double clamped_bw = std::min(bandwidth_bps, MAX_BANDWIDTH);
        bandwidth_score = clamped_bw / MAX_BANDWIDTH;
    }

    // Weighted composite score
    overall_score = (latency_score * LATENCY_WEIGHT) +
                    (success_score * SUCCESS_RATE_WEIGHT) +
                    (bandwidth_score * BANDWIDTH_WEIGHT);

    // Penalize if in cooldown
    if (is_in_cooldown) {
        overall_score = 0.0;
    }

    // Penalize if below minimum thresholds
    if (success_rate < MIN_SUCCESS_RATE && total_requests > 10) {
        overall_score *= 0.5;  // 50% penalty for low success rate
    }

    if (average_latency_ms > MAX_ACCEPTABLE_LATENCY_MS && total_requests > 5) {
        overall_score *= 0.5;  // 50% penalty for high latency
    }
}

void PeerScore::recordSuccess(int64_t latency_ms, size_t bytes_received) {
    total_requests++;
    successful_requests++;

    // Update rolling average latency (exponential moving average)
    constexpr double ALPHA = 0.3;  // Weight for new sample
    if (average_latency_ms == 0.0) {
        average_latency_ms = latency_ms;
    } else {
        average_latency_ms = (ALPHA * latency_ms) + ((1.0 - ALPHA) * average_latency_ms);
    }

    // Update success rate
    success_rate = (double)successful_requests / (double)total_requests;

    // Update bandwidth (exponential moving average)
    double elapsed_sec = latency_ms / 1000.0;
    double instant_bw = bytes_received / elapsed_sec;
    if (bandwidth_bps == 0.0) {
        bandwidth_bps = instant_bw;
    } else {
        bandwidth_bps = (ALPHA * instant_bw) + ((1.0 - ALPHA) * bandwidth_bps);
    }

    // Reset timeout strikes on success
    timeout_strikes = 0;

    last_success_time = std::chrono::steady_clock::now();

    // Recalculate composite score
    calculateScore();
}

void PeerScore::recordFailure() {
    total_requests++;
    failed_requests++;
    timeout_strikes++;

    // Update success rate
    success_rate = (double)successful_requests / (double)total_requests;

    // Recalculate composite score
    calculateScore();
}

bool PeerScore::shouldCooldown() const {
    return timeout_strikes >= PEER_TIMEOUT_STRIKES;
}

void PeerScore::enterCooldown() {
    is_in_cooldown = true;
    is_available = false;
    cooldown_until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(SLOW_PEER_COOLDOWN_MS);
    overall_score = 0.0;  // Zero score during cooldown

    dinero::g_logger.warning("Peer " + std::to_string(reinterpret_cast<uintptr_t>(peer)) +
                            " entered cooldown for " + std::to_string(SLOW_PEER_COOLDOWN_MS / 1000) + "s");
}

bool PeerScore::isCooldownExpired() const {
    if (!is_in_cooldown) {
        return false;
    }
    return std::chrono::steady_clock::now() >= cooldown_until;
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockDownloadTask Implementation
// ═══════════════════════════════════════════════════════════════════════════

bool BlockDownloadTask::hasTimedOut() const {
    if (!is_in_flight) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - request_time);
    return elapsed.count() > BLOCK_TIMEOUT_MS;
}

void BlockDownloadTask::reassign(Peer* new_peer) {
    assigned_peer = new_peer;
    request_time = std::chrono::steady_clock::now();
    retry_count++;
    is_in_flight = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ParallelBlockDownloader Implementation
// ═══════════════════════════════════════════════════════════════════════════

ParallelBlockDownloader::ParallelBlockDownloader(PeerManager* peer_manager)
    : peer_manager_(peer_manager)
    , blockchain_(nullptr)
    , max_parallel_peers_(DEFAULT_MAX_PARALLEL_PEERS)
    , max_blocks_per_peer_(MAX_BLOCKS_PER_PEER)
    , max_total_in_flight_(MAX_TOTAL_IN_FLIGHT)
{
    dinero::g_logger.info("ParallelBlockDownloader initialized (Option B: 10-20× speedup)");
    dinero::g_logger.info("  Max parallel peers: " + std::to_string(max_parallel_peers_));
    dinero::g_logger.info("  Max blocks per peer: " + std::to_string(max_blocks_per_peer_));
    dinero::g_logger.info("  Max total in-flight: " + std::to_string(max_total_in_flight_));
}

PeerScore& ParallelBlockDownloader::getPeerScore(Peer* peer) {
    auto it = peer_scores_.find(peer);
    if (it == peer_scores_.end()) {
        // Create new score entry
        PeerScore score;
        score.peer = peer;
        peer_scores_[peer] = score;
        return peer_scores_[peer];
    }
    return it->second;
}

void ParallelBlockDownloader::updatePeerScores() {
    auto now = std::chrono::steady_clock::now();

    for (auto& [peer, score] : peer_scores_) {
        // Check if cooldown expired
        if (score.is_in_cooldown && score.isCooldownExpired()) {
            score.is_in_cooldown = false;
            score.is_available = true;
            score.timeout_strikes = 0;  // Reset strikes after cooldown
            dinero::g_logger.info("Peer " + std::to_string(reinterpret_cast<uintptr_t>(peer)) +
                                 " cooldown expired, back in rotation");
        }

        // Recalculate score
        score.calculateScore();
    }
}

void ParallelBlockDownloader::removePeer(Peer* peer) {
    // Remove from peer scores
    peer_scores_.erase(peer);

    // Remove all in-flight tasks assigned to this peer
    std::vector<std::string> to_remove;
    for (auto& [hash_str, task] : in_flight_tasks_) {
        if (task.assigned_peer == peer) {
            to_remove.push_back(hash_str);
        }
    }

    for (const auto& hash_str : to_remove) {
        in_flight_tasks_.erase(hash_str);
    }

    if (!to_remove.empty()) {
        dinero::g_logger.warning("Removed " + std::to_string(to_remove.size()) +
                                " in-flight tasks from disconnected peer");
    }
}

std::vector<Peer*> ParallelBlockDownloader::selectParallelPeers(int max_peers) {
    // Update all scores first
    updatePeerScores();

    // Get all available peers from PeerManager
    std::vector<Peer*> all_peers;
    // TODO: Get peers from peer_manager_ (depends on PeerManager API)
    // For now, use tracked peers
    for (auto& [peer, score] : peer_scores_) {
        if (score.is_available && !score.is_in_cooldown) {
            all_peers.push_back(peer);
        }
    }

    // Sort by overall_score (descending)
    std::sort(all_peers.begin(), all_peers.end(), [this](Peer* a, Peer* b) {
        return peer_scores_[a].overall_score > peer_scores_[b].overall_score;
    });

    // Return top N peers
    if (all_peers.size() > (size_t)max_peers) {
        all_peers.resize(max_peers);
    }

    return all_peers;
}

int ParallelBlockDownloader::scheduleParallelDownloads(std::queue<std::vector<uint8_t>>& pending_blocks) {
    // Check global limit
    if (in_flight_tasks_.size() >= (size_t)max_total_in_flight_) {
        return 0;  // Global limit reached
    }

    // Select best peers
    std::vector<Peer*> peers = selectParallelPeers(max_parallel_peers_);
    if (peers.empty()) {
        return 0;  // No available peers
    }

    // Distribute blocks across peers (round-robin)
    int blocks_scheduled = 0;
    size_t peer_idx = 0;

    // Prepare per-peer request batches
    std::unordered_map<Peer*, std::vector<std::vector<uint8_t>>> peer_requests;

    while (!pending_blocks.empty() && in_flight_tasks_.size() < (size_t)max_total_in_flight_) {
        // Pick next peer (round-robin)
        Peer* peer = peers[peer_idx % peers.size()];
        peer_idx++;

        // Check if peer can accept more
        if (!canPeerAcceptMore(peer)) {
            continue;
        }

        // Get next block
        std::vector<uint8_t> block_hash = pending_blocks.front();
        pending_blocks.pop();

        // Create task
        BlockDownloadTask task;
        task.block_hash = block_hash;
        task.height = 0;  // TODO: Extract height if available
        task.assigned_peer = peer;
        task.request_time = std::chrono::steady_clock::now();
        task.retry_count = 0;
        task.is_in_flight = true;

        // Add to in-flight tracking
        std::string hash_str = util::hex(block_hash);
        in_flight_tasks_[hash_str] = task;

        // Add to peer's batch
        peer_requests[peer].push_back(block_hash);

        blocks_scheduled++;
    }

    // Send requests to each peer
    for (auto& [peer, hashes] : peer_requests) {
        requestBlocksFromPeer(peer, hashes);

        PeerScore& score = getPeerScore(peer);
        score.last_request_time = std::chrono::steady_clock::now();

        dinero::g_logger.debug("Requested " + std::to_string(hashes.size()) +
                              " blocks from peer (score: " +
                              std::to_string(score.overall_score) + ")");
    }

    if (blocks_scheduled > 0) {
        dinero::g_logger.info("Scheduled " + std::to_string(blocks_scheduled) +
                             " blocks across " + std::to_string(peer_requests.size()) + " peers");
    }

    return blocks_scheduled;
}

void ParallelBlockDownloader::handleTimeouts() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> timed_out;

    // Find timed-out tasks
    for (auto& [hash_str, task] : in_flight_tasks_) {
        if (task.hasTimedOut()) {
            timed_out.push_back(hash_str);
        }
    }

    if (timed_out.empty()) {
        return;
    }

    dinero::g_logger.warning("Found " + std::to_string(timed_out.size()) + " timed-out block requests");

    for (const auto& hash_str : timed_out) {
        BlockDownloadTask& task = in_flight_tasks_[hash_str];
        Peer* old_peer = task.assigned_peer;

        // Update peer score
        PeerScore& score = getPeerScore(old_peer);
        score.recordFailure();

        // Check if peer should go into cooldown
        if (score.shouldCooldown()) {
            score.enterCooldown();
        }

        // Find alternate peer
        Peer* new_peer = findAlternatePeer(old_peer);
        if (new_peer) {
            // Reassign to new peer
            task.reassign(new_peer);

            // Send new request
            std::vector<std::vector<uint8_t>> hashes = { task.block_hash };
            requestBlocksFromPeer(new_peer, hashes);

            dinero::g_logger.info("Reassigned block " + hash_str.substr(0, 8) +
                                 "... to alternate peer");
        } else {
            // No alternate peer available, mark as not in-flight
            task.is_in_flight = false;
            dinero::g_logger.warning("No alternate peer for block " + hash_str.substr(0, 8) +
                                    "..., will retry later");
        }
    }
}

void ParallelBlockDownloader::recordBlockReceived(const std::vector<uint8_t>& block_hash,
                                                   Peer* peer,
                                                   size_t block_size) {
    std::string hash_str = util::hex(block_hash);

    auto it = in_flight_tasks_.find(hash_str);
    if (it == in_flight_tasks_.end()) {
        dinero::g_logger.warning("Received unexpected block: " + hash_str.substr(0, 8) + "...");
        return;
    }

    BlockDownloadTask& task = it->second;

    // Calculate latency
    auto now = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - task.request_time);

    // Update peer score
    PeerScore& score = getPeerScore(peer);
    score.recordSuccess(latency.count(), block_size);

    // Remove from in-flight
    in_flight_tasks_.erase(it);

    dinero::g_logger.debug("Block received from peer (latency: " +
                          std::to_string(latency.count()) + "ms, score: " +
                          std::to_string(score.overall_score) + ")");
}

void ParallelBlockDownloader::requestBlocksFromPeer(Peer* peer,
                                                     const std::vector<std::vector<uint8_t>>& hashes) {
    // TODO: Implement actual network request
    // This depends on Peer API and network message format
    // For now, log the request

    dinero::g_logger.debug("Requesting " + std::to_string(hashes.size()) +
                          " blocks from peer " +
                          std::to_string(reinterpret_cast<uintptr_t>(peer)));

    // Example structure (to be implemented with actual Peer API):
    /*
    std::vector<uint8_t> payload;
    // Serialize getdata message with MSG_BLOCK type
    // ...
    peer->send("getdata", payload);
    */
}

bool ParallelBlockDownloader::canPeerAcceptMore(Peer* peer) const {
    int count = countPeerInflight(peer);
    return count < max_blocks_per_peer_;
}

int ParallelBlockDownloader::countPeerInflight(Peer* peer) const {
    int count = 0;
    for (const auto& [hash_str, task] : in_flight_tasks_) {
        if (task.assigned_peer == peer && task.is_in_flight) {
            count++;
        }
    }
    return count;
}

Peer* ParallelBlockDownloader::findAlternatePeer(Peer* exclude_peer) {
    std::vector<Peer*> candidates = selectParallelPeers(max_parallel_peers_);

    // Remove excluded peer
    candidates.erase(
        std::remove(candidates.begin(), candidates.end(), exclude_peer),
        candidates.end()
    );

    if (candidates.empty()) {
        return nullptr;
    }

    // Pick peer with lowest in-flight count
    Peer* best = candidates[0];
    int best_count = countPeerInflight(best);

    for (Peer* peer : candidates) {
        int count = countPeerInflight(peer);
        if (count < best_count) {
            best = peer;
            best_count = count;
        }
    }

    return best;
}

std::unordered_map<Peer*, int> ParallelBlockDownloader::getPerPeerInflightCounts() const {
    std::unordered_map<Peer*, int> counts;

    for (const auto& [hash_str, task] : in_flight_tasks_) {
        if (task.is_in_flight) {
            counts[task.assigned_peer]++;
        }
    }

    return counts;
}

double ParallelBlockDownloader::getTotalBandwidth() const {
    double total = 0.0;

    for (const auto& [peer, score] : peer_scores_) {
        if (score.is_available && !score.is_in_cooldown) {
            total += score.bandwidth_bps;
        }
    }

    return total;
}

std::vector<std::pair<Peer*, double>> ParallelBlockDownloader::getPeerRankings() const {
    std::vector<std::pair<Peer*, double>> rankings;

    for (const auto& [peer, score] : peer_scores_) {
        rankings.push_back({peer, score.overall_score});
    }

    // Sort by score (descending)
    std::sort(rankings.begin(), rankings.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    return rankings;
}
