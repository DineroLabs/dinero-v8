#pragma once

// Forward declarations
class RpcRegistry;
namespace dinero {
    class ChainDB;
}

/**
 * Register block invalidation RPC handler (regtest-only)
 * - invalidateblock: Invalidate the chain tip and roll back to parent
 */
void registerBlockInvalidation(
    RpcRegistry& registry,
    dinero::ChainDB* chaindb
);
