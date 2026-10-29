#include "daemon/orchard_network_block.h"
#include "daemon/services/block_ingress_service.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/validation_queue.h"
#include "dinero/daemon/block_acceptor.h"  // BlockAcceptor static methods
#include "common/ilogger.h"
#include "consensus/orchard_profile.h"
#include "util/hex.h"
#include <limits>
#include <iostream>
#include <sstream>

namespace dinero {

namespace {
// This concrete task is constructed only after parent-height/profile capture.
// Its service and exact bytes remain owned until the queue returns a result.
class QueuedOrchardBlock final : public consensus::CanonicalBlockTask {
public:
    QueuedOrchardBlock(std::shared_ptr<ChainstateService> source,
                       std::vector<uint8_t> wire, uint256 hash, uint64_t height)
        : source_(std::move(source)), wire_(std::move(wire)), hash_(hash), height_(height) {}
    const uint256& Hash() const noexcept override { return hash_; }
    uint64_t Height() const noexcept override { return height_; }
    size_t WireBytes() const noexcept override { return wire_.size(); }
    BlockAcceptResult ValidateAndApply() const override {
        auto* context = DaemonContext::instance();
        if (!context || context->chainstate != source_)
            return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED,
                "Queued Orchard selected owner changed", hash_, height_);
        const auto result = source_->TryAcceptOrchardBlockFromRPC(util::hex(wire_));
        if (!result)
            return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED,
                "Queued Orchard profile unavailable", hash_, height_);
        return *result;
    }
private:
    const std::shared_ptr<ChainstateService> source_;
    const std::vector<uint8_t> wire_;
    const uint256 hash_;
    const uint64_t height_;
};
}

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

    if (origin == BlockOrigin::P2P && Params().orchard_activation_height != UINT32_MAX) {
        // Classification is captured under the selected lock and never inferred
        // from a transaction version. Release that lock BEFORE queue waiting.
        if (!ctx_ || hex_block.size() < 256 || hex_block.size() % 2 ||
            hex_block.size() / 2 > 64 * 1024 * 1024)
            return BlockAcceptResult::Rejected(BlockRejectCode::PARSE_ERROR, "Invalid queued block frame");
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_->chainstate);
        if (!chainstate)
            return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, "Queued block owner unavailable");
        std::vector<uint8_t> prefix;
        if (!util::unhex(hex_block.substr(0,256),prefix))
            return BlockAcceptResult::Rejected(BlockRejectCode::PARSE_ERROR, "Invalid queued block header");
        const auto header = BlockHeader::Deserialize(prefix);
        if (!header)
            return BlockAcceptResult::Rejected(BlockRejectCode::PARSE_ERROR, "Invalid queued block header");
        uint64_t height = 0;
        bool orchard = false;
        {
            auto selected = chainstate->AcquireBlockIngressActivationLock();
            auto* db = chainstate->GetChainDB();
            if (!db || !consensus::OrchardProfileConfigurationValid(Params()))
                return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, "Queued block profile unavailable", header->GetHash());
            const auto parent = db->getBlockHeight(header->prev_block_hash);
            if (!parent.ok())
                return BlockAcceptResult::Rejected(parent.status() == Status::NotFound ? BlockRejectCode::MISSING_PARENT : BlockRejectCode::CONNECT_FAILED,
                    "Queued block parent unavailable", header->GetHash());
            if (*parent < 0 || *parent >= INT32_MAX)
                return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, "Queued block parent height unavailable", header->GetHash());
            height = uint64_t(*parent) + 1;
            orchard = consensus::OrchardActiveForHeight(Params(), static_cast<uint32_t>(height));
        }
        if (orchard) {
            const auto queue = ctx_->validation_queue;
            if (!queue || !queue->isRunning())
                return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, "Orchard validation queue unavailable", header->GetHash(), height);
            std::vector<uint8_t> wire;
            if (!util::unhex(hex_block,wire))
                return BlockAcceptResult::Rejected(BlockRejectCode::PARSE_ERROR, "Invalid queued Orchard block encoding", header->GetHash(), height);
            auto result=queue->submitAndWait(std::make_shared<QueuedOrchardBlock>(
                chainstate,std::move(wire),header->GetHash(),height));
            result.relayed=AnnounceAcceptedOrchardBlock(chainstate,result);
            return result;
        }
    }
    // Existing historical/RPC handling is unchanged.
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
