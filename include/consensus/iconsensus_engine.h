#pragma once

#include "primitives/block.h"
#include <string>
#include <memory>

// v2.2.0: REMOVED daemon/daemon_context.h dependency
// Consensus interface must NOT depend on daemon layer (Bitcoin Core pattern)
// This breaks critical layer violation where consensus code pulled in daemon infrastructure

namespace dinero {

// Forward declarations
class ChainDB;
class Blockchain;
class Mempool;

/**
 * IConsensusEngine - Abstract interface for consensus mechanisms
 * 
 * Allows swapping between PoW, PoS, hybrid, or other consensus algorithms
 * at runtime without changing core daemon code.
 * 
 * This abstraction enables:
 * - Modular consensus design
 * - Easy testing with mock engines
 * - Future PoS/hybrid implementations
 * - Consensus algorithm experimentation
 */
class IConsensusEngine {
public:
    virtual ~IConsensusEngine() = default;
    
    /**
     * Validate a block according to consensus rules
     *
     * v2.2.0: REMOVED DaemonContext parameter (layer violation)
     * Consensus validation should be stateless and not depend on daemon infrastructure.
     * Block validators inject their own dependencies (UTXO provider, chain DB, etc.)
     *
     * @param block The block to validate
     * @return true if block is valid, false otherwise
     */
    virtual bool ValidateBlock(const Block& block) = 0;
    
    /**
     * Create a new block template ready for mining/validation
     *
     * v2.2.0: REMOVED DaemonContext parameter (layer violation)
     * Implementations inject block assembler and other dependencies at construction.
     *
     * @return Block template, or empty block if creation failed
     */
    virtual Block CreateBlockTemplate() = 0;
    
    /**
     * Get the name of this consensus engine (e.g., "PoW", "PoS", "Hybrid")
     * 
     * @return Consensus engine name
     */
    virtual std::string GetName() const = 0;
    
    /**
     * Get current difficulty/target for mining
     *
     * v2.2.0: REMOVED DaemonContext parameter (layer violation)
     * Implementations inject chain DB at construction for difficulty calculations.
     *
     * @return Difficulty bits (compact format)
     */
    virtual uint32_t GetCurrentDifficulty() const = 0;
    
    /**
     * Check if a block hash meets the difficulty target
     *
     * @param block_hash Block hash to check (Phase M.1: uint256)
     * @param target_bits Difficulty target (compact format)
     * @return true if hash meets target, false otherwise
     */
    virtual bool CheckProofOfWork(const uint256& block_hash, uint32_t target_bits) const = 0;
};

} // namespace dinero

