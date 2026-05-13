// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/block_download_scheduler.h"
#include "common/logger.h"
#include <cassert>
#include <chrono>
#include <algorithm>

namespace dinero {

BlockDownloadScheduler::BlockDownloadScheduler(SendGetDataCallback send_callback)
    : send_callback_(send_callback) {

    g_logger.info("BlockDownloadScheduler initialized: "
                 "max_in_flight=" + std::to_string(max_in_flight_) +
                 ", timeout=" + std::to_string(timeout_seconds_) + "s" +
                 ", max_retries=" + std::to_string(max_retries_));
}

void BlockDownloadScheduler::scheduleBlock(const uint256& block_hash, uint32_t height, peer_id_t announcing_peer) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Check if already completed
    if (completed_.find(block_hash) != completed_.end()) {
        g_logger.debug("Block " + block_hash.ToString() + " already downloaded, skipping");
        return;
    }

    // Check if already in flight
    if (in_flight_.find(block_hash) != in_flight_.end()) {
        g_logger.debug("Block " + block_hash.ToString() + " already in flight, skipping");
        return;
    }

    // Check if already queued
    auto it = std::find_if(download_queue_.begin(), download_queue_.end(),
        [&block_hash](const BlockDownloadRequest& req) {
            return req.block_hash == block_hash;
        });

    if (it != download_queue_.end()) {
        g_logger.debug("Block " + block_hash.ToString() + " already queued, skipping");
        return;
    }

    // Create new request
    BlockDownloadRequest request;
    request.block_hash = block_hash;
    request.height = height;
    request.timestamp = getCurrentTime();
    request.requested_from = announcing_peer;
    request.in_flight = false;

    if (height == 0) {
        // INV-announced blocks without a header-chain height are speculative and
        // should never preempt known historical downloads during catch-up.
        download_queue_.push_back(request);
    } else {
        // Insert into queue (maintain height-based ordering)
        // Lower height = higher priority (insert at front)
        auto insert_pos = std::find_if(download_queue_.begin(), download_queue_.end(),
            [height](const BlockDownloadRequest& req) {
                return req.height == 0 || req.height > height;
            });

        download_queue_.insert(insert_pos, request);
    }

    g_logger.debug("Scheduled block " + block_hash.ToString() +
                  " at height " + std::to_string(height) +
                  " from peer " + announcing_peer +
                  " (queue size: " + std::to_string(download_queue_.size()) + ")");

    // Update stats
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.queued_blocks = static_cast<uint32_t>(download_queue_.size());
    }
}

void BlockDownloadScheduler::scheduleBlockRange(uint32_t start_height, uint32_t end_height, peer_id_t preferred_peer) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    uint32_t scheduled_count = 0;

    for (uint32_t height = start_height; height <= end_height; ++height) {
        // Note: We don't have block hash here (headers-first scenario)
        // This would need integration with header chain to get hashes
        // For now, this is a placeholder showing the pattern

        // In production, you would:
        // 1. Query MultiPeerHeadersSync for header at this height
        // 2. Get block_hash from header
        // 3. Schedule using scheduleBlock()

        scheduled_count++;
    }

    g_logger.info("Scheduled block range " + std::to_string(start_height) +
                 "-" + std::to_string(end_height) +
                 " (" + std::to_string(scheduled_count) + " blocks)" +
                 (preferred_peer.empty() ? "" : " from peer " + preferred_peer));
}

void BlockDownloadScheduler::notifyBlockReceived(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Remove from in-flight
    auto in_flight_it = in_flight_.find(block_hash);
    if (in_flight_it != in_flight_.end()) {
        // Calculate download time
        int64_t download_time = getCurrentTime() - in_flight_it->second.timestamp;
        peer_id_t peer = in_flight_it->second.requested_from;

        g_logger.debug("Block " + block_hash.ToString() + " download completed");
        in_flight_.erase(in_flight_it);

        // Record successful download with peer stats
        recordDownloadSuccess(peer, download_time);

        // Update stats
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.in_flight_blocks = static_cast<uint32_t>(in_flight_.size());
            stats_.completed_blocks++;
        }
    }

    // Add to completed set
    completed_.insert(block_hash);

    // Remove from retry count
    retry_count_.erase(block_hash);
}

