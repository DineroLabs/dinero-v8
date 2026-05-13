#pragma once

#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utxo_entry.h"  // Use existing UTXOEntry definition
#include <optional>
#include <cstdint>
#include <vector>

namespace dinero {
namespace consensus {

/**
 * @file global_utxo_set.h
 * @brief Read-only, globally consistent view of the UTXO set at current chain tip
 *
 * Phase 11a: GlobalUTXOSet Implementation
 *
 * IMPORTANT: This is NOT a new UTXO store. It is a read-only adapter/facade over:
 * - CoinsDB (canonical UTXO data)
 * - UtreexoForest (canonical accumulator)
 *
 * Design principles:
 * - Read-only (cannot mutate chain state)
 * - Snapshot semantics (valid only at one chain tip)
 * - No duplication (delegates to existing components)
 * - Thread-safe (inherits Chainstate's locking)
 * - Cannot affect consensus (worst case: RPC errors)
 *
 * Usage pattern:
 * ```cpp
 * auto guard = chainstate.AcquireReadLock();
 * GlobalUTXOSetImpl utxoset(chainstate);
 * auto entry = utxoset.Lookup(txid, vout);
 * // use entry
 * // guard goes out of scope, lock released
 * ```
 *
 * Rule: NEVER store GlobalUTXOSet globally. Create on demand or cache per-height.
 */

/**
 * @brief Read-only interface to the global UTXO set
 *
 * Provides:
 * - UTXO lookup by (txid, vout)
 * - Utreexo proof generation
 * - Current accumulator root
 * - Chain height consistency
 *
 * Does NOT provide:
 * - UTXO modification (use Chainstate)
 * - Historical queries (use block index)
 * - Iteration (use CoinsDB directly if needed)
 */
class GlobalUTXOSet {
public:
    /**
     * @brief Look up UTXO by outpoint
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return UTXO entry if unspent, nullopt if spent or never existed
     */
    virtual std::optional<UTXOEntry>
    Lookup(const uint256& txid, uint32_t vout) const = 0;

    /**
     * @brief Check if outpoint is unspent
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if UTXO exists and is unspent
     */
    virtual bool
    IsUnspent(const uint256& txid, uint32_t vout) const = 0;

    /**
     * @brief Generate Utreexo inclusion proof for this UTXO
     *
     * Proof demonstrates that the UTXO exists in the current accumulator.
     * Proof is valid only against the current Utreexo root.
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return Utreexo proof, or nullopt if UTXO not found
     */
    virtual std::optional<UtreexoProof>
    GenerateProof(const uint256& txid, uint32_t vout) const = 0;

    /**
     * @brief Get current Utreexo accumulator root
     *
     * This is the root hash that commits to the entire UTXO set.
     * Corresponds to the root in the current chain tip's block header.
     *
     * @return 32-byte Utreexo commitment hash
     */
    virtual UtreexoHash
    GetCurrentUtreexoRoot() const = 0;

    /**
     * @brief Get all Utreexo forest roots
     *
     * Returns the roots of all perfect binary trees in the forest.
     * For debugging and RPC queries.
     *
     * @return Vector of forest root hashes
     */
    virtual std::vector<UtreexoHash>
    GetUtreexoRoots() const = 0;

    /**
     * @brief Get Utreexo accumulator statistics
     *
     * @return Number of leaves (UTXOs) in the accumulator
     */
    virtual uint64_t
    GetNumLeaves() const = 0;

    /**
     * @brief Get chain height this view corresponds to
     *
     * Snapshot semantics: this view is valid only at this specific height.
     * If chain tip advances, create a new GlobalUTXOSet instance.
     *
     * @return Block height
     */
    virtual uint32_t
    GetHeight() const = 0;

    virtual ~GlobalUTXOSet() = default;
};

} // namespace consensus
} // namespace dinero
