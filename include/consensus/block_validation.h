#pragma once

#include <string>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <functional>
#include "consensus/block_undo.h"
#include "consensus/utreexo_accumulator.h"  // v0.14.0.4: Utreexo enforcement
#include "consensus/ibd_mode.h"              // Phase 5: Stateless IBD support
#include "consensus/validation_mode.h"       // Phase 8: Stateless validation mode
#include "consensus/interfaces/iconsensus_utxo_set.h"  // Phase 2: Pure consensus UTXO set
#include "primitives/transaction.h"
#include "primitives/block.h"

namespace dinero {
namespace consensus {

// Forward declaration for CPU budget monitoring (Phase E.3)
class CPUBudgetMonitor;

/**
 * Core block validation and UTXO management
 * Implements ConnectBlock/DisconnectBlock for consensus-critical UTXO operations
 * v0.14.0.4: Now enforces Utreexo commitments
 *
 * BLOCK VALIDATION INVARIANTS (runtime-enforced):
 * INV-1: Post-commit forest root == computed root (abort on violation)
 * INV-2: No UTXO state mutation before proof verification (deferred spends)
 * INV-3: No double-spend in stateless mode (outpoint tracking set)
 * INV-4: Snapshot restore produces correct UTXO count (DisconnectBlock)
 * INV-5: AddCoin completes before forest commit (ordering assertion)
 */
class BlockValidator {
public:
    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 2: Pure Consensus Architecture
    // ═══════════════════════════════════════════════════════════════════════════
    // BlockValidator operates on IConsensusUTXOSet (pure in-memory).
    // The UTXO set OWNS the Utreexo forest - no separate forest initialization.
    //
    // Invariants:
    // - No persistence headers (rocksdb, filesystem)
    // - No threading headers (mutex, thread)
    // - Snapshot-first validation (failed block = restore snapshot)
    // ═══════════════════════════════════════════════════════════════════════════

    explicit BlockValidator(IConsensusUTXOSet* utxo_set);
    ~BlockValidator() = default;

    // Forest access via UTXO set (Phase 2: forest owned by ConsensusUTXOSet)
    bool hasUtreexoForest() const { return consensus_utxo_set_ != nullptr; }

    // Direct access to the consensus UTXO set. Used by fork-aware
    // utreexo validation (BlockAcceptor) to chain the live UTXO
    // view beneath an overlay — the non-overlaid path still queries
    // this set as the UTXO source of truth.
    IConsensusUTXOSet* GetConsensusUTXOSet() { return consensus_utxo_set_; }
    const IConsensusUTXOSet* GetConsensusUTXOSet() const { return consensus_utxo_set_; }

    // Phase 5: Set IBD mode (stateful vs stateless)
    void setIBDConfig(const IBDConfig& config) { ibd_config_ = config; }

    // Phase 5: Get current IBD mode
    const IBDConfig& getIBDConfig() const { return ibd_config_; }

    // Phase 8: Set validation mode (stateful vs stateless)
    void setValidationMode(ValidationMode mode) { validation_mode_ = mode; }

    // Phase 8: Get current validation mode
    ValidationMode getValidationMode() const { return validation_mode_; }

    // ═══════════════════════════════════════════════════════════════════════════
    // CONSENSUS-CRITICAL: Utreexo Enforcement (NO BYPASS)
    // ═══════════════════════════════════════════════════════════════════════════
    // Utreexo root mismatches ALWAYS reject the block. There is no shadow mode,
    // no bypass flag, no gradual rollout. This is consensus-critical from genesis.
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Phase 11a: Apply block consensus state transition (WITHOUT root verification)
     *
     * This is the PURE consensus function that computes deterministic state changes.
     * Used by BOTH mining and validation.
     *
     * Mining path: Calls this to compute what the utreexo_root SHOULD be
     * Validation path: Calls this via ValidateAndApplyBlock (which adds verification)
     *
     * - Validates all transactions against UTXO set
     * - Spends inputs (removes from UTXO set)
     * - Creates outputs (adds to UTXO set)
     * - Updates Utreexo accumulator
     * - Computes AFTER-state commitment
     * - Populates undo data for potential reorg
     *
     * @param block The block to apply
     * @param height Block height (for UTXO tracking and coinbase maturity)
     * @param block_hash Block hash (for undo tracking)
     * @param undo Output parameter for undo information
     * @param computed_utreexo_root Output parameter for computed AFTER-state root
     * @param error Output parameter for error message
     * @param cpu_monitor Optional CPU budget monitor for timeout enforcement (Phase E.3)
     * @return true if block application succeeded
     */
    bool ApplyBlock(const Block& block, uint32_t height, const uint256& block_hash,
                    BlockUndo& undo, uint256& computed_utreexo_root,
                    std::string& error, CPUBudgetMonitor* cpu_monitor = nullptr);

