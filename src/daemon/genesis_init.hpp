// src/daemon/genesis_init.hpp
// Genesis block initialization for Dinero

#pragma once

namespace dinero {

// Forward declarations
class ChainDB;
class BlockStorage;
class UTXOIndex;

/**
 * Initialize genesis block (height 0)
 *
 * Creates and stores the genesis block with the same flatfile + metadata
 * contract used by the rest of the chain.
 *
 * @param chain_db ChainDB instance to store blocks
 * @param block_storage BlockStorage instance to persist the canonical body
 * @param utxo_set UTXOIndex instance to track UTXOs (optional)
 * @return true if successful, false on error
 */
bool InitializeGenesis(ChainDB* chain_db, BlockStorage* block_storage, UTXOIndex* utxo_set);

} // namespace dinero
