#include "daemon/services/block_ingress_service.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/validation_queue.h"
#include "dinero/daemon/block_acceptor.h"  // BlockAcceptor static methods
#include "common/ilogger.h"
#include <iostream>
#include <sstream>

namespace dinero {

bool BlockIngressService::Init(DaemonContext& ctx) {
    // Store logger dependency
    ctx_ = &ctx;
    logger_ = ctx.logger_interface;

    if (!logger_) {
        std::cerr << "[BlockIngressService] Logger interface dependency missing" << std::endl;
        return false;
    }

    logger_->info("[BlockIngressService] Initialized successfully");
    return true;
}

bool BlockIngressService::Start() {
    if (started_) {
        if (logger_) {
            logger_->warning("[BlockIngressService] Already started");
        }
        return false;
    }

    if (logger_) {
        logger_->info("[BlockIngressService] Started successfully");
    }

    started_ = true;
    return true;
}

void BlockIngressService::Stop() {
    if (!started_) {
        return;
    }

    if (logger_) {
        logger_->info("[BlockIngressService] Shutdown complete");
    }

    started_ = false;
}

bool BlockIngressService::IsHealthy() const {
    return started_;
}

std::string BlockIngressService::GetMetrics() const {
    std::ostringstream oss;
    const bool queue_ready = ctx_ && ctx_->validation_queue && ctx_->validation_queue->isRunning();
    oss << "{"
        << R"("service":"block_ingress",)"
        << R"("started":)" << (started_ ? "true" : "false") << ","
        << R"("p2p_uses_validation_queue":)" << (queue_ready ? "true" : "false")
        << "}";
    return oss.str();
}

// ========================================================================
// IBlockIngress INTERFACE IMPLEMENTATION
// ========================================================================

BlockAcceptResult BlockIngressService::Submit(const Block& block, BlockOrigin origin) {
    // Convert BlockOrigin to peer_id string for logging
    const char* source = BlockOriginToString(origin);

    if (logger_) {
        logger_->info("[BlockIngressService] Submitting block from " + std::string(source));
    }

    // Delegate to BlockAcceptor based on origin
    switch (origin) {
        case BlockOrigin::RPC:
            // RPC blocks come as hex, but here we have a Block struct
            // Serialize to hex and use AcceptBlockFromRPC
            return BlockAcceptor::AcceptBlockFromRPC(block.Serialize(), source);

        case BlockOrigin::MINER:
            // Miner blocks are local, use RPC path with miner source
            return BlockAcceptor::AcceptBlockFromRPC(block.Serialize(), "miner");

        case BlockOrigin::P2P:
            // Network blocks flow through ValidationQueue when available so the
            // queue owns scheduling but final acceptance still happens through
            // the canonical BlockAcceptor path.
            return SubmitViaValidationQueue(block);

        case BlockOrigin::INTERNAL:
            // Internal blocks (e.g., genesis) use RPC path
            return BlockAcceptor::AcceptBlockFromRPC(block.Serialize(), "internal");

        default:
            return BlockAcceptor::AcceptBlockFromRPC(block.Serialize(), source);
    }
}

BlockAcceptResult BlockIngressService::SubmitHex(const std::string& hex_block, BlockOrigin origin) {
    const char* source = BlockOriginToString(origin);

    if (logger_) {
        logger_->info("[BlockIngressService] Submitting hex block from " + std::string(source));
    }

    // Hex blocks are typically from RPC
    return BlockAcceptor::AcceptBlockFromRPC(hex_block, source);
}

BlockAcceptResult BlockIngressService::SubmitViaValidationQueue(const Block& block) {
    if (!ctx_ || !ctx_->validation_queue || !ctx_->validation_queue->isRunning()) {
        return BlockAcceptor::AcceptBlockFromPeer(block, "p2p");
    }

    auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_->chainstate);
    auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        return BlockAcceptResult::Rejected(
            BlockRejectCode::CONNECT_FAILED,
            "ChainDB not available for validation queue ingress",
            block.GetHash()
        );
    }

    uint64_t height = 0;
    const uint256& prev_hash = block.header.prev_block_hash;
    if (!prev_hash.IsNull()) {
        auto parent_height = chain_db->getBlockHeight(prev_hash);
        if (!parent_height.ok()) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::MISSING_PARENT,
                "Parent block not available for queued validation",
                block.GetHash()
            );
        }
        height = static_cast<uint64_t>(parent_height.value()) + 1;
    }

    return ctx_->validation_queue->submitAndWait(block, height, prev_hash);
}

} // namespace dinero