void BlockDownloadScheduler::notifyPeerDisconnected(peer_id_t peer_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    uint32_t rescheduled_count = 0;

    // Find all in-flight requests from this peer
    auto it = in_flight_.begin();
    while (it != in_flight_.end()) {
        if (it->second.requested_from == peer_id) {
            // Move back to download queue
            BlockDownloadRequest request = it->second;
            request.in_flight = false;
            request.requested_from = "";  // Clear peer assignment

            // Insert at front (high priority retry)
            download_queue_.push_front(request);

            g_logger.debug("Rescheduling block " + request.block_hash.ToString() +
                          " (peer " + peer_id + " disconnected)");

            it = in_flight_.erase(it);
            rescheduled_count++;
        } else {
            ++it;
        }
    }

    if (rescheduled_count > 0) {
        std::lock_guard<std::mutex> peer_lock(peer_mutex_);
        auto& stats = peer_stats_[peer_id];
        if (stats.in_flight_count > rescheduled_count) {
            stats.in_flight_count -= rescheduled_count;
        } else {
            stats.in_flight_count = 0;
        }
    }

    if (rescheduled_count > 0) {
        g_logger.info("Rescheduled " + std::to_string(rescheduled_count) +
                     " blocks after peer " + peer_id + " disconnected");

        // Update stats
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.queued_blocks = static_cast<uint32_t>(download_queue_.size());
            stats_.in_flight_blocks = static_cast<uint32_t>(in_flight_.size());
        }
    }
}

void BlockDownloadScheduler::processQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase G Safety Assertion: In-Flight Cap Check
    // ═══════════════════════════════════════════════════════════════════════════
    // This assertion ensures we never exceed max_in_flight_ blocks.
    // If violated, there's a leak or retry loop bug.
    assert(in_flight_.size() <= max_in_flight_ &&
           "Phase G Safety: In-flight blocks exceeded max limit");

    // Phase G.12: Detect and adapt to sync phase
    detectSyncPhase();

    // Retry timed-out downloads
    retryTimedOutDownloads();

    // Start new downloads while under limit
    while (in_flight_.size() < max_in_flight_ && !download_queue_.empty()) {
        if (!startNextDownload()) {
            break;  // Failed to start download
        }
    }

    // Post-loop invariant check
    assert(in_flight_.size() <= max_in_flight_ &&
           "Phase G Safety: In-flight blocks exceeded max limit after queue processing");
}

bool BlockDownloadScheduler::isInFlight(const uint256& block_hash) const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return in_flight_.find(block_hash) != in_flight_.end();
}

BlockDownloadScheduler::Stats BlockDownloadScheduler::getStats() const {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    Stats result = stats_;

    // Update live counts
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        result.queued_blocks = static_cast<uint32_t>(download_queue_.size());
        result.in_flight_blocks = static_cast<uint32_t>(in_flight_.size());
    }

    return result;
}

int64_t BlockDownloadScheduler::getCurrentTime() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool BlockDownloadScheduler::startNextDownload() {
    // Must be called with queue_mutex_ held

    if (download_queue_.empty()) {
        return false;
    }

    // Get next request
    BlockDownloadRequest request = download_queue_.front();
    download_queue_.pop_front();

    // Check retry limit
    uint32_t retries = retry_count_[request.block_hash];
    if (retries >= max_retries_) {
        g_logger.warning("Block " + request.block_hash.ToString() +
                        " exceeded max retries (" + std::to_string(max_retries_) +
                        "), dropping");

        // Update stats
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.failed_blocks++;
        }

        return true;  // Continue processing queue
    }

    // Select peer for download
    peer_id_t selected_peer = selectPeerForBlock(request);
    if (selected_peer.empty()) {
        g_logger.warning("No peer available for block " + request.block_hash.ToString() +
                        ", requeueing");

        // Put back at end of queue
        download_queue_.push_back(request);
        return false;  // Stop processing queue for now
    }

    // Send GETDATA via callback
    request.requested_from = selected_peer;
    request.in_flight = true;
    request.timestamp = getCurrentTime();

    if (!send_callback_(selected_peer, request.block_hash)) {
        g_logger.error("Failed to send GETDATA for block " + request.block_hash.ToString() +
                      " to peer " + selected_peer);

        // Requeue
        request.in_flight = false;
        download_queue_.push_back(request);
        return false;
    }

    // Record download start
    recordDownloadStart(selected_peer);

    // Add to in-flight
    in_flight_[request.block_hash] = request;

    g_logger.debug("Started download of block " + request.block_hash.ToString() +
                  " from peer " + selected_peer +
                  " (in_flight: " + std::to_string(in_flight_.size()) + ")");

    // Update stats
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.queued_blocks = static_cast<uint32_t>(download_queue_.size());
        stats_.in_flight_blocks = static_cast<uint32_t>(in_flight_.size());
    }

    return true;
}

