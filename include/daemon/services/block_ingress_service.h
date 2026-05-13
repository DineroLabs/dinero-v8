#pragma once
#include "daemon/iservice.h"
#include "daemon/interfaces/block_ingress.h"  // Step 5: IBlockIngress
#include "daemon/interfaces/origin.h"         // Step 5: BlockOrigin
#include <memory>
#include <string>

namespace dinero {

// Forward declarations
class ILogger;

/**
 * BlockIngressService - IBlockIngress interface implementation
 *
 * Step 5: Provides the canonical block ingress interface.
 * Wraps BlockAcceptor static methods into IBlockIngress interface.
 * External code should access via IBlockIngress*, not this service directly.
 *
 * Dependencies: Logger, ChainstateService (for BlockAcceptor context)
 *
 * Initialization order:
 * - Init() stores dependencies
 * - Start() no-op (BlockAcceptor uses static context)
 * - Stop() no-op
 */
class BlockIngressService : public IService, public IBlockIngress {
public:
    BlockIngressService() = default;
    ~BlockIngressService() override = default;

    std::string Name() const override { return "BlockIngress"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Service health
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // ========================================================================
    // IBlockIngress INTERFACE IMPLEMENTATION (Step 5)
    // ========================================================================

    /**
     * Submit a block for validation and chain acceptance (IBlockIngress interface)
     *
     * @param block The block to submit
     * @param origin Where this block came from
     * @return Structured result with accept/reject code, reason, and chain state
     */
    BlockAcceptResult Submit(const Block& block, BlockOrigin origin) override;

    /**
     * Submit a hex-encoded block (IBlockIngress interface)
     *
     * @param hex_block Hex-encoded block data
     * @param origin Where this block came from
     * @return Structured result with accept/reject code, reason, and chain state
     */
    BlockAcceptResult SubmitHex(const std::string& hex_block, BlockOrigin origin) override;

private:
    BlockAcceptResult SubmitViaValidationQueue(const Block& block);

    ::DaemonContext* ctx_ = nullptr;
    ILogger* logger_ = nullptr;
    bool started_ = false;
};

} // namespace dinero
