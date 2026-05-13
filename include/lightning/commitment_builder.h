#pragma once

#include "lightning/lightning_types.h"
#include <vector>
#include <cstdint>
#include <string>

// Forward declare secp256k1_context_struct
typedef struct secp256k1_context_struct secp256k1_context;

namespace dinero {
    // Forward declarations
    struct Transaction;
}

namespace dinero {
namespace lightning {

/**
 * @class CommitmentBuilder
 * @brief Builds and validates Lightning commitment transactions
 *
 * Phase 7.3: Taproot-based commitment transaction construction
 *
 * Responsibilities:
 * - Build commitment transactions with Taproot outputs
 * - Implement MuSig2 aggregated signatures for funding outputs
 * - Create script-path spending conditions (revocation, timelock)
 * - Sign commitment transactions with BIP340 Schnorr signatures
 * - Validate commitment transaction structure
 *
 * Commitment Transaction Structure (Taproot):
 *
 * Input:
 *   - Funding output (2-of-2 MuSig2 aggregated key)
 *
 * Outputs:
 *   - Output 0: to_local (our balance, Taproot with script-path)
 *       - Key-path: time-locked (CSV delay) direct spend
 *       - Script-path: revocation branch (peer can claim if we broadcast revoked)
 *   - Output 1: to_remote (peer's balance, simple key-path spend)
 *   - Output 2+: HTLC outputs (one per pending payment)
 *
 * Thread Safety: All methods are thread-safe
 */
class CommitmentBuilder {
public:
    /**
     * @brief Construct CommitmentBuilder
     */
    CommitmentBuilder();
    ~CommitmentBuilder();

    // ═══════════════════════════════════════════════════════════════════════════
    // Commitment Transaction Construction
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Build commitment transaction for local node
     *
     * Creates a commitment transaction spending the funding output and
     * distributing balances according to the current channel state.
     *
     * @param channel Channel to build commitment for
     * @param local_privkey Our private key (32 bytes)
     * @param remote_pubkey Peer's public key (33 bytes compressed)
     * @return Result<Transaction> Commitment transaction or error
     */
    Result<Transaction> buildCommitmentTransaction(
        const Channel& channel,
        const std::vector<uint8_t>& local_privkey,
        const std::vector<uint8_t>& remote_pubkey
    );

    /**
     * @brief Build to_local output (our balance with revocation)
     *
     * Creates a Taproot output with:
     * - Key-path: time-locked spend (CSV delay)
     * - Script-path: revocation branch (peer can claim if we cheat)
     *
     * Taproot script tree:
     *   Internal key: tweaked pubkey (time-lock branch)
     *   Script leaf: OP_IF <revocation_pubkey> OP_ELSE <to_self_delay> OP_CSV OP_ENDIF
     *
     * @param local_pubkey Our public key (33 bytes)
     * @param revocation_pubkey Revocation public key (33 bytes)
     * @param to_self_delay CSV delay in blocks
     * @param amount_sats Output amount
     * @return Result<std::vector<uint8_t>> Taproot scriptPubKey or error
     */
    Result<std::vector<uint8_t>> buildToLocalOutput(
        const std::vector<uint8_t>& local_pubkey,
        const std::vector<uint8_t>& revocation_pubkey,
        uint32_t to_self_delay,
        uint64_t amount_sats
    );

    /**
     * @brief Build to_remote output (peer's balance, simple key-path)
     *
     * Creates a simple Taproot output that the peer can spend immediately.
     *
     * @param remote_pubkey Peer's public key (33 bytes)
     * @param amount_sats Output amount
     * @return Result<std::vector<uint8_t>> Taproot scriptPubKey or error
     */
    Result<std::vector<uint8_t>> buildToRemoteOutput(
        const std::vector<uint8_t>& remote_pubkey,
        uint64_t amount_sats
    );

