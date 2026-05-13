#pragma once
/**
 * V7 P2MR (Pay-to-Merkle-Root) consensus primitives.
 *
 * Spec: docs/consensus/V7_GENESIS_SPEC.md § "Consensus Rules At Block 0".
 *
 * A P2MR output is a 34-byte scriptPubKey:
 *
 *     0x53  0x20  <32-byte Merkle root>
 *     ^^^   ^^^   ^^^^^^^^^^^^^^^^^^^^^^^
 *     OP_3  PUSH32  commitment
 *
 * where the Merkle root commits to one or more PQ public keys under a
 * tree of depth 0..8. At v7 genesis the default is depth-0 (single-leaf),
 * where the root is simply `SHA256(scheme_id || pubkey)`.
 *
 * The spend witness is a canonical structured payload (see WitnessFormat
 * below). The consensus verifier:
 *
 *   1. Recognize the 34-byte output shape (IsP2MRScript).
 *   2. Deserialize the witness; reject any malformed layout.
 *   3. Gate on PQSchemeRegistry — scheme_id MUST be ACCEPT at this height.
 *   4. Verify the PQ signature over the sighash using the revealed pubkey.
 *   5. Hash the leaf up the Merkle path using the revealed siblings +
 *      leaf_index; reject if the recomputed root != the scriptPubKey's
 *      commitment.
 *
 * The five steps are implemented as separate pure functions so each can
 * be unit-tested in isolation. VerifyP2MRSpend() is the end-to-end entry
 * point a future v7 block validator will call.
 *
 * This header is consensus-critical. Keep it narrow.
 */

#include "consensus/pq/scheme_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dinero::consensus::pq {

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

/** Length in bytes of a P2MR output script: 0x53, 0x20, 32-byte root. */
constexpr std::size_t P2MR_SCRIPT_BYTES    = 34;

/** Leading opcodes of a P2MR script. */
constexpr uint8_t     P2MR_OP_WITNESS_V3   = 0x53;  ///< OP_3
constexpr uint8_t     P2MR_OP_PUSH32       = 0x20;  ///< push the next 32 bytes

/** Merkle-root byte length, same as the witness-program payload. */
constexpr std::size_t P2MR_ROOT_BYTES      = 32;

/** Maximum Merkle tree depth v7 genesis permits. Caps pubkey-set cardinality
 *  at 256 and prevents DoS via deep proofs. */
constexpr uint8_t     P2MR_MAX_MERKLE_DEPTH = 8;

// -----------------------------------------------------------------------
// Script recognition
// -----------------------------------------------------------------------

/**
 * Returns true iff `script` is a well-formed P2MR scriptPubKey:
 *     34 bytes, starts 0x53 0x20, followed by any 32 bytes.
 *
 * Does NOT validate the Merkle root bytes (they're opaque commitment data).
 * Does NOT check activation height; that's a separate consensus gate.
 */
bool IsP2MRScript(const std::vector<uint8_t>& script) noexcept;
bool IsP2MRScript(const uint8_t* script, std::size_t len) noexcept;

/**
 * Extract the 32-byte Merkle root from a valid P2MR scriptPubKey. Returns
 * nullopt if the script is not a valid P2MR shape.
 */
std::optional<std::array<uint8_t, P2MR_ROOT_BYTES>>
ExtractP2MRMerkleRoot(const std::vector<uint8_t>& script) noexcept;

// -----------------------------------------------------------------------
// Witness wire format + codec
// -----------------------------------------------------------------------

/**
 * Canonical P2MR spend witness layout:
 *
 *     [1 byte]    scheme_id
 *     [varint]    pubkey_len
 *     [N bytes]   pubkey_bytes   (N = pubkey_len; must match registry max)
 *     [varint]    signature_len
 *     [M bytes]   signature_bytes (M = signature_len; must match registry max)
 *     [1 byte]    merkle_depth   (0..8 inclusive)
 *     [depth*32]  sibling_hashes (empty when depth == 0)
 *     [varint]    leaf_index     (< 2^depth; always 0 when depth == 0)
 *
 * Varints use Bitcoin's standard CompactSize encoding (1, 3, 5, or 9 bytes
 * depending on magnitude). We restrict accepted magnitudes to values that
 * fit the layout, so practically most varints here are 1-3 bytes.
 *
 * The entire witness blob is byte-for-byte deterministic — different
 * signers with the same scheme/pubkey/sig/path MUST produce identical
 * bytes. This matters for mempool dedup, block template reproducibility,
 * and test vectors.
 */
