#pragma once

#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "common/status.h"

#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * ChainStateView - Read-only UTXO query interface
 *
 * Phase M.1: Minimal interface for correct boundaries, not speed.
 * Batch APIs deferred to Phase M.2 (avoid premature optimization).
 *
 * This abstraction decouples mempool validation from CoinsViewCache implementation,
 * enabling:
 * - Future UTXO storage backends (Utreexo, remote UTXO service)
 * - Clean separation between consensus state and mempool policy
 * - Type-safe validation without tight coupling
 *
 * Implementations:
 * - CoinsViewCache (consensus L1 cache for block validation)
 * - Future: Utreexo, pruned node UTXO service, etc.
 */
class ChainStateView {
public:
    virtual ~ChainStateView() = default;

    /**
     * Get UTXO entry for a given outpoint
     *
     * @param outpoint Transaction output identifier (txid + vout)
     * @return UTXOEntry if found, NotFound error otherwise
     *
     * Thread-safety: Implementation-defined. CoinsViewCache is safe for
     * single-threaded use (block validation, mempool validation).
     */
    virtual StatusOr<UTXOEntry> getCoin(const OutPoint& outpoint) const = 0;

    /**
     * Check if UTXO exists
     *
     * @param outpoint Transaction output identifier
     * @return true if UTXO exists in chainstate
     *
     * This is equivalent to getCoin().ok() but may be more efficient
     * for some implementations.
     */
    virtual bool hasCoin(const OutPoint& outpoint) const = 0;

    /**
     * Get current blockchain height
     *
     * @return Height of active chain tip
     *
     * Used for:
     * - Coinbase maturity checks (height >= coinbase_height + 100)
     * - Relative timelock validation (CSV)
     * - Fee estimation context
     */
    virtual uint32_t getHeight() const = 0;
};

} // namespace consensus
} // namespace dinero
