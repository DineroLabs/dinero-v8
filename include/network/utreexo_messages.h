// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <vector>
#include <cstdint>
#include "primitives/uint256.h"
#include "primitives/block.h"
#include "consensus/utreexo_accumulator.h"

namespace dinero {

/**
 * Utreexo proof request flags
 * Used in GetUtreexoProofMessage.flags field
 */
namespace UtreexoProofFlags {
    constexpr uint32_t FLAG_BATCH = 0x01;           // Request is part of a batch
    constexpr uint32_t FLAG_INCLUDE_ROOTS = 0x02;   // Include accumulator roots in response
}

/**
 * GetUtreexoProofMessage - Request Utreexo proofs for blocks
 *
 * Sent by stateless nodes to bridge nodes to request batched
 * Utreexo proofs for one or more blocks.
 *
 * Phase 7.4: Proof serving protocol
 */
struct GetUtreexoProofMessage {
    std::vector<uint256> block_hashes;  // Block hashes to request proofs for
    uint32_t flags;                     // Request flags (reserved for future use)

    static constexpr size_t MAX_BATCH_SIZE = 16;  // DoS protection limit

    GetUtreexoProofMessage() : flags(0) {}

    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Validate message constraints
     * @return true if batch size is valid and non-empty
     */
    bool isValid() const {
        return !block_hashes.empty() && block_hashes.size() <= MAX_BATCH_SIZE;
    }
};

/**
 * UtreexoProofMessage - Response containing Utreexo proof for a block
 *
 * Sent by bridge nodes to stateless nodes in response to
 * GetUtreexoProofMessage. Contains cryptographic proof + metadata
 * for validating a block without full UTXO set.
 *
 * Phase 7.4: Proof serving protocol
 */
struct UtreexoProofMessage {
    uint256 block_hash;                              // Hash of block this proof applies to
    uint32_t block_height;                           // Block height
    consensus::UtreexoHash accumulator_root_before;      // Utreexo root BEFORE applying block
    consensus::UtreexoHash accumulator_root_after;       // Utreexo root AFTER applying block
    consensus::BlockUtreexoData proof_data;          // Batched proof + spent outputs

    static constexpr size_t MAX_PROOF_SIZE = 10 * 1024 * 1024;  // 10MB limit

    UtreexoProofMessage() : block_height(0) {}

    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);
};

/**
 * GetUtreexoHeadersMessage - Request block headers with Utreexo commitments
 *
 * Similar to Bitcoin's getheaders, but specifically for stateless sync.
 * Uses block locator to find common ancestor with bridge node.
 *
 * Phase 7.4: Proof serving protocol
 */
struct GetUtreexoHeadersMessage {
    uint32_t version;                      // Protocol version
    std::vector<uint256> locator_hashes;   // Block locator (for fork detection)
    uint256 hash_stop;                     // Stop hash (empty = go to tip)

    static constexpr size_t MAX_LOCATOR_SIZE = 101;

    GetUtreexoHeadersMessage() : version(1) {}

    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Validate message constraints
     * @return true if locator size is valid
     */
    bool isValid() const {
        return locator_hashes.size() <= MAX_LOCATOR_SIZE;
    }
};

/**
 * UtreexoHeadersMessage - Response containing block headers
 *
 * Sent by bridge nodes in response to GetUtreexoHeadersMessage.
 * Each header includes the utreexo_root field (128-byte BlockHeader v1 format).
 *
 * Phase 7.4: Proof serving protocol
 */
struct UtreexoHeadersMessage {
    std::vector<BlockHeader> headers;  // Headers with utreexo_root field

    static constexpr size_t MAX_HEADERS_COUNT = 2000;

    UtreexoHeadersMessage() = default;

    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Validate message constraints
     * @return true if header count is valid
     */
    bool isValid() const {
        return headers.size() <= MAX_HEADERS_COUNT;
    }
};

/**
 * Proof rejection reason codes for UtreexoProofNackMessage.
 * Tells the requesting peer WHY their proof request was not served.
 */
enum class ProofNackReason : uint8_t {
    QUEUE_FULL = 0,   // Bridge proof queue at capacity — retry after delay
    STALE = 1,        // Proof was generated but chain reorged — request again
    NOT_FOUND = 2,    // Block not found or not on canonical chain
    SHUTDOWN = 3,     // Node is shutting down — don't retry
};

/**
 * UtreexoProofNackMessage - Rejection notification for proof requests
 *
 * Sent by bridge nodes when proof generation is rejected, typically due
 * to queue backpressure. Allows CSN nodes to implement exponential backoff
 * rather than blindly retrying.
 */
struct UtreexoProofNackMessage {
    ProofNackReason reason;               // Why the request was rejected
    uint32_t retry_after_ms;              // Suggested retry delay in milliseconds (0 = don't retry)
    std::vector<uint256> block_hashes;    // Which requested hashes were rejected

    static constexpr size_t MAX_NACK_HASHES = 16;  // Same as GetUtreexoProofMessage::MAX_BATCH_SIZE

    UtreexoProofNackMessage() : reason(ProofNackReason::QUEUE_FULL), retry_after_ms(0) {}

    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);

    bool isValid() const {
        return !block_hashes.empty() && block_hashes.size() <= MAX_NACK_HASHES;
    }
};

} // namespace dinero