    /**
     * Phase 11a: Validate and apply block (WITH root verification)
     *
     * This wraps ApplyBlock() and adds Utreexo root verification.
     * Used by validation/sync paths only.
     *
     * - Calls ApplyBlock() to compute state changes and commitment
     * - Verifies computed commitment matches block header
     * - REJECTS if mismatch (consensus rule)
     *
     * @param block The block to validate and apply
     * @param height Block height
     * @param block_hash Block hash
     * @param undo Output parameter for undo information
     * @param error Output parameter for error message
     * @param cpu_monitor Optional CPU budget monitor
     * @return true if block is valid and applied successfully
     */
    bool ValidateAndApplyBlock(const Block& block, uint32_t height, const uint256& block_hash,
                               BlockUndo& undo, std::string& error,
                               CPUBudgetMonitor* cpu_monitor = nullptr);

    /**
     * Phase 11a: Compute Utreexo root (PURE - no state mutation)
     *
     * Used by MINING to compute what the utreexo_root WOULD be without
     * actually mutating chainstate. Equivalent to Bitcoin Core's TestBlockValidity().
     *
     * Implementation: Creates temporary snapshot of Utreexo forest, applies
     * block to snapshot, extracts root, discards snapshot.
     *
     * IMPORTANT: This does NOT:
     * - Mutate UTXO set
     * - Mutate Utreexo forest
     * - Update position index
     * - Validate scripts (mining path trusts its own blocks)
     *
     * @param block The block to test
     * @param height Block height
     * @param computed_utreexo_root Output parameter for computed AFTER-state root
     * @param error Output parameter for error message
     * @return true if computation succeeded
     */
    bool ComputeUtreexoRootPure(const Block& block, uint32_t height,
                                uint256& computed_utreexo_root,
                                std::string& error);

    /**
     * Like ComputeUtreexoRootPure, but uses the supplied `starting_forest`
     * and `utxo_lookup` instead of the live `consensus_utxo_set_`.
     * Useful for accept-time validation of side-chain blocks whose
     * pre-block UTXO/forest state is NOT the current main-chain state.
     *
     * The caller is responsible for constructing a lookup that returns
     * the UTXO as it existed at `(height - 1)` on the side-chain's
     * ancestry — walking back through main-chain undo data and/or
     * forward through earlier side-chain blocks. For coinbase-only
     * blocks the lookup is never consulted; a no-op that always
     * returns nullptr is fine.
     *
     * @param block                The block to test.
     * @param height               Block's height.
     * @param starting_forest      Forest state at (height - 1) for this
     *                             fork. Consumed by value.
     * @param utxo_lookup          Fork-aware UTXO lookup. Return the
     *                             UTXOEntry pointer (still owned by the
     *                             caller) for the given outpoint, or
     *                             nullptr if the outpoint is not a
     *                             spendable UTXO in this fork's view.
     * @param computed_utreexo_root  Output: computed AFTER-state root.
     * @param error                Output: error reason on failure.
     * @return true iff computation succeeded.
     */
    bool ComputeUtreexoRootPureFromForest(
        const Block& block, uint32_t height,
        UtreexoForest starting_forest,
        const std::function<const UTXOEntry*(const OutPoint&)>& utxo_lookup,
        uint256& computed_utreexo_root,
        std::string& error);

    /**
     * Connect a block to the blockchain (LEGACY WRAPPER)
     *
     * For backward compatibility, this wraps ValidateAndApplyBlock().
     * New code should use ApplyBlock() (mining) or ValidateAndApplyBlock() (sync).
     *
     * @param block The block to connect
     * @param height Block height (for UTXO tracking and coinbase maturity)
     * @param block_hash Block hash (for undo tracking)
     * @param undo Output parameter for undo information
     * @param error Output parameter for error message
     * @param cpu_monitor Optional CPU budget monitor for timeout enforcement (Phase E.3)
     * @return true if block is valid and connected successfully
     */
    bool ConnectBlock(const Block& block, uint32_t height, const uint256& block_hash, BlockUndo& undo, std::string& error, CPUBudgetMonitor* cpu_monitor = nullptr);
    
    /**
     * Disconnect a block from the blockchain (for reorgs)
     * - Removes all outputs created in this block
     * - Restores all inputs spent in this block (from undo data)
     * - Leaves UTXO set in state before this block
     *
     * @param block The block to disconnect
     * @param height Block height
     * @param undo The undo information from when block was connected
     * @param error Output parameter for error message
     * @return true if block disconnected successfully
     */
    bool DisconnectBlock(const Block& block, uint32_t height, const BlockUndo& undo, std::string& error);
    
    /**
     * Validate a single transaction against the UTXO set
     * - Check all inputs exist and are unspent
     * - Check coinbase maturity (100 blocks)
     * - Verify no double-spends
     * - Calculate and validate fees
     * 
     * @param tx The transaction to validate
     * @param height Current blockchain height
     * @param is_coinbase Whether this is a coinbase transaction
     * @param total_input_value Output parameter for sum of input values
     * @param error Output parameter for error message
     * @return true if transaction is valid
     */
    bool ValidateTransaction(const Transaction& tx, uint32_t height, bool is_coinbase,
                           uint64_t& total_input_value, std::string& error);
    
