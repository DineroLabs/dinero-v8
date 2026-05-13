#pragma once

#include "consensus/global_utxo_set.h"
#include "primitives/uint256.h"
#include <cstdint>

// Forward declarations
namespace dinero {
    class ChainstateService;
    class ChainDB;

    namespace consensus {
        class CoinsDB;
        class UtreexoForest;
    }

    namespace indexing {
        class UTXOPositionIndex;
    }
}

namespace dinero {
namespace consensus {

/**
 * @brief Concrete implementation of GlobalUTXOSet backed by Chainstate
 *
 * Delegates to:
 * - CoinsDB for UTXO lookups
 * - UtreexoForest for accumulator queries
 *
 * Thread safety: Inherits from Chainstate's locking
 * Lifetime: Valid only at construction height (snapshot semantics)
 *
 * Usage:
 * ```cpp
 * auto guard = chainstate.AcquireReadLock();  // TODO: Add lock API
 * GlobalUTXOSetImpl utxoset(chainstate);
 * auto entry = utxoset.Lookup(txid, vout);
 * ```
 */
class GlobalUTXOSetImpl final : public GlobalUTXOSet {
public:
    /**
     * @brief Construct GlobalUTXOSet from Chainstate snapshot
     *
     * @param chainstate Chainstate service (must outlive this object)
     *
     * IMPORTANT: Caller must hold chainstate read lock during construction and use.
     * This object does NOT acquire locks itself (inherited from caller's context).
     */
    explicit GlobalUTXOSetImpl(const ChainstateService& chainstate);

    // GlobalUTXOSet interface implementation
    std::optional<UTXOEntry>
    Lookup(const uint256& txid, uint32_t vout) const override;

    bool
    IsUnspent(const uint256& txid, uint32_t vout) const override;

    std::optional<UtreexoProof>
    GenerateProof(const uint256& txid, uint32_t vout) const override;

    UtreexoHash
    GetCurrentUtreexoRoot() const override;

    std::vector<UtreexoHash>
    GetUtreexoRoots() const override;

    uint64_t
    GetNumLeaves() const override;

    uint32_t
    GetHeight() const override;

private:
    // Non-owning pointers to Chainstate components
    // These are valid only during the lifetime of this object
    // and only while the caller holds the chainstate lock
    const ChainDB* coins_db_;       // UTXO data source
    const UtreexoForest* forest_;   // Accumulator source
    const uint32_t height_;         // Snapshot height
    const indexing::UTXOPositionIndex* position_index_; // Position mapping for proof generation
};

} // namespace consensus
} // namespace dinero
