/**
 * @file genesis_canonical.cpp
 * @brief Phase 3.1 canonical genesis block implementation
 *
 * CRITICAL: This file contains the IMMUTABLE mainnet genesis block.
 * Genesis parameters are hardcoded from mining results (NOT dynamically generated).
 *
 * v7 Genesis Block (BlockHeader v1 - 128 bytes):
 * - Mined: 2026-04-17
 * - Hash: 0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f
 * - Nonce: 813915426
 * - Timestamp: 1776384000 (2026-04-17 00:00:00 UTC)
 * - Difficulty: 0x1d31ffce (50x easier than Bitcoin; reused from v5)
 * - Motto: "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026"
 * - No premine (coinbase burned via OP_RETURN; double commitment in scriptSig + OP_RETURN)
 * - Utreexo root: empty forest commitment (identical to v5; no UTXO created at genesis)
 */

#include "consensus/genesis_canonical.h"
#include "consensus/chain_bundle_generated.h"
#include "consensus/utreexo_accumulator.h"
#include "primitives/uint256.h"
#include <cassert>
#include <cstdlib>  // std::abort — genesis-integrity gate must fire even under NDEBUG
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace dinero {

// ============================================================================
// PHASE 3 GENESIS CONSTANTS (IMMUTABLE - DO NOT MODIFY)
// ============================================================================

// Expected genesis hash (display format - big-endian hex)
static constexpr const char* MAINNET_GENESIS_HASH_HEX =
    dinero::chain_bundle::GENESIS_BLOCK_HASH;

// Merkle root (display format - big-endian hex)
static constexpr const char* MAINNET_MERKLE_ROOT_HEX =
    dinero::chain_bundle::GENESIS_MERKLE_ROOT;

// Exact coinbase transaction hex from genesis miner output
// DO NOT modify this hex string - it is the canonical genesis transaction
// 100 DIN burned via OP_RETURN - NO PREMINE
// Motto: "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026"
// DOUBLE COMMITMENT: Motto embedded in BOTH scriptSig AND OP_RETURN
static constexpr const char* MAINNET_COINBASE_HEX =
    dinero::chain_bundle::GENESIS_COINBASE_HEX;

// Genesis parameters (from v7 mining results)
static constexpr uint32_t GENESIS_VERSION = 1;
static constexpr uint64_t GENESIS_TIMESTAMP = dinero::chain_bundle::GENESIS_TIMESTAMP;
static constexpr uint32_t GENESIS_DIFFICULTY = dinero::chain_bundle::GENESIS_DIFFICULTY;
static constexpr uint32_t GENESIS_NONCE = dinero::chain_bundle::GENESIS_NONCE;

// ============================================================================
// GENESIS BLOCK CONSTRUCTION (Phase 3)
// ============================================================================

/**
 * @brief Build mainnet genesis block from hardcoded parameters
 *
 * This function reconstructs the genesis BlockHeader from individual fields
 * (NOT from serialized bytes) to ensure clarity and prevent bugs.
 *
 * CRITICAL CHECKS:
 * 1. Computed hash MUST match expected hash (assertion)
 * 2. Reserved[12] MUST be all zeros (assertion)
 * 3. Header serialization MUST be 128 bytes (assertion)
 *
 * If any assertion fails, the binary is INVALID and must not start.
 */
CanonicalGenesis BuildCanonicalGenesis(const ChainParams& params) {
    CanonicalGenesis genesis;

    // ========================================================================
    // STEP 1: Reconstruct BlockHeader from fields (Phase 3 - 128 bytes)
    // ========================================================================

    BlockHeader header;

    // Version (4 bytes)
    header.version = GENESIS_VERSION;

    // Previous block hash (32 bytes, all zeros for genesis)
    header.prev_block_hash = uint256();

    // Merkle root (32 bytes)
    header.merkle_root = uint256::FromHexUnsafe(MAINNET_MERKLE_ROOT_HEX);

    // Utreexo root: hardcoded empty forest commitment (from genesis mining).
    // Genesis burns 100 DIN via OP_RETURN (unspendable) → empty UTXO set.
    // HARDCODED to prevent platform-dependent UtreexoForest differences.
    header.utreexo_root = uint256::FromHexUnsafe(
        dinero::chain_bundle::GENESIS_UTREEXO_ROOT);

    // Timestamp (8 bytes, 64-bit - Phase 3)
    header.timestamp = GENESIS_TIMESTAMP;

    // Difficulty (4 bytes)
    header.difficulty = GENESIS_DIFFICULTY;

    // Nonce (4 bytes)
    header.nonce = GENESIS_NONCE;

    // Reserved field (12 bytes, MUST be all zeros - consensus rule)
    header.ZeroReserved();

    // ========================================================================
    // STEP 2: Serialize header and compute hash
    // ========================================================================

    auto header_bytes = header.SerializeForHash();

    // ASSERTION: Header MUST be exactly 128 bytes (BlockHeader v1)
    assert(header_bytes.size() == 128 &&
           "FATAL: Genesis header must be exactly 128 bytes");

    // ASSERTION: Reserved field MUST be all zeros (consensus rule)
    assert(header.IsReservedValid() &&
           "FATAL: Genesis header reserved field must be all zeros");

    // Compute the block hash
    uint256 computed_hash = header.GetHash();

    // ========================================================================
    // STEP 3: MANDATORY HASH VERIFICATION
    // ========================================================================

    // Expected hash from mining results
    const uint256 expected_hash =
        uint256::FromHexUnsafe(MAINNET_GENESIS_HASH_HEX);

    // CRITICAL ASSERTION: Computed hash MUST match expected hash
    // If this fails, the binary is INVALID and the chain must NOT start
    //
    // NOTE: Internal comparison uses little-endian byte order (Bitcoin-style)
    // Display format (GetHex) uses big-endian for human readability
    // Both representations are correct - they're byte-reverses of each other
    if (computed_hash != expected_hash) {
        std::cerr << "\n❌ GENESIS HASH MISMATCH:\n";
        std::cerr << "Computed (display): " << computed_hash.GetHex() << "\n";
        std::cerr << "Expected (display): " << expected_hash.GetHex() << "\n\n";

        // Write both headers to files for comparison
        std::ostringstream computed_hex;
        computed_hex << std::hex << std::setfill('0');
        for (size_t i = 0; i < header_bytes.size(); i++) {
            computed_hex << std::setw(2) << (int)header_bytes[i];
        }

        std::cerr << "Computed header: " << computed_hex.str() << "\n";
        std::cerr << "\n";
        // MUST abort even in release builds. This is the daemon's genesis
        // self-test (genesis_init.cpp calls BuildCanonicalGenesis() purely for
        // this check). Release builds define NDEBUG, which turns assert() into a
        // no-op — so an assert() here would let a binary with the WRONG compiled-in
        // genesis start and serve a divergent chain. std::abort() is unconditional.
        std::cerr << "FATAL: Genesis hash mismatch — binary is invalid; refusing to start.\n";
        std::cerr.flush();
        std::abort();
    }

    // ========================================================================
    // STEP 4: Package results
    // ========================================================================

    genesis.header = header;
    genesis.coinbase_hex = std::string(MAINNET_COINBASE_HEX);
    genesis.hash_hex = std::string(MAINNET_GENESIS_HASH_HEX);

    return genesis;
}

} // namespace dinero
