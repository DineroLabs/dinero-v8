#pragma once
#include <array>
#include <string>
#include "consensus/chainparams.h"
#include "primitives/block.h"

namespace dinero {

/**
 * @brief Canonical genesis block for Phase 3 (BlockHeader v1 - 128 bytes)
 *
 * CRITICAL: This struct contains the IMMUTABLE mainnet genesis block.
 * The genesis block is reconstructed from BlockHeader fields (NOT raw bytes)
 * to ensure clarity and prevent serialization bugs.
 */
struct CanonicalGenesis {
    BlockHeader header;        // Phase 3: Complete 128-byte BlockHeader v1
    std::string coinbase_hex;  // Exact coinbase transaction hex from miner
    std::string hash_hex;      // Expected genesis hash (display format)
};

/**
 * @brief Build the canonical genesis block for the active chain
 *
 * Phase 3: Reconstructs genesis BlockHeader from fields (NOT serialized bytes)
 * Includes MANDATORY hash verification assertion.
 *
 * @param params Chain parameters containing genesis metadata
 * @return CanonicalGenesis with fully initialized BlockHeader
 *
 * FATAL CONDITIONS:
 * - If computed hash ≠ expected hash → assertion failure (binary invalid)
 * - If reserved[12] is not all zeros → assertion failure
 * - If header size ≠ 128 bytes → assertion failure
 */
CanonicalGenesis BuildCanonicalGenesis(const ChainParams& params);

} // namespace dinero
