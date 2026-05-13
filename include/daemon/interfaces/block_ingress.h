#pragma once

#include "daemon/interfaces/origin.h"
#include "daemon/interfaces/ingress_types.h"  // BlockAcceptResult, BlockRejectCode (no impl headers)
#include "primitives/block.h"
#include <string>

namespace dinero {

/**
 * Step 5: Canonical Block Ingress Interface
 *
 * This is the ONLY way external components should submit blocks.
 *
 * Implementors:
 *   - BlockIngressService (production)
 *   - MockBlockIngress (testing)
 *
 * Consumers:
 *   - RPC handlers (submitblock)
 *   - P2P message handlers (block messages)
 *   - Miner (after finding valid nonce)
 */
struct IBlockIngress {
    virtual ~IBlockIngress() = default;

    /**
     * Submit a block for validation and chain acceptance.
     *
     * @param block The block to submit
     * @param origin Where this block came from (for logging/relay decisions)
     * @return Structured result with accept/reject code, reason, and chain state
     */
    virtual BlockAcceptResult Submit(const Block& block, BlockOrigin origin) = 0;

    /**
     * Submit a block in hex format for validation and chain acceptance.
     *
     * Used by RPC submitblock which receives hex-encoded blocks.
     *
     * @param hex_block Hex-encoded block data
     * @param origin Where this block came from
     * @return Structured result with accept/reject code, reason, and chain state
     */
    virtual BlockAcceptResult SubmitHex(const std::string& hex_block, BlockOrigin origin) = 0;
};

} // namespace dinero