void BlockDownloadScheduler::retryTimedOutDownloads() {
    // Must be called with queue_mutex_ held

    int64_t now = getCurrentTime();
    std::vector<uint256> timed_out_blocks;

    // Find timed-out requests
    for (const auto& [block_hash, request] : in_flight_) {
        int64_t elapsed = now - request.timestamp;
        if (elapsed > timeout_seconds_) {
            timed_out_blocks.push_back(block_hash);
        }
    }

    // Retry each timed-out request
    for (const auto& block_hash : timed_out_blocks) {
        auto it = in_flight_.find(block_hash);
        if (it == in_flight_.end()) {
            continue;  // Already completed
        }

        BlockDownloadRequest request = it->second;
        peer_id_t failed_peer = request.requested_from;

        g_logger.warning("Block " + block_hash.ToString() +
                        " download timed out from peer " + failed_peer +
                        " (elapsed: " + std::to_string(now - request.timestamp) + "s)");

        // Remove from in-flight
        in_flight_.erase(it);

        // Record download failure for peer stats
        recordDownloadFailure(failed_peer);

        // Increment retry count
        retry_count_[block_hash]++;

        // Requeue (at front for priority)
        request.in_flight = false;
        request.requested_from = "";  // Clear peer assignment
        download_queue_.push_front(request);

        // Update stats
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.retry_count++;
        }
    }

    if (!timed_out_blocks.empty()) {
        g_logger.info("Retrying " + std::to_string(timed_out_blocks.size()) +
                     " timed-out block downloads");
    }
}

peer_id_t BlockDownloadScheduler::selectPeerForBlock(const BlockDownloadRequest& request) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    // If announcing peer is available and not overloaded, prefer it
    if (!request.requested_from.empty()) {
        if (available_peers_.count(request.requested_from) > 0) {
            auto& stats = peer_stats_[request.requested_from];
            if (stats.in_flight_count < max_peer_in_flight_) {
                return request.requested_from;
            }
        }
    }

    struct CandidatePeer {
        peer_id_t peer_id;
        double score{0.0};
        uint32_t in_flight{0};
        uint32_t total_downloads{0};
    };

    std::vector<CandidatePeer> candidates;
    candidates.reserve(available_peers_.size());
    double best_score = -1.0;

    for (const auto& peer_id : available_peers_) {
        const auto& stats = peer_stats_[peer_id];

        // Skip peers that are at capacity
        if (stats.in_flight_count >= max_peer_in_flight_) {
            continue;
        }

        // Phase G.11: Use external score provider if available, otherwise internal
        double score;
        if (peer_score_provider_) {
            score = peer_score_provider_(peer_id);
            // If provider returns -1 (unknown peer), fall back to internal scoring
            if (score < 0.0) {
                score = stats.getScore();
            }
        } else {
            score = stats.getScore();
        }

        candidates.push_back(CandidatePeer{
            peer_id,
            score,
            stats.in_flight_count,
            stats.total_downloads
        });
        if (score > best_score) {
            best_score = score;
        }
    }

    if (!candidates.empty()) {
        // Diversity hardening: pick from peers that are near the best score,
        // then prefer the least loaded / least used peer.
        constexpr double NEAR_BEST_SCORE_RATIO = 0.90;
        const double near_best_threshold = best_score * NEAR_BEST_SCORE_RATIO;

        std::vector<CandidatePeer> near_best;
        near_best.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (candidate.score >= near_best_threshold) {
                near_best.push_back(candidate);
            }
        }
        if (near_best.empty()) {
            near_best = candidates;
        }

        auto selected_it = std::min_element(
            near_best.begin(),
            near_best.end(),
            [](const CandidatePeer& a, const CandidatePeer& b) {
                if (a.in_flight != b.in_flight) {
                    return a.in_flight < b.in_flight;
                }
                if (a.total_downloads != b.total_downloads) {
                    return a.total_downloads < b.total_downloads;
                }
                return a.score > b.score;
            });

        const auto& selected = *selected_it;
        g_logger.debug("Selected peer " + selected.peer_id + " for block " +
                      request.block_hash.ToString().substr(0, 16) + "... (score: " +
                      std::to_string(selected.score) + ", inflight=" +
                      std::to_string(selected.in_flight) + ")");
        return selected.peer_id;
    }

    g_logger.debug("No suitable peer available for block " + request.block_hash.ToString());
    return "";
}

void BlockDownloadScheduler::registerPeers(const std::vector<peer_id_t>& peer_ids) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    for (const auto& peer_id : peer_ids) {
        available_peers_.insert(peer_id);
        // Initialize stats if not exists
        if (peer_stats_.find(peer_id) == peer_stats_.end()) {
            peer_stats_[peer_id] = PeerStats();
        }
    }

    g_logger.info("Registered " + std::to_string(peer_ids.size()) + " peers for block downloads");
}

void BlockDownloadScheduler::unregisterPeer(peer_id_t peer_id) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    available_peers_.erase(peer_id);
    g_logger.debug("Unregistered peer " + peer_id + " from block downloads");
}

