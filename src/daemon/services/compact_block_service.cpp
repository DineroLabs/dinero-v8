#include "daemon/services/compact_block_service.h"
#include "daemon/daemon_context.h"
#include "daemon/block_relay_manager.h"
#include <iostream>

namespace dinero {
namespace daemon {

CompactBlockService::CompactBlockService() = default;

CompactBlockService::~CompactBlockService() = default;

bool CompactBlockService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    std::cout << "[CompactBlockService] Initialized (BIP152 binary compact blocks)" << std::endl;
    return true;
}

bool CompactBlockService::Start() {
    std::cout << "[CompactBlockService] Started (binary wire format, ~95% bandwidth reduction)" << std::endl;
    return true;
}

void CompactBlockService::Stop() {
    std::cout << "[CompactBlockService] Stopped" << std::endl;
}

uint64_t CompactBlockService::getBlocksProcessed() const {
    if (ctx_ && ctx_->block_relay) {
        auto stats = ctx_->block_relay->GetStats();
        return stats.compact_blocks_received;
    }
    return 0;
}

double CompactBlockService::getReconstructionRate() const {
    if (ctx_ && ctx_->block_relay) {
        return ctx_->block_relay->GetCompactBlockSuccessRate();
    }
    return 0.0;
}

uint64_t CompactBlockService::getBandwidthSaved() const {
    if (ctx_ && ctx_->block_relay) {
        auto stats = ctx_->block_relay->GetStats();
        // Estimate: each compact block saves ~950KB vs full block (95% reduction)
        // This is a rough estimate based on average block size
        return stats.compact_blocks_reconstructed * 950 * 1024;
    }
    return 0;
}

} // namespace daemon
} // namespace dinero
