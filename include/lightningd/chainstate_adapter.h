#pragma once

#include "grpc/blockchain_client.h"
#include <memory>
#include <cstdint>

namespace lightningd {

/**
 * ChainstateAdapter - Adapter to make BlockchainClient look like ChainstateService
 *
 * This adapter allows Lightning components to use the same API they expect
 * (from ChainstateService) but backed by gRPC calls to dinerod instead of
 * direct database access.
 *
 * Only implements the subset of ChainstateService API that Lightning actually uses.
 */
class ChainstateAdapter {
public:
    /**
     * Construct adapter wrapping a BlockchainClient
     * @param blockchain_client gRPC client for blockchain queries
     */
    explicit ChainstateAdapter(dinero::grpc_client::BlockchainClient* blockchain_client)
        : m_blockchain_client(blockchain_client)
    {
    }

    /**
     * Get current blockchain height
     * @return Current height, or 0 if query fails
     *
     * Delegates to BlockchainClient::GetBlockHeight() via gRPC
     */
    uint32_t getBlockHeight() const {
        if (!m_blockchain_client) {
            return 0;
        }

        auto height_result = m_blockchain_client->GetBlockHeight();
        if (!height_result.ok()) {
            return 0;  // Fallback to 0 on error (matches ChainstateService behavior)
        }

        return static_cast<uint32_t>(height_result.value());
    }

private:
    dinero::grpc_client::BlockchainClient* m_blockchain_client;
};

} // namespace lightningd
