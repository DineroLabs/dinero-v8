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
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace dinero::consensus::shielded {

class NullifierSet {
public:
    enum class OpenResult : uint8_t {
        Ok            = 0,
        IoError       = 1,
        SchemaError   = 2,
        /**
         * The file opened and the schema is fine, but a query needed to decide
         * whether its rows are authoritative failed. Refusing to open is the
         * only safe answer: guessing either way is unrecoverable -- guess
         * "cache" and a stamp permanently demotes a real legacy set, guess
         * "legacy" and crash residue gets promoted to authoritative.
         */
        Indeterminate = 3,
    };

    NullifierSet() = default;
    ~NullifierSet();
    NullifierSet(const NullifierSet&) = delete;
    NullifierSet& operator=(const NullifierSet&) = delete;

    /**
     * Where this database's rows came from — the discriminator that decides
     * whether they may ever be treated as authoritative.
     *
     * "ChainDB has no nullifiers and sqlite has some" is NOT evidence of a
     * legacy database. The crash window produces exactly that state: the first
     * shielded block commits its nullifier batch to sqlite, dies before the
     * ChainDB write, and restarts with an empty ChainDB and a populated cache.
     * Promoting those rows would make an unconnected block's nullifiers
     * authoritative and permanently unspendable — a false-spend denial.
     *
     * Stored in sqlite's user_version pragma, which legacy files leave at 0.
     */
    enum class Provenance : uint8_t {
        Unknown        = 0,  ///< not opened
        FreshCache     = 1,  ///< created by this build; empty; never authoritative
        Cache          = 2,  ///< marked as cache-under-ChainDB-authority
        LegacyCandidate = 3, ///< pre-authority file WITH rows: migration eligible
    };

    /** Schema version written by builds that treat sqlite as a cache. */
    static constexpr int kCacheSchemaVersion = 1;

    /**
     * What Open() should conclude from the two facts it can observe.
     *
     * Separated from Open() so the rule is exhaustively testable without a
     * database, including the states that only occur under sqlite failure --
     * which is where this went wrong. `Indeterminate` exists because "we could
     * not read it" is a THIRD answer, and collapsing it into either of the
     * other two is what let a transient lock permanently demote a legacy
     * database. Same idiom as consensus/block_status_generation.h.
     */
    enum class OpenDecision : uint8_t {
        AlreadyCache    = 0,  ///< stamped: rows are cache, never authoritative
        LegacyCandidate = 1,  ///< unstamped AND populated: migration eligible
        StampFresh      = 2,  ///< unstamped AND empty: safe to stamp now
        Indeterminate   = 3,  ///< a read failed: decide NOTHING, stamp NOTHING
    };

    /**
     * The rule. `user_version` and `row_count` are nullopt when their query
     * failed, which must never be read as "0".
     */
    static OpenDecision DecideProvenance(std::optional<int> user_version,
                                         std::optional<uint64_t> row_count);

    /**
     * Row count, or nullopt if the query failed.
     *
     * Size() cannot express failure and answers 0 for both "empty" and
     * "SQLITE_BUSY". Any code whose DECISION depends on the count must use
     * this instead; a wrong 0 here selected the wrong migration mode and
     * stamped a populated legacy database as a cache.
     */
    std::optional<uint64_t> TryCount() const;

    /** user_version pragma, or nullopt if the query failed. */
    std::optional<int> TryReadUserVersion() const;

    /** Open or create the nullifier database at `path`. Idempotent. */
    OpenResult Open(const std::string& path);

    /** Provenance determined at Open time. */
    Provenance GetProvenance() const { return provenance_; }

    /**
     * Mark this database as a cache, permanently. Called after a legacy
     * migration completes so the same file can never be read as legacy again,
     * and on any file this build creates.
     */
    bool MarkAsCache();
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
     * Insert a whole batch inside ONE sqlite transaction: all rows commit, or
     * none do.
     *
     * Sequential Insert() calls cannot give that guarantee. A failure on the
     * Nth insert leaves the first N-1 rows written, so a caller that returns
     * "refused" still mutated durable state. The residue is fail-safe against
     * inflation -- a surplus nullifier can only refuse a spend -- but it can
     * reject a VALID spend until startup rebuilds the set from ChainDB, which
     * is a denial of service, not a harmless artefact.
     *
     * Returns false and rolls back if ANY entry fails, including a duplicate
     * inside the batch itself.
     */
    bool InsertBatch(const std::vector<std::pair<Hash, uint32_t>>& entries);

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
    /**
     * Row count, 0 on ANY failure. Retained for diagnostics and reporting.
     * Never branch on this: use TryCount(), which distinguishes an empty
     * database from an unreadable one.
     */
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
     *
     * Returns true ONLY if the whole set was enumerated: the scan reached
     * SQLITE_DONE, every row was well-formed, and the visitor never stopped
     * early. Any sqlite error mid-scan -- BUSY, IOERR, CORRUPT, INTERRUPT --
     * returns false with no partial result implied. Callers accumulating a
     * consensus digest depend on this: a truncated set reported as complete
     * is indistinguishable from every nullifier having been deleted.
     */
    using Visitor = std::function<bool(uint32_t height, const uint8_t* nullifier_32)>;
    bool ForEach(const Visitor& visit) const;

    /**
     * TEST-ONLY. Make the next step of an in-flight scan fail with a REAL
     * sqlite error (SQLITE_INTERRUPT) instead of a simulated one.
     *
     * The fail-closed enumeration contract can only be tested against a
     * mid-iteration failure, and no ordinary operation produces one on demand:
     * the happy path always ends in SQLITE_DONE, and a competing connection
     * cannot make THIS connection's reader fail deterministically. Calling
     * this from inside a ForEach visitor is the only intended use.
     *
     * No-op on a closed set. Never called by production code.
     */
    void InterruptForTesting();

private:
    /// One scalar read; nullopt unless sqlite positively returned a row.
    std::optional<int64_t> ScalarQuery(const char* sql) const;

    sqlite3* db_ = nullptr;
    Provenance provenance_ = Provenance::Unknown;
};

} // namespace dinero::consensus::shielded