    // NOTE: Removed VerifyP2WPKH - now using ScriptVerifier directly (supports P2WPKH + Taproot)

    /**
     * Calculate block subsidy based on Dinero consensus rules (FROZEN MONETARY POLICY)
     * Height 0: 0 (genesis unspendable, OP_RETURN)
     * Height 1+: 100 DIN initial, halving every 1,314,000 blocks (~5 years at 2 min blocks)
     * Tail emission: max(halving_subsidy, 1 DIN) forever
     *
     * NOTE: Use ConsensusSubsidy::GetBlockSubsidy(height) instead - this is the authoritative source.
     * This function may be deprecated in favor of direct ConsensusSubsidy usage.
     *
     * Subsidy is purely height-based - no dependency on total_issued or chain state.
     *
     * @param height Block height
     * @return Block subsidy in una (una)
     */
    static uint64_t GetBlockSubsidy(uint32_t height);
    
private:
    // Phase 11a: Internal implementation shared by ApplyBlock and ValidateAndApplyBlock
    bool ConnectBlockInternal(const Block& block, uint32_t height, const uint256& block_hash,
                              BlockUndo& undo, uint256& computed_utreexo_root,
                              bool verify_root, std::string& error,
                              CPUBudgetMonitor* cpu_monitor = nullptr);

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 2: Pure Consensus - Single Dependency
    // ═══════════════════════════════════════════════════════════════════════════
    // IConsensusUTXOSet provides:
    //   - UTXO operations (GetCoin, AddCoin, SpendCoin, DeleteCoin)
    //   - Utreexo forest (via GetForest())
    //   - Snapshot/Restore for trivial reorg
    //
    // NO separate forest pointer. NO persistence. NO threading.
    // ═══════════════════════════════════════════════════════════════════════════

    IConsensusUTXOSet* consensus_utxo_set_;  // Phase 2: Pure consensus UTXO set (owns forest)
    IBDConfig ibd_config_;  // Phase 5: IBD mode (stateful vs stateless)
    ValidationMode validation_mode_ = ValidationMode::STATEFUL;  // Phase 8: Validation mode (default: stateful)

    // Intra-block UTXO overlay: tracks outputs created by earlier transactions
    // in the same block, enabling transaction chaining within a single block.
    // Cleared at the start of each ConnectBlockInternal call.
    // Entries are erased as they are spent to prevent intra-block double-spends.
    std::unordered_map<OutPoint, UTXOEntry> intra_block_utxos_;

    // v7 Shielded pool state — injected by ChainstateService.
    // Forward-declared as opaque pointers to avoid pulling shielded
    // headers into the block validation header.
    void* shielded_tree_           = nullptr;  // CommitmentTree*
    void* shielded_nullifiers_     = nullptr;  // NullifierSet*
    void* shielded_anchor_history_ = nullptr;  // AnchorHistory* (Phase 3 wave 1)
    void* active_consensus_write_batch_ = nullptr;  // ConsensusWriteBatch* (phase 3a)
public:
    void setShieldedState(void* tree, void* nullifiers, void* anchor_history = nullptr) {
        shielded_tree_           = tree;
        shielded_nullifiers_     = nullifiers;
        shielded_anchor_history_ = anchor_history;
    }
    bool hasShieldedState() const { return shielded_tree_ && shielded_nullifiers_; }

    // Phase 3a of the shielded reorg invertibility plan
    // (docs/specs/atomic_consensus_persistence_phase3.md). When the
    // hidden flag is on, ChainstateService::ConnectTip constructs a
    // ConsensusWriteBatch and points the validator at it for the
    // duration of one block apply. UTXO additions and spends inside
    // ConnectBlockInternal are routed onto the batch's working copy
    // instead of the live consensus_utxo_set_; the batch's Commit()
    // replays them as one atomic step.
    //
    // Caller MUST clear the pointer before the batch destructs.
    // RAII'd from the call site via ScopedActiveConsensusWriteBatch.
    // Kept as void* in the header for the same reason shielded state
    // is opaque here — phase 3a does not pull a new include into
    // every translation unit that drags this header in.
    void setActiveConsensusWriteBatch(void* batch) {
        active_consensus_write_batch_ = batch;
    }
    void* getActiveConsensusWriteBatch() const {
        return active_consensus_write_batch_;
    }

private:

    // Coinbase maturity (100 blocks)
    static constexpr uint32_t COINBASE_MATURITY = 100;

    // Helper: Sum all output values in a transaction
    uint64_t SumOutputs(const Transaction& tx) const;

    // Helper: Check if an output is standard P2WPKH
    bool IsStandardP2WPKH(const std::vector<uint8_t>& scriptPubKey) const;
};

} // namespace consensus
} // namespace dinero
