#pragma once

#include "consensus/iconsensus_engine.h"
#include <memory>

namespace dinero {

// Forward declarations
class BlockAssembler;  // Phase C: Use BlockAssembler instead of Mining
class ChainDB;

/**
 * PowConsensusEngine - Proof-of-Work consensus implementation
 *
 * Phase C: Updated to use BlockAssembler for template creation
 * (Legacy Mining class removed)
 *
 * Factory function to create a PoW consensus engine instance.
 * This allows services to create the engine without knowing
 * the implementation details.
 */
std::unique_ptr<IConsensusEngine> CreatePowConsensusEngine(BlockAssembler* block_assembler, ChainDB* chain_db);

} // namespace dinero

