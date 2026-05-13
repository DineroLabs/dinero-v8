#include "daemon/peer_scoring.h"
#include "common/logger.h"
#include <algorithm>
#include <chrono>
#include <numeric>

namespace dinero {

PeerScoring::PeerScoring() {
    g_logger.info("PeerScoring initialized");
}

PeerScoring::~PeerScoring() {
    g_logger.info("PeerScoring destroyed");
}

void PeerScoring::addPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_peer_scores.find(peer_id) == m_peer_scores.end()) {
        PeerScore score;
        score.peer_id = peer_id;
        m_peer_scores[peer_id] = score;
        
        g_logger.debug("Added peer to scoring: " + peer_id);
    }
}

void PeerScoring::removePeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        m_peer_scores.erase(it);
        g_logger.debug("Removed peer from scoring: " + peer_id);
    }
}

bool PeerScoring::hasPeer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peer_scores.find(peer_id) != m_peer_scores.end();
}

void PeerScoring::recordEvent(const std::string& peer_id, PeerEvent event) {
    recordEvent(peer_id, event, getEventScore(event));
}

void PeerScoring::recordEvent(const std::string& peer_id, PeerEvent event, int32_t score_delta) {
    if (!m_enabled) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it == m_peer_scores.end()) {
        // Add peer if not exists
        PeerScore score;
        score.peer_id = peer_id;
        m_peer_scores[peer_id] = score;
        it = m_peer_scores.find(peer_id);
    }
    
    PeerScore& peer_score = it->second;
    peer_score.last_activity = std::chrono::steady_clock::now();
    
    // Update counters based on event type
    switch (event) {
        case PeerEvent::CONNECTION_ATTEMPT:
            peer_score.connection_attempts++;
            break;
        case PeerEvent::CONNECTION_SUCCESS:
            peer_score.connection_successes++;
            break;
        case PeerEvent::MESSAGE_RECEIVED:
            peer_score.messages_received++;
            break;
        case PeerEvent::MESSAGE_SENT:
            peer_score.messages_sent++;
            break;
        case PeerEvent::INVALID_MESSAGE:
            peer_score.invalid_messages++;
            break;
        case PeerEvent::BLOCK_RECEIVED:
            peer_score.blocks_received++;
            break;
        case PeerEvent::BLOCK_VALID:
            peer_score.valid_blocks++;
            break;
        case PeerEvent::BLOCK_INVALID:
            peer_score.invalid_blocks++;
            break;
        case PeerEvent::TRANSACTION_RECEIVED:
            peer_score.transactions_received++;
            break;
        case PeerEvent::TRANSACTION_VALID:
            peer_score.valid_transactions++;
            break;
        case PeerEvent::TRANSACTION_INVALID:
            peer_score.invalid_transactions++;
            break;
        case PeerEvent::TIMEOUT:
            peer_score.timeouts++;
            break;
        case PeerEvent::BAN_TRIGGER:
            // Handle ban trigger
            break;
        default:
            break;
    }
    
    // Update score
    updateScore(peer_score, score_delta);
    
    // Check if peer should be banned
    if (shouldBan(peer_score)) {
        applyBan(peer_score, m_default_ban_duration.load());
        g_logger.warning("Banned peer due to low score: " + peer_id + " (score: " + std::to_string(peer_score.score) + ")");
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
        m_stats.total_events++;
        if (event == PeerEvent::BAN_TRIGGER) {
            m_stats.ban_events++;
        }
    }
    
    g_logger.debug("Recorded event for peer " + peer_id + ": " + std::to_string(static_cast<int>(event)) + 
                   " (score delta: " + std::to_string(score_delta) + ")");
}

int32_t PeerScoring::getScore(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        return it->second.score;
    }
    
    return 0;
}

PeerScore PeerScoring::getPeerScore(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        return it->second;
    }
    
    return PeerScore();
}

std::vector<std::string> PeerScoring::getTopPeers(size_t count) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::pair<std::string, int32_t>> peer_scores;
    for (const auto& pair : m_peer_scores) {
        if (!pair.second.banned) {
            peer_scores.emplace_back(pair.first, pair.second.score);
        }
    }
    
    // Sort by score (descending)
    std::sort(peer_scores.begin(), peer_scores.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
    
    std::vector<std::string> top_peers;
    size_t selected = std::min(count, peer_scores.size());
    for (size_t i = 0; i < selected; ++i) {
        top_peers.push_back(peer_scores[i].first);
    }
    
    return top_peers;
}

