#pragma once

#include "wallet/wallet_manager.h"
#include "lightning/lightning_types.h"
#include "wallet/transaction.h"
#include "dinerod.pb.h"  // Phase 3 Commit 1: Use gRPC proto types (generated header)
#include <secp256k1.h>
#include <memory>
#include <vector>
#include <map>

// Forward declarations
struct DaemonContext;

// Phase 3 Commit 1: Use gRPC proto UTXO type (enforces gRPC boundary)
// WalletManager::UTXO doesn't exist - use dinerod::UTXO (proto type) instead
using UTXO = dinerod::UTXO;

namespace dinero {
namespace lightning {

// Forward declarations
class LightningService;

// ═══════════════════════════════════════════════════════════════════════════
// Supporting Types (defined before LightningWallet class)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @enum ChannelKeyType
 * @brief Types of keys used in Lightning channels (BOLT #3)
 */
enum class ChannelKeyType {
    FUNDING,            // For MuSig2 aggregate key in funding output
    REVOCATION_BASE,    // For penalty transactions
    PAYMENT_BASE,       // For HTLC outputs
    DELAYED_PAYMENT_BASE, // For to_self outputs with CSV delay
    HTLC_BASE          // For HTLC transaction signing
};

/**
 * @struct MuSig2NoncePair
 * @brief MuSig2 nonce pair for commitment signing
 */
struct MuSig2NoncePair {
    std::vector<uint8_t> secret_nonce;  // 32 bytes - MUST be kept secret
    std::vector<uint8_t> public_nonce;  // 66 bytes - sent to remote peer

    // Timestamp for nonce expiry (nonces should not be reused)
    uint64_t created_at;

    // Flag to prevent nonce reuse
    bool used;
};

/**
 * @class LightningWallet
 * @brief Bridge between WalletService and LightningService for Lightning operations
 *
 * Architecture:
 * - Accesses WalletManager for UTXO selection and transaction creation
 * - Integrates with LightningService for channel management
 * - Handles Lightning-specific key derivation
 * - Manages MuSig2 signing sessions
 *
 * Thread Safety: All public methods are thread-safe
 */
class LightningWallet {
public:
    /**
     * @brief Construct LightningWallet with daemon context
     * @param ctx DaemonContext containing wallet and lightning services
     */
    explicit LightningWallet(DaemonContext& ctx);
    ~LightningWallet();

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Funding Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Create Lightning funding transaction
     *
     * Creates a 2-of-2 MuSig2 funding output:
     * 1. Selects appropriate UTXOs from wallet
     * 2. Creates funding output with aggregate pubkey
     * 3. Adds change output if needed
     * 4. Signs transaction with wallet keys
     * 5. Returns unsigned funding transaction (needs counter-party signature)
     *
     * @param aggregate_pubkey 32-byte x-only MuSig2 aggregate key
     * @param amount_sats Channel funding amount in una
     * @param fee_rate Fee rate in una per vbyte
     * @return Result<Transaction> Funding transaction or error
     */
    Result<Transaction> createFundingTransaction(
        const std::vector<uint8_t>& aggregate_pubkey,
        uint64_t amount_sats,
        uint64_t fee_rate
    );

    /**
     * @brief Select UTXOs for channel funding
     *
     * Implements coin selection algorithm for Lightning:
     * - Prefers confirmed UTXOs (6+ confirmations)
     * - Avoids dust outputs
     * - Minimizes fee overhead
     * - Considers amount + estimated fees
     *
     * @param amount_sats Required amount (channel capacity)
     * @param fee_rate Fee rate in una per vbyte
     * @return Result<std::vector<UTXO>> Selected UTXOs or error
     */
    Result<std::vector<UTXO>> selectUTXOsForChannel(
        uint64_t amount_sats,
        uint64_t fee_rate
    );

    /**
     * @brief Broadcast funding transaction to network
     *
     * Submits transaction to mempool and waits for confirmation.
     *
     * @param tx Signed funding transaction
     * @return Result<std::string> Transaction ID or error
     */
    Result<std::string> broadcastFundingTransaction(const Transaction& tx);

    // ═══════════════════════════════════════════════════════════════════════════
    // MuSig2 Signing Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate MuSig2 nonce pair for commitment signing
     *
     * Creates secure nonce pair (secret nonce + public nonce).
     * The public nonce must be exchanged with the remote peer via BOLT #2.
     *
     * @param channel_id Channel identifier
     * @return Result<MuSig2NoncePair> Nonce pair or error
     */
    Result<MuSig2NoncePair> generateCommitmentNonce(
        const std::vector<uint8_t>& channel_id
    );

    /**
     * @brief Sign commitment transaction with MuSig2
     *
     * Creates partial signature for commitment transaction:
     * 1. Retrieves stored secret nonce for this channel
     * 2. Processes remote public nonce to create signing session
     * 3. Signs transaction input with secp256k1_musig_partial_sign()
     * 4. Returns partial signature to be sent to remote peer
     *
     * @param channel_id Channel identifier
     * @param tx Commitment transaction to sign
     * @param input_index Input index to sign (usually 0 - funding output)
     * @param remote_nonce Remote peer's public nonce (66 bytes)
     * @param sighash_type Signature hash type (default: SIGHASH_ALL)
     * @return Result<std::vector<uint8_t>> 32-byte partial signature or error
     */
    Result<std::vector<uint8_t>> signCommitmentWithMuSig2(
        const std::vector<uint8_t>& channel_id,
        const Transaction& tx,
        uint32_t input_index,
        const std::vector<uint8_t>& remote_nonce,
        uint32_t sighash_type = 0x01  // SIGHASH_ALL
    );

    /**
     * @brief Clear stored nonce for channel
     *
     * Securely wipes nonce from memory after signing.
     * MUST be called after receiving remote partial signature.
     *
     * @param channel_id Channel identifier
     * @return Result<void> Success or error
     */
    Result<void> clearCommitmentNonce(const std::vector<uint8_t>& channel_id);

    // ═══════════════════════════════════════════════════════════════════════════
    // Lightning Key Derivation
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Derive Lightning channel key
     *
     * Derives key for Lightning channel using custom derivation path:
     * m/purpose'/coin_type'/account'/change/index
     *
     * Lightning key types:
     * - Funding key: Used for MuSig2 aggregate key
     * - Revocation base: Used for penalty transactions
     * - Payment base: Used for HTLC outputs
     * - Delayed payment base: Used for to_self outputs
     *
     * @param key_type Type of key to derive
     * @param channel_index Channel index (0, 1, 2, ...)
     * @return Result<std::vector<uint8_t>> 32-byte private key or error
     */
    Result<std::vector<uint8_t>> deriveChannelKey(
        ChannelKeyType key_type,
        uint32_t channel_index
    );

    /**
     * @brief Get public key for Lightning channel
     *
     * Derives public key from private key.
     *
     * @param private_key 32-byte private key
     * @return Result<std::vector<uint8_t>> 33-byte compressed public key or error
     */
    Result<std::vector<uint8_t>> getPublicKey(const std::vector<uint8_t>& private_key);

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Closure Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Create cooperative close transaction
     *
     * Creates mutual close transaction splitting channel balance.
     *
     * @param channel_id Channel identifier
     * @param local_amount_sats Amount to local wallet
     * @param remote_amount_sats Amount to remote peer
     * @param fee_rate Fee rate in una per vbyte
     * @return Result<Transaction> Close transaction or error
     */
    Result<Transaction> createCooperativeCloseTransaction(
        const std::vector<uint8_t>& channel_id,
        uint64_t local_amount_sats,
        uint64_t remote_amount_sats,
        uint64_t fee_rate
    );

    /**
     * @brief Broadcast force-close commitment transaction
     *
     * Broadcasts latest commitment transaction for unilateral close.
     *
     * @param channel_id Channel identifier
     * @return Result<std::string> Transaction ID or error
     */
    Result<std::string> broadcastForceClose(const std::vector<uint8_t>& channel_id);

    // ═══════════════════════════════════════════════════════════════════════════
    // Balance Queries
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get available balance for Lightning channels
     *
     * Returns wallet balance excluding:
     * - Locked outputs in existing channels
     * - Reserved amounts for fees
     * - Dust outputs
     *
     * @param min_confirmations Minimum confirmations required (default: 6)
     * @return uint64_t Available balance in una
     */
    uint64_t getAvailableBalanceForLightning(int min_confirmations = 6) const;

    /**
     * @brief Get total locked balance in Lightning channels
     *
     * Sum of all channel funding outputs.
     *
     * @return uint64_t Locked balance in una
     */
    uint64_t getLockedBalanceInChannels() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;              // Daemon context with services
    WalletManager* m_wallet;                  // Wallet for UTXO management
    LightningService* m_lightning;            // Lightning service for channels

    // MuSig2 nonce storage (channel_id -> nonce pair)
    // NOTE: In production, this should be persisted to disk
    std::map<std::vector<uint8_t>, MuSig2NoncePair> m_nonce_storage;
    mutable std::mutex m_nonce_mutex;

    // secp256k1 context for cryptographic operations
    secp256k1_context* m_secp_ctx;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Estimate transaction size for fee calculation
     * @param num_inputs Number of inputs
     * @param num_outputs Number of outputs
     * @return size_t Estimated size in vbytes
     */
    size_t estimateTransactionSize(size_t num_inputs, size_t num_outputs) const;

    /**
     * @brief Validate aggregate pubkey format
     * @param aggregate_pubkey 32-byte x-only pubkey
     * @return bool True if valid
     */
    bool validateAggregatePubkey(const std::vector<uint8_t>& aggregate_pubkey) const;

    /**
     * @brief Generate secure random nonce
     * @return std::vector<uint8_t> 32-byte nonce
     */
    std::vector<uint8_t> generateSecureNonce() const;
};

} // namespace lightning
} // namespace dinero
