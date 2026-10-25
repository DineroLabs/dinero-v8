#pragma once

#include <string>

namespace dinero {

/**
 * Origin of a transaction submission.
 * Used for logging, metrics, and policy decisions.
 */
enum class TxOrigin {
    RPC,            // JSON-RPC submitrawtransaction
    GRPC,           // gRPC broadcast
    P2P,            // Received from peer
    WALLET,         // Local wallet sendtoaddress
    INTERNAL,       // Internal/test usage
    CORE_REOPEN,    // Durable public candidates; normal validation and relay
};

/**
 * Origin of a block submission.
 * Used for logging, metrics, and relay decisions.
 */
enum class BlockOrigin {
    RPC,            // JSON-RPC submitblock
    MINER,          // Local miner found block
    P2P,            // Received from peer
    INTERNAL,       // Internal/test usage
};

/**
 * Convert TxOrigin to string for logging/source attribution.
 */
inline const char* TxOriginToString(TxOrigin origin) {
    switch (origin) {
        case TxOrigin::RPC: return "rpc";
        case TxOrigin::GRPC: return "grpc";
        case TxOrigin::P2P: return "p2p";
        case TxOrigin::WALLET: return "wallet";
        case TxOrigin::INTERNAL: return "internal";
        case TxOrigin::CORE_REOPEN: return "core-reopen";
        default: return "unknown";
    }
}

/**
 * Convert BlockOrigin to string for logging/source attribution.
 */
inline const char* BlockOriginToString(BlockOrigin origin) {
    switch (origin) {
        case BlockOrigin::RPC: return "rpc";
        case BlockOrigin::MINER: return "miner";
        case BlockOrigin::P2P: return "p2p";
        case BlockOrigin::INTERNAL: return "internal";
        default: return "unknown";
    }
}

} // namespace dinero