std::vector<std::string> PeerScoring::getBannedPeers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::string> banned_peers;
    for (const auto& pair : m_peer_scores) {
        if (pair.second.banned) {
            banned_peers.push_back(pair.first);
        }
    }
    
    return banned_peers;
}

bool PeerScoring::isBanned(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        return it->second.banned && std::chrono::steady_clock::now() < it->second.ban_until;
    }
    
    return false;
}

void PeerScoring::banPeer(const std::string& peer_id, std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        applyBan(it->second, duration);
        g_logger.info("Banned peer: " + peer_id + " for " + std::to_string(duration.count()) + " seconds");
    }
}

void PeerScoring::unbanPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_peer_scores.find(peer_id);
    if (it != m_peer_scores.end()) {
        removeBan(it->second);
        g_logger.info("Unbanned peer: " + peer_id);
    }
}

void PeerScoring::unbanAllPeers() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (auto& pair : m_peer_scores) {
        if (pair.second.banned) {
            removeBan(pair.second);
        }
    }
    
    g_logger.info("Unbanned all peers");
}

void PeerScoring::cleanup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto now = std::chrono::steady_clock::now();
    size_t removed = 0;
    
    // Remove peers that have been inactive for too long
    for (auto it = m_peer_scores.begin(); it != m_peer_scores.end();) {
        const PeerScore& score = it->second;
        
        // Remove if inactive for more than 24 hours
        if (now - score.last_activity > std::chrono::hours(24)) {
            it = m_peer_scores.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    
    if (removed > 0) {
        g_logger.info("Cleaned up " + std::to_string(removed) + " inactive peers");
    }
}

void PeerScoring::updateScores() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
        m_stats.total_peers = m_peer_scores.size();
        m_stats.banned_peers = 0;
        m_stats.active_peers = 0;
        
        int32_t total_score = 0;
        for (const auto& pair : m_peer_scores) {
            if (pair.second.banned) {
                m_stats.banned_peers++;
            } else {
                m_stats.active_peers++;
                total_score += pair.second.score;
            }
        }
        
        if (m_stats.active_peers > 0) {
            m_stats.avg_score = total_score / m_stats.active_peers;
        } else {
            m_stats.avg_score = 0;
        }
    }
}

PeerScoring::ScoringStats PeerScoring::getStats() const {
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    return m_stats;
}

void PeerScoring::updateScore(PeerScore& peer_score, int32_t delta) {
    peer_score.score += delta;
    
    // Clamp score
    if (peer_score.score > MAX_SCORE) {
        peer_score.score = MAX_SCORE;
    } else if (peer_score.score < MIN_SCORE) {
        peer_score.score = MIN_SCORE;
    }
}

bool PeerScoring::shouldBan(const PeerScore& peer_score) const {
    return peer_score.score <= m_ban_threshold.load();
}

void PeerScoring::applyBan(PeerScore& peer_score, std::chrono::seconds duration) {
    peer_score.banned = true;
    peer_score.ban_until = std::chrono::steady_clock::now() + duration;
    peer_score.last_ban = std::chrono::steady_clock::now();
}

void PeerScoring::removeBan(PeerScore& peer_score) {
    peer_score.banned = false;
    peer_score.ban_until = std::chrono::steady_clock::now();
}

int32_t PeerScoring::getEventScore(PeerEvent event) const {
    switch (event) {
        case PeerEvent::CONNECTION_ATTEMPT:
            return -1;
        case PeerEvent::CONNECTION_SUCCESS:
            return 10;
        case PeerEvent::CONNECTION_FAILURE:
            return -5;
        case PeerEvent::MESSAGE_RECEIVED:
            return 1;
        case PeerEvent::MESSAGE_SENT:
            return 0;
        case PeerEvent::INVALID_MESSAGE:
            return -20;
        case PeerEvent::BLOCK_RECEIVED:
            return 5;
        case PeerEvent::BLOCK_VALID:
            return 15;
        case PeerEvent::BLOCK_INVALID:
            return -30;
        case PeerEvent::TRANSACTION_RECEIVED:
            return 2;
        case PeerEvent::TRANSACTION_VALID:
            return 5;
        case PeerEvent::TRANSACTION_INVALID:
            return -10;
        case PeerEvent::PING_RECEIVED:
            return 1;
        case PeerEvent::PONG_RECEIVED:
            return 1;
        case PeerEvent::TIMEOUT:
            return -10;
        case PeerEvent::BAN_TRIGGER:
            return -100;
        default:
            return 0;
    }
}

} // namespace dinero