struct P2MRWitness {
    uint8_t                                    scheme_id;
    std::vector<uint8_t>                       pubkey_bytes;
    std::vector<uint8_t>                       signature_bytes;
    uint8_t                                    merkle_depth;
    std::vector<std::array<uint8_t, 32>>       sibling_hashes;   // size == merkle_depth
    uint64_t                                   leaf_index;
};

/**
 * Serialize a P2MRWitness to its canonical byte representation.
 *
 * Returns the bytes on success. Returns an empty vector if the input is
 * invalid (e.g. sibling_hashes.size() != merkle_depth, or
 * merkle_depth > P2MR_MAX_MERKLE_DEPTH, or leaf_index out of range).
 */
std::vector<uint8_t> SerializeP2MRWitness(const P2MRWitness& w);

enum class P2MRWitnessDecodeError : uint8_t {
    Ok                      = 0,
    Truncated               = 1,   ///< ran out of bytes mid-parse
    PubkeyLenInvalid        = 2,   ///< not exactly the scheme's fixed pubkey length
    SignatureLenInvalid     = 3,   ///< not exactly the scheme's fixed signature length
    MerkleDepthTooDeep      = 4,   ///< > P2MR_MAX_MERKLE_DEPTH
    LeafIndexOutOfRange     = 5,   ///< >= 2^merkle_depth
    TrailingBytes           = 6,   ///< extra bytes after the last field
    UnknownScheme           = 7,   ///< scheme_id not in PQSchemeRegistry
    VarintOverflow          = 8,   ///< varint decodes to value > uint64 sanity
    /**
     * Structurally well-formed witness that does NOT re-encode to its
     * own input bytes. Catches non-minimal CompactSize varints, hidden
     * bit-flips in padding, or any future parser slack the per-field
     * bounds checks above let through. Enforces "one valid spend =
     * one valid byte encoding".
     */
    NonCanonical            = 9,
};

/**
 * Deserialize a P2MRWitness from bytes. Does NOT check registry activation
 * state — it only validates structural shape and per-scheme length bounds.
 * Use IsSchemeAcceptedAtHeight() separately to apply height-based gating.
 *
 * On success: `*out` is populated, returns P2MRWitnessDecodeError::Ok.
 * On failure: `*out` is left unchanged; returns the specific error code.
 */
P2MRWitnessDecodeError DeserializeP2MRWitness(const uint8_t* bytes,
                                              std::size_t    len,
                                              P2MRWitness*   out);

inline P2MRWitnessDecodeError DeserializeP2MRWitness(const std::vector<uint8_t>& bytes,
                                                     P2MRWitness* out) {
    return DeserializeP2MRWitness(bytes.data(), bytes.size(), out);
}

// -----------------------------------------------------------------------
// Merkle path verification
// -----------------------------------------------------------------------

/**
 * Compute the Merkle root implied by the given leaf + path.
 *
 *     leaf_hash_0 = leaf_hash  (caller-provided; typically SHA256(scheme_id || pubkey))
 *     for i in 0..depth-1:
 *         if bit i of leaf_index == 0:
 *             leaf_hash_{i+1} = SHA256(leaf_hash_i || sibling_i)
 *         else:
 *             leaf_hash_{i+1} = SHA256(sibling_i || leaf_hash_i)
 *     return leaf_hash_depth
 *
 * Returns nullopt if the inputs are out-of-bounds
 * (depth > P2MR_MAX_MERKLE_DEPTH, sibling_hashes.size() != depth,
 * leaf_index >= 2^depth). Caller compares the returned root against the
 * scriptPubKey's commitment.
 */
std::optional<std::array<uint8_t, 32>>
ComputeMerkleRoot(const std::array<uint8_t, 32>&             leaf_hash,
                  uint8_t                                    depth,
                  const std::vector<std::array<uint8_t, 32>>& sibling_hashes,
                  uint64_t                                   leaf_index);

