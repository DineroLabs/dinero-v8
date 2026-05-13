// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "primitives/block.h"
#include "primitives/uint256.h"
#include "common/status.h"

namespace dinero {

/**
 * IChainDataView — Read-only chain data access interface
 *
 * **Purpose:** Provide substitutable read access to blockchain data
 * **Triggered by:** Phase 7.4.2 (Utreexo proof serving)
 *
 * **What this enables:**
 * - BridgeNode can generate proofs without direct ChainDB dependency
 * - Active P2P and relay handlers can access blocks via a clean interface
 * - Tests can mock chain data without faking ChainDB inheritance
 * - Future consumers (RPC, indexing, snapshots) have clear abstraction
 *
 * **What this does NOT do:**
 * - No write access (read-only view)
 * - No reorg logic
 * - No persistence
 * - No locks/transactions
 * - No indexes
 *
 * **Design Philosophy:**
 * This is a minimal, surgical interface extraction.
 * Only read-paths become virtual.
 * ChainDB remains concrete for write operations.
 *
 * **Implementation:**
 * - ChainDB implements this interface (primary production implementation)
 * - MockChainDataView for tests (in-memory simulation)
 * - Future: Pruned nodes, snapshot readers, remote chain data
 *
 * **API Design:**
 * Uses StatusOr<T> to match ChainDB's existing error handling pattern.
 * This maintains consistency with the rest of the codebase.
 */
class IChainDataView {
public:
    virtual ~IChainDataView() = default;

    /**
     * @brief Read full block by hash
     * @param hash Block hash to look up
     * @return StatusOr<Block> containing block or error
     */
    virtual StatusOr<Block> getBlock(const uint256& hash) const = 0;

    /**
     * @brief Read block header by hash
     * @param hash Block hash to look up
     * @return StatusOr<BlockHeader> containing header or error
     */
    virtual StatusOr<BlockHeader> getHeader(const uint256& hash) const = 0;

    /**
     * @brief Read block hash by height
     * @param height Block height to look up
     * @return StatusOr<uint256> containing block hash or error
     */
    virtual StatusOr<uint256> getBlockHashByHeight(int height) const = 0;
};

} // namespace dinero
