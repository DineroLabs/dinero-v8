#include "daemon/services/compact_proof_block_service.h"
#include "daemon/services/compact_block_service.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"

namespace dinero {
namespace daemon {

CompactProofBlockService::CompactProofBlockService()
    : manager_(std::make_unique<p2p::CompactProofBlockManager>()) {
}

CompactProofBlockService::~CompactProofBlockService() = default;

bool CompactProofBlockService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;

    g_logger.info("Phase 34.7: CompactProofBlockService initializing...");

    // Wire to CompactBlockService if available
    if (ctx.compact_blocks) {
        auto* compact_block_service = dynamic_cast<CompactBlockService*>(ctx.compact_blocks.get());
        if (compact_block_service && compact_block_service->getManager()) {
            manager_->setCompactBlockManager(compact_block_service->getManager());
            g_logger.info("Phase 34.7: Linked to CompactBlockManager");
        }
    }

    // Configure based on network (stateless mode for fast sync nodes)
    // For now, disable stateless mode by default (requires full Utreexo integration)
    manager_->setStatelessMode(false);
    manager_->setProofCaching(true);
    manager_->setMaxProofCacheSize(100);

    g_logger.info("Phase 34.7: CompactProofBlockService initialized");
    g_logger.info("Phase 34.7: Handlers ready: blocktxnproofs (compact block + Utreexo proofs)");

    return true;
}

bool CompactProofBlockService::Start() {
    g_logger.info("Phase 34.7: CompactProofBlockService started");
    g_logger.info("Phase 34.7: Compact-proof blocks enabled (BIP152 + Utreexo)");
    return true;
}

void CompactProofBlockService::Stop() {
    g_logger.info("Phase 34.7: CompactProofBlockService stopped");

    // Log final stats
    if (manager_) {
        g_logger.info("Phase 34.7: Final stats:\n" + manager_->getStatsString());
    }
}

void CompactProofBlockService::setStatelessMode(bool enabled) {
    if (manager_) {
        manager_->setStatelessMode(enabled);
        g_logger.info("Phase 34.7: Stateless mode " + std::string(enabled ? "enabled" : "disabled"));
    }
}

bool CompactProofBlockService::isStatelessMode() const {
    return manager_ ? manager_->isStatelessMode() : false;
}

} // namespace daemon
} // namespace dinero
