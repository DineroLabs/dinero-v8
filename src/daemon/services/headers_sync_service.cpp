#include "daemon/services/headers_sync_service.h"
#include "daemon/daemon_context.h"
#include <iostream>

namespace dinero {
namespace daemon {

HeadersSyncService::HeadersSyncService()
    : manager_(std::make_unique<p2p::HeadersFirstSync>()) {
}

HeadersSyncService::~HeadersSyncService() = default;

bool HeadersSyncService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;

    std::cout << "[HeadersSyncService] Initialized" << std::endl;
    return true;
}

bool HeadersSyncService::Start() {
    std::cout << "[HeadersSyncService] Started (headers-first sync ready)" << std::endl;
    return true;
}

void HeadersSyncService::Stop() {
    if (manager_) {
        manager_->stopSync();
    }

    std::cout << "[HeadersSyncService] Stopped" << std::endl;
}

void HeadersSyncService::startSync(const std::string& peer_id) {
    if (!manager_) return;
    manager_->startSync(peer_id);
}

void HeadersSyncService::stopSync() {
    if (!manager_) return;
    manager_->stopSync();
}

bool HeadersSyncService::isSyncing() const {
    if (!manager_) return false;
    return manager_->isSyncing();
}

p2p::SyncState HeadersSyncService::getCurrentState() const {
    if (!manager_) return p2p::SyncState::IDLE;
    return manager_->getCurrentState();
}

bool HeadersSyncService::processHeaders(const std::string& peer_id, const p2p::HeadersResponse& response) {
    if (!manager_) return false;
    return manager_->processHeaders(peer_id, response);
}

void HeadersSyncService::requestNextHeaders(const std::string& peer_id) {
    if (!manager_) return;
    manager_->requestNextHeaders(peer_id);
}

void HeadersSyncService::requestBlocks(const std::vector<std::string>& block_hashes) {
    if (!manager_) return;
    manager_->requestBlocks(block_hashes);
}

bool HeadersSyncService::processBlock(const std::string& block_hash, const std::string& block_data) {
    if (!manager_) return false;
    return manager_->processBlock(block_hash, block_data);
}

uint32_t HeadersSyncService::getBestHeight() const {
    if (!manager_) return 0;
    return manager_->getBestHeight();
}

std::string HeadersSyncService::getBestBlockHash() const {
    if (!manager_) return "";
    return manager_->getBestBlockHash();
}

uint32_t HeadersSyncService::getHeadersCount() const {
    if (!manager_) return 0;
    return manager_->getHeadersCount();
}

uint32_t HeadersSyncService::getBlocksDownloaded() const {
    if (!manager_) return 0;
    return manager_->getBlocksDownloaded();
}

void HeadersSyncService::setMaxHeadersPerRequest(uint32_t max_headers) {
    if (!manager_) return;
    manager_->setMaxHeadersPerRequest(max_headers);
}

void HeadersSyncService::setValidationEnabled(bool enabled) {
    if (!manager_) return;
    manager_->setValidationEnabled(enabled);
}

void HeadersSyncService::setTimeout(std::chrono::seconds timeout) {
    if (!manager_) return;
    manager_->setTimeout(timeout);
}

din::Json HeadersSyncService::getStatus() const {
    if (!manager_) return din::obj();
    return manager_->getStatus();
}

din::Json HeadersSyncService::getMetrics() const {
    if (!manager_) return din::obj();
    return manager_->getMetrics();
}

} // namespace daemon
} // namespace dinero