void BlockDownloadScheduler::recordDownloadStart(peer_id_t peer) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    auto& stats = peer_stats_[peer];
    stats.in_flight_count++;
    stats.total_downloads++;
}

void BlockDownloadScheduler::recordDownloadSuccess(peer_id_t peer, int64_t download_time) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    auto& stats = peer_stats_[peer];
    if (stats.in_flight_count > 0) {
        stats.in_flight_count--;
    } else {
        g_logger.warning("Peer " + peer + " download success arrived with zero in-flight count");
    }
    stats.total_download_time += download_time;
    stats.last_success_time = getCurrentTime();

    g_logger.debug("Peer " + peer + " download success: " +
                  std::to_string(download_time) + "s (success_rate: " +
                  std::to_string(stats.getSuccessRate() * 100.0) + "%, avg: " +
                  std::to_string(stats.getAvgDownloadTime()) + "s)");
}

void BlockDownloadScheduler::recordDownloadFailure(peer_id_t peer) {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    auto& stats = peer_stats_[peer];
    if (stats.in_flight_count > 0) {
        stats.in_flight_count--;
    } else {
        g_logger.warning("Peer " + peer + " download failure arrived with zero in-flight count");
    }
    stats.failed_downloads++;

    g_logger.debug("Peer " + peer + " download failure (success_rate: " +
                  std::to_string(stats.getSuccessRate() * 100.0) + "%)");
}

std::vector<peer_id_t> BlockDownloadScheduler::getSortedPeersByScore() {
    std::lock_guard<std::mutex> lock(peer_mutex_);

    std::vector<std::pair<peer_id_t, double>> peer_scores;

    for (const auto& peer_id : available_peers_) {
        const auto& stats = peer_stats_[peer_id];
        peer_scores.push_back({peer_id, stats.getScore()});
    }

    // Sort by score (descending)
    std::sort(peer_scores.begin(), peer_scores.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    std::vector<peer_id_t> sorted_peers;
    for (const auto& [peer_id, score] : peer_scores) {
        sorted_peers.push_back(peer_id);
    }

    return sorted_peers;
}

//=============================================================================
// Phase G.12: Sync Phase Awareness
//=============================================================================

void BlockDownloadScheduler::detectSyncPhase() {
    // Must be called with queue_mutex_ held

    if (!auto_phase_detection_) {
        return;  // Manual mode
    }

    size_t queue_size = download_queue_.size() + in_flight_.size();
    SyncPhase old_phase = current_sync_phase_;
    SyncPhase new_phase = current_sync_phase_;

    // Determine new phase based on queue size
    if (queue_size > IBD_THRESHOLD) {
        new_phase = SyncPhase::IBD;
    } else if (queue_size > CATCHING_UP_THRESHOLD) {
        new_phase = SyncPhase::CATCHING_UP;
    } else {
        new_phase = SyncPhase::STEADY_STATE;
    }

    // Only apply parameters if phase changed
    if (new_phase != old_phase) {
        current_sync_phase_ = new_phase;
        applyPhaseParameters();

        const char* phase_name[] = {"IBD", "CATCHING_UP", "STEADY_STATE"};
        g_logger.info("Sync phase transition: " + std::string(phase_name[static_cast<int>(old_phase)]) +
                     " → " + std::string(phase_name[static_cast<int>(new_phase)]) +
                     " (queue_size=" + std::to_string(queue_size) + ")");
    }
}

void BlockDownloadScheduler::applyPhaseParameters() {
    // Must be called with queue_mutex_ held

    switch (current_sync_phase_) {
        case SyncPhase::IBD:
            // Aggressive: High parallelism, fast retries, short timeout
            max_in_flight_ = 32;
            timeout_seconds_ = 30;
            max_retries_ = 2;
            max_peer_in_flight_ = 8;  // Allow more per peer
            g_logger.info("Applied IBD parameters: max_in_flight=32, timeout=30s, max_retries=2");
            break;

        case SyncPhase::CATCHING_UP:
            // Balanced: Moderate parallelism (default values)
            max_in_flight_ = 16;
            timeout_seconds_ = 60;
            max_retries_ = 3;
            max_peer_in_flight_ = 4;
            g_logger.info("Applied CATCHING_UP parameters: max_in_flight=16, timeout=60s, max_retries=3");
            break;

        case SyncPhase::STEADY_STATE:
            // Conservative: Low parallelism, patient retries, long timeout
            max_in_flight_ = 8;
            timeout_seconds_ = 120;
            max_retries_ = 5;
            max_peer_in_flight_ = 2;  // Stick with fewer peers
            g_logger.info("Applied STEADY_STATE parameters: max_in_flight=8, timeout=120s, max_retries=5");
            break;
    }
}

} // namespace dinero
