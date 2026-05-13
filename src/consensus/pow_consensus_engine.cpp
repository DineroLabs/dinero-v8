#include "consensus/pow_consensus_engine.h"
#include "mining/block_assembler.h"  // Phase C: Use BlockAssembler instead of Mining
#include "daemon/block_acceptor.h"
#include "storage/chain_db.h"
#include "consensus/pow.hpp"
#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"  // Phase 11a: Canonical merkle computation
#include "consensus/witness_commitment.h"  // Phase 11c.3: Witness commitment validation
#include "common/logger.h"
#include "common/sha256d.h"
#include "util/hex.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace dinero {

/**
 * PowConsensusEngine - Proof-of-Work consensus implementation
 *
 * Phase C: Updated to use BlockAssembler for template creation
 * (Legacy Mining class removed)
 *
 * Implements IConsensusEngine using BlockAssembler for block template creation
 * and ChainDB for difficulty calculations.
 */
class PowConsensusEngine : public IConsensusEngine {
public:
    /**
     * Constructor
     *
     * @param block_assembler BlockAssembler instance for template creation
     * @param chain_db ChainDB for difficulty calculations
     */
    PowConsensusEngine(BlockAssembler* block_assembler, ChainDB* chain_db)
        : block_assembler_(block_assembler), chain_db_(chain_db) {
        // Note: block_assembler can be nullptr for validation-only use cases (e.g., tests)
        // It's only required for CreateBlockTemplate, not for ValidateBlock
    }
    
    ~PowConsensusEngine() override = default;
    
    std::string GetName() const override {
        return "PoW";
    }
    
    bool ValidateBlock(const Block& block) override {
        // Phase C.0: Comprehensive block validation (consensus-critical)
        // This performs stateless checks (doesn't require UTXO set)
        // Stateful validation (UTXO/script checks) happens via BlockValidator::ConnectBlock

        // 1. Check block header version
        if (block.header.version == 0) {
            g_logger.error("PowConsensusEngine: Invalid block version");
            return false;
        }

        // 2. Check block has at least one transaction (coinbase)
        if (block.vtx.empty()) {
            g_logger.error("PowConsensusEngine: Block has no transactions");
            return false;
        }

        // 3. Verify merkle root matches transactions
        // Phase 11a: Use canonical merkle computation
        uint256 computed_merkle = consensus::ComputeMerkleRoot(block.vtx);
        if (computed_merkle != block.header.merkle_root) {
            g_logger.error("PowConsensusEngine: Merkle root mismatch (header: " +
                          block.header.merkle_root.GetHex() + ", computed: " + computed_merkle.GetHex() + ")");
            return false;
        }

        // 4. Check Proof-of-Work
        // Phase M.1: CheckProofOfWork now takes uint256 directly
        if (!CheckProofOfWork(block.GetHash(), block.header.difficulty)) {
            g_logger.error("PowConsensusEngine: Block hash does not meet difficulty target");
            return false;
        }

        // 5. Check timestamp is not too far in the future (2 hours tolerance)
        uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
        uint32_t max_future_time = current_time + (2 * 60 * 60);  // 2 hours
        if (block.header.timestamp > max_future_time) {
            g_logger.error("PowConsensusEngine: Block timestamp too far in future");
            return false;
        }

        // ═════════════════════════════════════════════════════════════════════════
        // Phase 11c.3: Validate witness commitment (if present)
        // ═════════════════════════════════════════════════════════════════════════
        // This is OPTIONAL in Phase 11c - commitment is validated IF present,
        // but is NOT required for block validity.
        // ═════════════════════════════════════════════════════════════════════════
        std::string witness_error;
        if (!consensus::ValidateWitnessCommitment(block.vtx, witness_error)) {
            g_logger.error("PowConsensusEngine: Witness commitment validation failed: " + witness_error);
            return false;
        }
        // ═════════════════════════════════════════════════════════════════════════

        return true;
    }
    
    Block CreateBlockTemplate() override {
        if (!block_assembler_) {
            g_logger.error("PowConsensusEngine: BlockAssembler not available");
            return Block{};  // Return empty block on error
        }

        // Phase C: Use BlockAssembler to create a mining job, then construct a Block
        auto job = block_assembler_->CreateJob();
        if (!job) {
            g_logger.error("PowConsensusEngine: Failed to create mining job");
            return Block{};
        }

        // Construct Block from MiningJob
        Block block;
        block.header = job->header;
        block.vtx = job->transactions;  // Use vtx, not transactions

        return block;
    }
    
    uint32_t GetCurrentDifficulty() const override {
        if (!chain_db_) {
            return 0x1f00ffff;  // Default difficulty (regtest/testnet)
        }

        // Phase C: Return default difficulty
        // TODO: Calculate actual difficulty based on chain tip and difficulty adjustment
        // For now, return constant difficulty (regtest/testnet compatible)
        return 0x1f00ffff;  // Default difficulty
    }
    
    bool CheckProofOfWork(const uint256& block_hash, uint32_t target_bits) const override {
        if (block_hash.IsNull() || target_bits == 0) {
            return false;
        }

        // Convert uint256 to bytes (already in memory, no parsing needed)
        // Phase M.1: uint256 is little-endian internally
        std::vector<uint8_t> hash_bytes(block_hash.begin(), block_hash.end());

        // Reverse to big-endian for comparison
        std::reverse(hash_bytes.begin(), hash_bytes.end());
        
        // Decode target bits to get target value
        const auto& params = Params();
        
        // Extract exponent and mantissa from compact format
        uint32_t exp = target_bits >> 24;
        uint32_t mant = target_bits & 0x00ffffff;
        
        // Build target value (32 bytes, big-endian)
        uint8_t target[32] = {0};
        
        if (exp <= 3) {
            // Target fits in 4 bytes
            uint32_t v = mant >> (8 * (3 - exp));
            for (int i = 0; i < 4; ++i) {
                target[31 - i] = (v >> (8 * i)) & 0xff;
            }
        } else {
            // Target spans multiple bytes
            int offset = exp - 3;
            if (offset < 32) {
                target[32 - offset - 3] = (mant >> 16) & 0xff;
                target[32 - offset - 2] = (mant >> 8) & 0xff;
                target[32 - offset - 1] = mant & 0xff;
            }
        }
        
        // Compare hash (big-endian) with target (big-endian)
        // Hash must be less than or equal to target
        for (int i = 0; i < 32; ++i) {
            if (hash_bytes[i] < target[i]) {
                return true;  // Hash is less than target
            }
            if (hash_bytes[i] > target[i]) {
                return false;  // Hash is greater than target
            }
        }
        
        // Hash equals target (valid, but rare)
        return true;
    }
    
private:
    BlockAssembler* block_assembler_;  // Phase C: Non-owning pointer (owned by DaemonContext)
    ChainDB* chain_db_;                // For difficulty calculations
};

// Factory function
std::unique_ptr<IConsensusEngine> CreatePowConsensusEngine(BlockAssembler* block_assembler, ChainDB* chain_db) {
    return std::make_unique<PowConsensusEngine>(block_assembler, chain_db);
}

} // namespace dinero

