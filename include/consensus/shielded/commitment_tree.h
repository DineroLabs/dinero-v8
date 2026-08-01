#pragma once
/**
 * Shielded Pool Commitment Tree — Poseidon-based incremental Merkle tree.
 *
 * This tree is the PRIVATE state. It holds note commitments for shielded
 * outputs. It is consensus-critical but structurally isolated from
 * Utreexo — no leaf from this tree ever enters the Utreexo accumulator.
 *
 * Boundary rule (non-negotiable):
 *   - Shield:   Utreexo removes transparent UTXO → this tree adds commitment
 *   - Transfer: This tree only — add commitment, publish nullifier
 *   - Unshield: Publish nullifier → Utreexo adds new transparent UTXO
 *
 * Tree structure:
 *   - Fixed depth (32 levels → 2^32 capacity)
 *   - Incremental (append-only, no deletions — nullifiers handle spending)
 *   - Poseidon hash for ZK-friendliness (low R1CS constraint count)
 *   - Each leaf is the address-bound note commitment defined below
 *
 * The tree is deterministic consensus state derived from validated bundle
 * bytes. BlockHeader v1 does not carry a separate shielded root (its reserved
 * bytes remain zero). Current mainnet blocks indirectly commit v5 bundle bytes
 * through the coinbase DINW witness commitment; v6 also commits them in txid.
 * Validators must maintain the tree to verify later spend anchors.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace dinero::consensus::shielded {

constexpr size_t TREE_DEPTH = 32;
constexpr size_t HASH_BYTES = 32;

using Hash = std::array<uint8_t, HASH_BYTES>;

/**
 * Compute Poseidon hash of two 32-byte inputs (tree node merge).
 * This MUST match the Poseidon gadget in the ZK circuit — if these
 * diverge, proofs generated off-chain won't verify on-chain.
 */
Hash PoseidonHash2(const Hash& left, const Hash& right);

/**
 * Address-binding tag scalar: Poseidon-hash domain separator forcing
 * every valid commitment to incorporate (d, pk_note) under a fixed DST.
 * Phase 2 wave 5 — see spec §6.2.
 *
 * Format: ASCII "DIN/v7/shielded/addr/v1" right-padded to 32 bytes
 * with zeros, interpreted big-endian as a secp256k1 scalar (matches
 * Scalar(const uint8_t*) construction).
 */
const Hash& AddrBindTag();

/**
 * Compute a note commitment with address binding (Phase 2 wave 5).
 *
 *   addr_bind = Poseidon(AddrBindTag, Poseidon(d, pk_note))
 *   inner     = Poseidon(addr_bind, value)
 *   cm        = Poseidon(inner, randomness)
 *
 * `pk_note` is derived from the encrypted note's rcm. The diversified address
 * key pk_d transports that plaintext; it is not itself a circuit input.
 */
Hash NoteCommitment(const Hash& d,
                    const Hash& recipient_pk,
                    const Hash& value_commitment,
                    const Hash& randomness);

/**
 * Compute the nullifier for a note: Poseidon(secret_key, leaf_index).
 *
 * Deterministic: one note → one nullifier. Publishing the nullifier
 * marks the note as spent without revealing which commitment it
 * corresponds to (the secret_key is known only to the owner).
 */
Hash ComputeNullifier(const Hash& secret_key, uint64_t leaf_index);

/**
 * Incremental Merkle tree — append-only, Poseidon-hashed.
 *
 * The tree maintains the authentication path for the next insertion
 * point, enabling O(depth) appends without storing the full tree.
 * Only the frontier (one hash per level) is kept in memory.
 *
 * For proof generation, the wallet maintains a full copy of the tree
 * (or a witness for each owned note). The consensus layer only needs
 * the root — which is stored in the block header.
 */
class CommitmentTree {
public:
    CommitmentTree();
    CommitmentTree(const CommitmentTree& other);
    CommitmentTree& operator=(const CommitmentTree& other);
    CommitmentTree(CommitmentTree&& other) noexcept;
    CommitmentTree& operator=(CommitmentTree&& other) noexcept;

    /** Append a new leaf commitment. Returns the leaf index (0-based). */
    uint64_t Append(const Hash& commitment);

    /** Current Merkle root. */
    Hash Root() const;

    /** Number of leaves inserted so far. */
    uint64_t Size() const { return size_; }

    /** Get the authentication path for a leaf at `index`. Only valid if
     *  the caller has stored the full tree (wallet-side, not consensus). */
    struct AuthPath {
        std::array<Hash, TREE_DEPTH> siblings;
        uint64_t leaf_index;
    };

    /**
     * Build the authentication path for a leaf if the tree has the full
     * leaf set in memory (wallet-side use). Trees restored only from the
     * compact frontier cannot answer this.
     */
    std::optional<AuthPath> GetAuthPath(uint64_t index) const;

    /** Wallet-side reorg helper: truncate the leaf set back to `new_size`. */
    bool Truncate(uint64_t new_size);

    /**
     * Serialize the frontier (for persistence across restarts).
     * Format: size (8 bytes LE) + TREE_DEPTH * HASH_BYTES.
     */
    std::vector<uint8_t> SerializeFrontier() const;
    bool DeserializeFrontier(const uint8_t* data, size_t len);

private:
    mutable std::mutex mutex_;
    uint64_t size_ = 0;
    std::array<Hash, TREE_DEPTH> frontier_;  ///< One hash per level (left siblings)
    std::vector<Hash> leaves_;               ///< Full leaf set for wallet-side path building

    /** Empty subtree hashes — precomputed for each level. */
    static const std::array<Hash, TREE_DEPTH + 1>& EmptyRoots();
};

} // namespace dinero::consensus::shielded
