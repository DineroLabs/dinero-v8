#include "daemon/services/peer_scoring_service.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "common/logger.h"
#include <filesystem>
#include <iostream>

namespace dinero {
namespace daemon {

PeerScoringService::PeerScoringService()
    : manager_(std::make_unique<p2p::PeerScoringManager>()) {
}

PeerScoringService::~PeerScoringService() = default;

bool PeerScoringService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;

    // Set ban list file path
    if (ctx.config) {
        std::filesystem::path data_dir = ctx.config->DataDir();
        ban_list_file_ = (data_dir / "banlist.dat").string();

        // Load existing ban list if available
        if (std::filesystem::exists(ban_list_file_)) {
            if (loadBanList(ban_list_file_)) {
                std::cout << "[PeerScoringService] Loaded ban list from: " << ban_list_file_ << std::endl;
            } else {
                std::cout << "[PeerScoringService] Warning: Failed to load ban list from: " << ban_list_file_ << std::endl;
            }
        }
    }

    std::cout << "[PeerScoringService] Initialized" << std::endl;
    return true;
}

bool PeerScoringService::Start() {
    std::cout << "[PeerScoringService] Started" << std::endl;
    return true;
}

void PeerScoringService::Stop() {
    // Save ban list before shutdown
    if (!ban_list_file_.empty()) {
        if (saveBanList(ban_list_file_)) {
            std::cout << "[PeerScoringService] Saved ban list to: " << ban_list_file_ << std::endl;
        } else {
            std::cout << "[PeerScoringService] Warning: Failed to save ban list to: " << ban_list_file_ << std::endl;
        }
    }

    std::cout << "[PeerScoringService] Stopped" << std::endl;
}

void PeerScoringService::addMisbehavior(const std::string& peer_id, p2p::MisbehaviorType type) {
    if (!manager_) return;

    manager_->addMisbehavior(peer_id, type);

    // Log if peer was banned
    if (isBanned(peer_id)) {
        std::cout << "[PeerScoringService] Warning: Peer " << peer_id << " banned due to misbehavior" << std::endl;
    }
}

int32_t PeerScoringService::getScore(const std::string& peer_id) const {
    if (!manager_) return 0;
    return manager_->getScore(peer_id);
}

bool PeerScoringService::isBanned(const std::string& peer_id) const {
    if (!manager_) return false;
    return manager_->isBanned(peer_id);
}

void PeerScoringService::banPeer(const std::string& peer_id, std::chrono::seconds duration) {
    if (!manager_) return;

    manager_->banPeer(peer_id, duration);
    std::cout << "[PeerScoringService] Manually banned peer: " << peer_id << " for " << duration.count() << " seconds" << std::endl;
}

void PeerScoringService::unbanPeer(const std::string& peer_id) {
    if (!manager_) return;

    manager_->unbanPeer(peer_id);
    std::cout << "[PeerScoringService] Unbanned peer: " << peer_id << std::endl;
}

p2p::PeerScore PeerScoringService::getPeerScore(const std::string& peer_id) const {
    if (!manager_) return p2p::PeerScore{};
    return manager_->getPeerScore(peer_id);
}

std::vector<std::string> PeerScoringService::getBannedPeers() const {
    if (!manager_) return {};
    return manager_->getBannedPeers();
}

p2p::PeerScoringManager::ScoringStats PeerScoringService::getStats() const {
    if (!manager_) return {};
    return manager_->getStats();
}

void PeerScoringService::performMaintenance() {
    if (!manager_) return;
    manager_->performMaintenance();
}

void PeerScoringService::clearExpiredBans() {
    if (!manager_) return;
    manager_->clearExpiredBans();
    std::cout << "[PeerScoringService] Cleared expired bans" << std::endl;
}

void PeerScoringService::clearAllBans() {
    if (!manager_) return;
    manager_->clearAllBans();
    std::cout << "[PeerScoringService] Cleared all bans" << std::endl;
}

void PeerScoringService::resetScores() {
    if (!manager_) return;
    manager_->resetScores();
    std::cout << "[PeerScoringService] Reset all peer scores" << std::endl;
}

void PeerScoringService::setBanThreshold(int32_t threshold) {
    if (!manager_) return;
    manager_->setBanThreshold(threshold);
}

void PeerScoringService::setDecayRate(double rate) {
    if (!manager_) return;
    manager_->setDecayRate(rate);
}

void PeerScoringService::setDefaultBanDuration(std::chrono::seconds duration) {
    if (!manager_) return;
    manager_->setDefaultBanDuration(duration);
}

void PeerScoringService::setMaxBanDuration(std::chrono::seconds duration) {
    if (!manager_) return;
    manager_->setMaxBanDuration(duration);
}

void PeerScoringService::setHistorySize(size_t max_history) {
    if (!manager_) return;
    manager_->setHistorySize(max_history);
}

bool PeerScoringService::saveBanList(const std::string& filename) const {
    if (!manager_) return false;
    return manager_->saveToFile(filename);
}

bool PeerScoringService::loadBanList(const std::string& filename) {
    if (!manager_) return false;
    return manager_->loadFromFile(filename);
}

} // namespace daemon
} // namespace dinero
