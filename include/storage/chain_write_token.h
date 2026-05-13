#pragma once

namespace dinero {

// Forward declarations
class BlockAcceptor;
class ChainDB;
class BlockStorage;
class UTXOIndex;
class HeaderSyncManager;  // Phase H.3: Header-first sync persistence

namespace daemon {
    class PruneService;   // Phase P.2: Block data pruning
}

namespace consensus {
    class BlockReindexer;  // Blockchain database rebuild from blk*.dat files
}

// Forward declare genesis initialization function
bool InitializeGenesis(ChainDB* chain_db, BlockStorage* block_storage, UTXOIndex* utxo_set);

/**
 * ChainWriteToken - Unforgeable authorization for ChainDB writes
 *
 * DESIGN:
 * Only BlockAcceptor can construct this token. All ChainDB write operations
 * require passing a ChainWriteToken, making unauthorized writes physically
 * impossible at compile-time.
 *
 * This is a structural lock, not policy. It prevents:
 * - Miner from writing blocks
 * - Network from mutating state
 * - RPC from side effects
 * - Wallet from touching consensus data
 * - Tests from accidentally corrupting chain
 *
 * INVARIANT:
 * ChainDB mutations are ONLY valid when performed by BlockAcceptor.
 * This token enforces that invariant at compile-time.
 *
 * EXCEPTIONS - Bootstrap Cases Only:
 * 1. ChainDB::init() - One-time database schema initialization
 * 2. InitializeGenesis() - One-time genesis block creation
 * 3. Test code - Explicitly opted-in tests for validation
 *
 * After bootstrap, ALL writes must go through BlockAcceptor.
 */
class ChainWriteToken {
private:
    // Private constructor - only friends can create
    ChainWriteToken() = default;

    // ═══════════════════════════════════════════════════════════════════
    // NORMAL OPERATION (Post-Bootstrap)
    // ═══════════════════════════════════════════════════════════════════

    // Only BlockAcceptor may construct this token (normal operation)
    friend class BlockAcceptor;

    // ChainManager may construct during ActivateBestChain (chain coordination)
    friend class ChainManager;

    // ChainstateService may construct during reorg operations (Phase F.12)
    // Service layer has write authority for ActivateBestChain reorg persistence
    friend class ChainstateService;

    // Phase H.3: HeaderSyncManager may write header metadata (header-first sync)
    // Note: HeaderSyncManager writes ONLY minimal header metadata (parent_hash,
    // height, chainwork, status_flags) for restart recovery. It does NOT write
    // block bodies or UTXO data.
    friend class HeaderSyncManager;

    // Phase P.2: PruneService may write prune metadata and update block index flags
    // Note: PruneService writes ONLY prune_height metadata and clears
    // BLOCK_HAVE_DATA/BLOCK_HAVE_UNDO flags after deleting block/undo data.
    // It does NOT write block bodies or UTXO data.
    friend class daemon::PruneService;

    // ═══════════════════════════════════════════════════════════════════
    // BOOTSTRAP EXCEPTIONS (One-Time Setup Only)
    // ═══════════════════════════════════════════════════════════════════

    // ChainDB may construct ONLY during init() for schema setup
    friend class ChainDB;

    // Genesis initialization function (one-time bootstrap)
    friend bool InitializeGenesis(ChainDB*, BlockStorage*, UTXOIndex*);

    // BlockReindexer may construct for database rebuild operations (recovery)
    // This is a bootstrap operation that rebuilds the entire database from blk*.dat files
    friend class consensus::BlockReindexer;

    // DaemonApp may construct during startup for height index audit (one-time repair)
    friend class DaemonApp;

public:
    // Non-copyable, non-movable (single-use authorization)
    ChainWriteToken(const ChainWriteToken&) = delete;
    ChainWriteToken& operator=(const ChainWriteToken&) = delete;
    ChainWriteToken(ChainWriteToken&&) = delete;
    ChainWriteToken& operator=(ChainWriteToken&&) = delete;

    // ═══════════════════════════════════════════════════════════════════
    // TEST-ONLY Token Creator
    // ═══════════════════════════════════════════════════════════════════
    // WARNING: This function BYPASSES all invariant protection!
    // ONLY use in test code where you need to directly manipulate ChainDB.
    // Production code must NEVER call this - use BlockAcceptor instead.
    static ChainWriteToken CreateForTesting() {
        return ChainWriteToken();
    }
};

} // namespace dinero
