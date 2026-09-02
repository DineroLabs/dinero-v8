#pragma once
/**
 * Wallet-side shielded note store — tracks owned notes in the
 * commitment tree so the wallet can spend them later.
 *
 * Each row represents a note the wallet can spend:
 *   - secret_key: spending authority (encrypted at rest)
 *   - value: note amount in una
 *   - randomness: per-note randomness used in commitment
 *   - leaf_index: position in the commitment tree
 *   - commitment: the note commitment (for quick lookup)
 *   - spent: whether a nullifier has been published for this note
 *
 * This store is wallet-layer only. Nothing here is consensus-critical.
 * Consensus validates proofs; this store just remembers which notes
 * belong to this wallet.
 */

#include "consensus/shielded/commitment_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace dinero::wallet {

/// Which convention binds a note's committed key — fixed when the note is
/// CREATED, not when it is spent, so it must be persisted per note.
///
/// A note can only be spent under the circuit matching its scheme, so the
/// spend path MUST refuse a mismatch loudly rather than build a proof that
/// consensus will reject. Post-activation there is no mixed state in practice:
/// the paired epoch reset makes every pre-activation note unspendable, so every
/// live note is Auth. The field exists so that invariant is checked rather than
/// assumed.
enum class NoteKeyScheme : uint8_t {
    /// pk = Poseidon(sk, 0), where the SENDER invented `sk`. The sender retains
    /// the ability to spend the note they sent.
    LegacySenderKey = 0,
    /// pk_d = s·G, where `s` derives from the RECIPIENT's ivk. Only the
    /// recipient can spend.
    Auth = 1,
};

struct ShieldedNote {
    int64_t  id = 0;
    uint64_t value_una = 0;
    consensus::shielded::Hash secret_key;
    consensus::shielded::Hash public_key;
    consensus::shielded::Hash randomness;
    /// Packed 11-byte diversifier, zero-padded to 32 bytes. Required by the
    /// note-commitment spend witness and therefore persisted with auth notes.
    consensus::shielded::Hash d{};
    consensus::shielded::Hash commitment;
    uint64_t leaf_index = 0;
    consensus::shielded::Hash nullifier;
    bool     confirmed = false;
    bool     spent = false;
    uint32_t created_height = 0;
    uint32_t confirmed_height = 0;
    uint32_t spent_height = 0;
    /// Defaults to LegacySenderKey so rows written by older daemons (and the
    /// ADD COLUMN default) read back as legacy, which is what they are.
    NoteKeyScheme key_scheme = NoteKeyScheme::LegacySenderKey;
};

class ShieldedNoteStore {
public:
    enum class OpenResult : uint8_t { Ok = 0, IoError = 1, SchemaError = 2 };

    ShieldedNoteStore() = default;
    ~ShieldedNoteStore();
    ShieldedNoteStore(const ShieldedNoteStore&) = delete;
    ShieldedNoteStore& operator=(const ShieldedNoteStore&) = delete;

    OpenResult Open(const std::string& path);
    void Close() noexcept;

    /** Store a newly created shielded note (from shield operation). */
    bool AddNote(uint64_t value_una,
                 const consensus::shielded::Hash& secret_key,
                 const consensus::shielded::Hash& public_key,
                 const consensus::shielded::Hash& randomness,
                 const consensus::shielded::Hash& commitment,
                 uint64_t leaf_index,
                 uint32_t created_height);

    /** Store a locally-created note that is not yet confirmed on-chain. */
    bool AddPendingNote(uint64_t value_una,
                        const consensus::shielded::Hash& secret_key,
                        const consensus::shielded::Hash& public_key,
                        const consensus::shielded::Hash& randomness,
                        const consensus::shielded::Hash& commitment,
                        uint32_t created_height,
                        NoteKeyScheme key_scheme =
                            NoteKeyScheme::LegacySenderKey,
                        const consensus::shielded::Hash& diversifier = {});

    /** Mark a note as spent (nullifier published). */
    bool MarkSpent(uint64_t leaf_index);
    bool MarkSpentByNullifier(const consensus::shielded::Hash& nullifier,
                              uint32_t spent_height);
    bool UnmarkSpentByNullifier(const consensus::shielded::Hash& nullifier);

    /** Atomically undo wallet mutations for a transaction that was not
     *  accepted by the mempool. Only pending spends (spent_height = 0) and
     *  unconfirmed notes are affected; confirmed chain state is never
     *  removed or unspent. The operation is idempotent so it can also clean
     *  up a partially-persisted attach operation. */
    bool RollbackPendingTransaction(
        const std::vector<consensus::shielded::Hash>& spend_nullifiers,
        const std::vector<consensus::shielded::Hash>& pending_commitments);

    /** Roll back all pending-spent rows (`spent=1 AND spent_height=0`).
     *  Called at wallet runtime startup so notes whose unshield tx never
     *  mined are returned to the unspent set. The mempool's nullifier-
     *  conflict check (mempool.cpp `Shielded nullifier conflict with
     *  mempool transaction <txid>`) is the safety net against a
     *  concurrent re-spend if the original tx is still in mempool. */
    int UnmarkAllPendingSpent();

    /** Confirm a previously-created note once its on-chain leaf index is known. */
    bool ConfirmNote(const consensus::shielded::Hash& commitment,
                     uint64_t leaf_index,
                     uint32_t confirmed_height);

    /** Revert a note back to pending/unconfirmed during a disconnect. */
    bool UnconfirmNote(const consensus::shielded::Hash& commitment);

    /** Get all unspent notes (for coin selection in private transfers). */
    std::vector<ShieldedNote> ListUnspent() const;
    std::vector<ShieldedNote> ListAll() const;

    /** Get a specific note by leaf index. */
    std::optional<ShieldedNote> GetByLeafIndex(uint64_t leaf_index) const;

    /** Total unspent shielded balance. */
    uint64_t GetBalance() const;

    /** Wallet-side copy of all shielded chain leaves, in leaf-index order. */
    bool AppendChainLeaf(const consensus::shielded::Hash& commitment,
                         uint64_t leaf_index,
                         uint32_t created_height);
    bool TruncateChainLeaves(uint64_t new_size);
    std::vector<consensus::shielded::Hash> LoadChainLeaves() const;
    uint64_t GetChainLeafCount() const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace dinero::wallet
