#pragma once
/**
 * Shielded Pool Nullifier Set — consensus-enforced double-spend prevention.
 *
 * Every shielded spend publishes a 32-byte nullifier derived from the
 * spender's secret key and the note's leaf index in the commitment tree:
 *
 *     nullifier = Poseidon(secret_key, leaf_index)
 *
 * If the nullifier already exists in the set → double-spend → reject.
 * The nullifier reveals nothing about which commitment was spent (the
 * secret_key is never published; the leaf_index is hidden by the ZK proof).
 *
 * Storage: persistent (on-disk), indexed for O(1) membership checks.
 * The set is append-only during normal operation. Reorgs require
 * rollback of nullifiers added in disconnected blocks.
 *
 * This set is structurally separate from Utreexo. It tracks private-side
 * double-spend prevention. Utreexo tracks public-side UTXO existence.
 * The two never intersect.
 */

#include "consensus/shielded/commitment_tree.h"  // Hash type

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct sqlite3;

namespace dinero::consensus::shielded {

class NullifierSet {
public:
    enum class OpenResult : uint8_t {
        Ok          = 0,
        IoError     = 1,
        SchemaError = 2,
    };

    NullifierSet() = default;
    ~NullifierSet();
    NullifierSet(const NullifierSet&) = delete;
    NullifierSet& operator=(const NullifierSet&) = delete;

    /** Open or create the nullifier database at `path`. Idempotent. */
    OpenResult Open(const std::string& path);
    void Close() noexcept;

    /**
     * Check if a nullifier has already been published.
     * Consensus code calls this for every shielded spend in a block/mempool tx.
     */
    bool Contains(const Hash& nullifier) const;

    /**
     * Add a nullifier to the set. Called when a block containing a
     * shielded spend is connected. Returns false if already present
     * (double-spend — caller should reject the block).
     */
    bool Insert(const Hash& nullifier, uint32_t block_height);

    /**
     * Remove all nullifiers added at heights > `height`.
     * Called during a reorg to roll back disconnected blocks.
     */
    void RollbackAbove(uint32_t height);

    /**
     * Phase 3b nullifier fold-in: wipe every row.
     *
     * Used by ChainstateService at startup when the sqlite cache
     * disagrees with the ChainDB-canonical nullifier rows — Clear()
     * lets the rebuild repopulate from ChainDB without the
     * RollbackAbove "strictly greater than" constraint.
     */
    void Clear();

    /** Number of nullifiers in the set. */
    uint64_t Size() const;

    /**
     * Phase 3b step 1 — content fingerprint of every nullifier in
     * the set, in deterministic order. Format:
     *
     *   tag 'NSCF' || version=1 || count_LE_u64 ||
     *     for each entry (sorted by block_height ASC, then nullifier ASC):
     *       block_height_LE_u32 || nullifier_bytes (32)
     *
     * Order is `(block_height ASC, nullifier ASC)` so two nodes at
     * the same tip produce byte-identical output regardless of
     * insertion order. Fed into `daemon.shieldedstatehash` v2 as
     * the input to a SHA256 — that gives the property test a
     * content-level oracle for nullifier drift, not just count
     * drift (audit gap #9). Returns an empty vector if the
     * database is closed.
     */
    std::vector<uint8_t> SerializeContent() const;

    /**
     * Replace the entire set with the content produced by SerializeContent().
     * Clears the table first, then re-inserts every (block_height, nullifier).
     * Used by the shielded epoch reset's reorg undo to restore the pre-cutover
     * nullifier set when a reorg crosses the cutover height. Returns false on a
     * malformed payload or DB error (leaving the set cleared).
     */
    bool DeserializeContent(const std::vector<uint8_t>& bytes);

    /**
     * Visit every (block_height, nullifier) in ascending (height, nullifier)
     * order. Returning false from the visitor aborts the scan early. Used by the
     * shielded epoch reset's reorg undo to re-put the restored nullifier rows
     * into the authoritative ChainDB set on a disconnect across the cutover.
     * Returns false on a DB error.
     */
    using Visitor = std::function<bool(uint32_t height, const uint8_t* nullifier_32)>;
    bool ForEach(const Visitor& visit) const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace dinero::consensus::shielded