    /**
     * @brief Build HTLC output
     *
     * Creates a Taproot output for an HTLC with:
     * - Success path: Reveal preimage to claim (peer for incoming, us for outgoing)
     * - Timeout path: Claim after CLTV expiry (us for incoming, peer for outgoing)
     *
     * @param htlc HTLC to create output for
     * @param local_pubkey Our public key
     * @param remote_pubkey Peer's public key
     * @return Result<std::vector<uint8_t>> Taproot scriptPubKey or error
     */
    Result<std::vector<uint8_t>> buildHTLCOutput(
        const HTLC& htlc,
        const std::vector<uint8_t>& local_pubkey,
        const std::vector<uint8_t>& remote_pubkey
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // MuSig2 Aggregated Signatures
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Create MuSig2 aggregated public key for funding output
     *
     * Combines local and remote public keys into a single aggregated key
     * using MuSig2 protocol (BIP327).
     *
     * @param local_pubkey Our public key (33 bytes compressed)
     * @param remote_pubkey Peer's public key (33 bytes compressed)
     * @return Result<std::vector<uint8_t>> Aggregated x-only pubkey (32 bytes) or error
     */
    Result<std::vector<uint8_t>> createMuSig2AggregateKey(
        const std::vector<uint8_t>& local_pubkey,
        const std::vector<uint8_t>& remote_pubkey
    );

    /**
     * @brief Sign commitment transaction with MuSig2
     *
     * Creates a partial signature for the commitment transaction input
     * (spending the funding output).
     *
     * @param tx Transaction to sign
     * @param input_index Input index to sign
     * @param local_privkey Our private key (32 bytes)
     * @param remote_pubkey Peer's public key (33 bytes)
     * @param funding_amount_sats Funding output amount
     * @return Result<std::vector<uint8_t>> Partial signature (32 bytes) or error
     */
    Result<std::vector<uint8_t>> signCommitmentWithMuSig2(
        const Transaction& tx,
        uint32_t input_index,
        const std::vector<uint8_t>& local_privkey,
        const std::vector<uint8_t>& remote_pubkey,
        uint64_t funding_amount_sats
    );

    /**
     * @brief Aggregate MuSig2 partial signatures
     *
     * Combines our partial signature with peer's partial signature to create
     * a complete Schnorr signature for the funding input.
     *
     * @param local_partial_sig Our partial signature (32 bytes)
     * @param remote_partial_sig Peer's partial signature (32 bytes)
     * @return Result<std::vector<uint8_t>> Complete signature (64 bytes) or error
     */
    Result<std::vector<uint8_t>> aggregateMuSig2Signatures(
        const std::vector<uint8_t>& local_partial_sig,
        const std::vector<uint8_t>& remote_partial_sig
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Taproot Script Construction
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Build revocation script
     *
     * Script: OP_IF <revocation_pubkey> OP_CHECKSIG OP_ELSE <to_self_delay> OP_CSV OP_DROP <delayed_pubkey> OP_CHECKSIG OP_ENDIF
     *
     * @param revocation_pubkey Revocation public key (33 bytes)
     * @param delayed_pubkey Time-locked public key (33 bytes)
     * @param to_self_delay CSV delay in blocks
     * @return std::vector<uint8_t> Script bytes
     */
    std::vector<uint8_t> buildRevocationScript(
        const std::vector<uint8_t>& revocation_pubkey,
        const std::vector<uint8_t>& delayed_pubkey,
        uint32_t to_self_delay
    );

    /**
     * @brief Build HTLC success script
     *
     * Script: OP_HASH256 <payment_hash> OP_EQUALVERIFY <remote_pubkey> OP_CHECKSIG
     *
     * @param payment_hash SHA256 hash of preimage (32 bytes)
     * @param pubkey Public key that can claim with preimage
     * @return std::vector<uint8_t> Script bytes
     */
    std::vector<uint8_t> buildHTLCSuccessScript(
        const std::vector<uint8_t>& payment_hash,
        const std::vector<uint8_t>& pubkey
    );

    /**
     * @brief Build HTLC timeout script
     *
     * Script: <cltv_expiry> OP_CHECKLOCKTIMEVERIFY OP_DROP <local_pubkey> OP_CHECKSIG
     *
     * @param cltv_expiry Absolute block height for timeout
     * @param pubkey Public key that can claim after timeout
     * @return std::vector<uint8_t> Script bytes
     */
    std::vector<uint8_t> buildHTLCTimeoutScript(
        uint32_t cltv_expiry,
        const std::vector<uint8_t>& pubkey
    );

    /**
     * @brief Compute Taproot tweak for script tree
     *
     * Tweaks an internal public key with a Taproot script tree.
     *
     * @param internal_pubkey Internal public key (32 bytes x-only)
     * @param script_tree Taproot script tree
     * @return Result<std::vector<uint8_t>> Tweaked pubkey (32 bytes x-only) or error
     */
    Result<std::vector<uint8_t>> computeTaprootTweak(
        const std::vector<uint8_t>& internal_pubkey,
        const std::vector<std::vector<uint8_t>>& script_tree
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Validation
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Validate commitment transaction structure
     *
     * Checks:
     * - Correct number of outputs
     * - Output amounts match channel balances
     * - Taproot outputs are well-formed
     * - Transaction size is reasonable
     *
     * @param tx Commitment transaction to validate
     * @param channel Channel state
     * @return Result<void> Success or error with reason
     */
    Result<void> validateCommitmentTransaction(
        const Transaction& tx,
        const Channel& channel
    );

    /**
     * @brief Verify commitment transaction signature
     *
     * @param tx Signed commitment transaction
     * @param signature Schnorr signature (64 or 65 bytes)
     * @param pubkey Public key to verify against (32 bytes x-only)
     * @param funding_value Value of the funding output in una
     * @param funding_script_pubkey ScriptPubKey of the funding output
     * @return bool True if signature is valid
     */
    bool verifyCommitmentSignature(
        const Transaction& tx,
        const std::vector<uint8_t>& signature,
        const std::vector<uint8_t>& pubkey,
        uint64_t funding_value,
        const std::vector<uint8_t>& funding_script_pubkey
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Revocation Keys
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Derive revocation public key for commitment
     *
     * Uses per-commitment secrets to derive unique revocation keys.
     *
     * @param revocation_basepoint Revocation base point (33 bytes)
     * @param per_commitment_point Per-commitment point (33 bytes)
     * @return Result<std::vector<uint8_t>> Revocation pubkey (33 bytes) or error
     */
    Result<std::vector<uint8_t>> deriveRevocationPubkey(
        const std::vector<uint8_t>& revocation_basepoint,
        const std::vector<uint8_t>& per_commitment_point
    );

    /**
     * @brief Derive revocation private key (for breach remedy)
     *
     * Used to spend a revoked commitment transaction output.
     *
     * @param revocation_basepoint_secret Revocation base secret (32 bytes)
     * @param per_commitment_secret Per-commitment secret (32 bytes)
     * @return Result<std::vector<uint8_t>> Revocation private key (32 bytes) or error
     */
    Result<std::vector<uint8_t>> deriveRevocationPrivkey(
        const std::vector<uint8_t>& revocation_basepoint_secret,
        const std::vector<uint8_t>& per_commitment_secret
    );

private:
    // Secp256k1 context for cryptographic operations
    secp256k1_context* m_secp_ctx;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Compute SHA256 hash
     * @param data Input data
     * @return std::vector<uint8_t> 32-byte hash
     */
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

    /**
     * @brief Compute double SHA256 hash
     * @param data Input data
     * @return std::vector<uint8_t> 32-byte double hash
     */
    std::vector<uint8_t> doubleSHA256(const std::vector<uint8_t>& data);

    /**
     * @brief Compute tagged hash (BIP340)
     * @param tag Hash tag (e.g., "TapTweak", "TapLeaf")
     * @param data Input data
     * @return std::vector<uint8_t> 32-byte tagged hash
     */
    std::vector<uint8_t> taggedHash(const std::string& tag, const std::vector<uint8_t>& data);

    /**
     * @brief Convert compressed pubkey to x-only
     * @param compressed_pubkey 33-byte compressed pubkey
     * @return std::vector<uint8_t> 32-byte x-only pubkey
     */
    std::vector<uint8_t> toXOnly(const std::vector<uint8_t>& compressed_pubkey);

    /**
     * @brief Serialize script for Taproot leaf hash
     * @param script Script bytes
     * @return std::vector<uint8_t> Serialized script
     */
    std::vector<uint8_t> serializeScriptForTapLeaf(const std::vector<uint8_t>& script);
};

} // namespace lightning
} // namespace dinero
