#pragma once

#include <string>

namespace dinero {

// Forward declaration
struct ChainParams;

/**
 * @brief Verifies genesis block integrity at runtime
 *
 * This function performs self-auditing verification of the genesis block:
 *
 * BLOCK 0 (Genesis):
 * - Recomputes Merkle root from genesis coinbase transaction hex
 * - Recomputes block hash from header parameters
 * - Compares against hardcoded values in chainparams
 *
 * This ensures:
 * 1. Genesis transaction serialization is correct
 * 2. Merkle root matches expected value
 * 3. Block hash matches expected value
 * 4. No accidental divergence between builds or chain forks
 * 5. Consensus checksum is displayed for cross-node validation
 *
 * @param params The chain parameters containing genesis block data
 * @return true if all verifications pass, false otherwise (daemon aborts on false)
 */
bool VerifyGenesisBlock(const ChainParams& params);

} // namespace dinero
