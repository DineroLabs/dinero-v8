#pragma once

#include "daemon/iservice.h"
#include "p2p/peer_scoring.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace dinero {
namespace daemon {

/**
 * Architecture V3: Service wrapper for P2P peer scoring and DoS protection
 *
 * Provides centralized access to peer reputation scoring, misbehavior tracking,
 * and DoS protection mechanisms. Replaces global g_peer_scoring variable.
 *
 * Key Features:
 * - Peer misbehavior tracking and scoring
 * - Automatic ban management based on score thresholds
 * - Score decay over time
 * - Ban list persistence
 * - Rate limiting and DoS protection utilities
 *
 * Access Pattern:
 *   ctx->peer_scoring->addMisbehavior(peer_id, MisbehaviorType::INVALID_BLOCK);
 *   if (ctx->peer_scoring->isBanned(peer_id)) { ... }
 */
class PeerScoringService : public IService {
public:
    PeerScoringService();
    ~PeerScoringService() override;

    // IService interface
    std::string Name() const override { return "PeerScoringService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    /**
     * Add misbehavior score for a peer
     * Thread-safe: Can be called from any thread
     */
    void addMisbehavior(const std::string& peer_id, p2p::MisbehaviorType type);

    /**
     * Get current score for a peer
     */
    int32_t getScore(const std::string& peer_id) const;

    /**
     * Check if peer is currently banned
     */
    bool isBanned(const std::string& peer_id) const;

    /**
     * Manually ban a peer with specified duration
     */
    void banPeer(const std::string& peer_id, std::chrono::seconds duration);

    /**
     * Unban a peer
     */
    void unbanPeer(const std::string& peer_id);

    /**
     * Get detailed peer score information
     */
    p2p::PeerScore getPeerScore(const std::string& peer_id) const;

    /**
     * Get all currently banned peers
     */
    std::vector<std::string> getBannedPeers() const;

    /**
     * Get scoring statistics
     */
    p2p::PeerScoringManager::ScoringStats getStats() const;

    /**
     * Maintenance operations
     */
    void performMaintenance();
    void clearExpiredBans();
    void clearAllBans();
    void resetScores();

    /**
     * Configuration
     */
    void setBanThreshold(int32_t threshold);
    void setDecayRate(double rate);
    void setDefaultBanDuration(std::chrono::seconds duration);
    void setMaxBanDuration(std::chrono::seconds duration);
    void setHistorySize(size_t max_history);

    /**
     * Persistence
     */
    bool saveBanList(const std::string& filename) const;
    bool loadBanList(const std::string& filename);

    /**
     * Direct access to manager (for advanced use cases)
     */
    p2p::PeerScoringManager* getManager() { return manager_.get(); }
    const p2p::PeerScoringManager* getManager() const { return manager_.get(); }

private:
    DaemonContext* ctx_{nullptr};
    std::unique_ptr<p2p::PeerScoringManager> manager_;
    std::string ban_list_file_;
};

} // namespace daemon
} // namespace dinero