/**
 * Compute a leaf hash: SHA256(scheme_id || pubkey_bytes).
 *
 * Caller is responsible for passing the registry-correct pubkey length;
 * this function does not validate.
 */
std::array<uint8_t, 32> ComputeP2MRLeafHash(uint8_t              scheme_id,
                                            const uint8_t*       pubkey_bytes,
                                            std::size_t          pubkey_len);

// -----------------------------------------------------------------------
// End-to-end spend verifier
// -----------------------------------------------------------------------

enum class P2MRVerifyError : uint8_t {
    Ok                     = 0,
    BadScriptShape         = 1,  ///< scriptPubKey is not a valid 34-byte P2MR
    WitnessDecodeFailed    = 2,  ///< DeserializeP2MRWitness returned non-Ok
    SchemeNotAcceptedHere  = 3,  ///< registry gate rejects scheme_id at height
    SignatureInvalid       = 4,  ///< ML-DSA verify returned false
    MerklePathMismatch     = 5,  ///< recomputed root != scriptPubKey commitment
    InternalError          = 6,  ///< unexpected internal failure
};

/**
 * End-to-end: verify a spend witness against a P2MR scriptPubKey.
 *
 * Inputs:
 *   scriptPubKey  — 34-byte P2MR script from the consumed UTXO.
 *   witness_bytes — the spender's signed witness payload.
 *   sighash       — 32-byte BIP341-style sighash the signature signs over.
 *   height        — height of the block being validated (for registry gate).
 *
 * Returns P2MRVerifyError::Ok on successful verification, or a specific
 * error code otherwise. No exception is thrown.
 *
 * This function is pure in the inputs — same inputs → same decision on
 * every host, every architecture.
 */
P2MRVerifyError VerifyP2MRSpend(const std::vector<uint8_t>&   script_pubkey,
                                const std::vector<uint8_t>&   witness_bytes,
                                const std::array<uint8_t, 32>& sighash,
                                uint32_t                       height);

} // namespace dinero::consensus::pq

// -----------------------------------------------------------------------
// Verification Weight Units (VWU) — the economic-truth fee metric.
//
// Lives in dinero::consensus (not ::pq) because VWU is a tx-level metric
// that applies to every tx, not just P2MR spends. The PQ-specific bit
// is just the per-input surcharge looked up from the scheme registry.
// -----------------------------------------------------------------------

namespace dinero {
struct Transaction;
namespace consensus {
struct UTXOEntry;

/**
 * Compute Verification Weight Units for a transaction.
 *
 * Formula (cross-node deterministic):
 *
 *     VWU(tx) = stripped_size(tx)
 *             + Σ_over_inputs ( witness_byte_weight_i × witness_bytes_i
 *                             + verify_cost_weight_i )
 *
 * For a P2MR input, `witness_byte_weight_i` and `verify_cost_weight_i`
 * are the scheme registry values keyed by the witness blob's scheme_id
 * byte (dinero::consensus::pq::GetSchemeParams).
 *
 * For any other input (P2PKH, P2TR, P2WPKH, ring, or unknown), the
 * implicit values are witness_byte_weight = 1 and verify_cost_weight = 0 —
 * i.e. each legacy witness byte contributes 1 VWU and there is no flat
 * per-input verify surcharge. This intentionally DROPS the BIP141 4×
 * witness discount; v7 is a fresh-genesis chain and the discount only
 * made sense as a backwards-compatibility subsidy on Bitcoin.
 *
 * Caveats:
 *   - Coinbase inputs contribute zero input-side cost (no prevout, no
 *     sig to verify).
 *   - If a P2MR input's witness blob fails to decode, the function
 *     returns std::nullopt. In consensus paths this maps to "reject
 *     block"; in mempool paths callers should have run witness
 *     validation first so this never triggers.
 *
 * `prevouts` MUST be the same length as `tx.vin` (one UTXOEntry per
 * input, matching position). Coinbase txs may pass empty prevouts.
 */
std::optional<uint64_t>
ComputeVWU(const dinero::Transaction& tx,
           const std::vector<UTXOEntry>& prevouts);

}  // namespace consensus
}  // namespace dinero
