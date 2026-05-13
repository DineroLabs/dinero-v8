#pragma once

/**
 * @file transaction_signer.h
 * @brief Milestone 12.5 - Transaction Signing & Local Validation
 *
 * Single responsibility: Sign UnsignedTransaction → SignedTransaction
 *
 * Core Rule (Non-Negotiable):
 * Signing must NOT change transaction structure.
 * - Input/output order remains identical
 * - Fee does not change
 * - Change output does not change
 * - Only scriptSig/witness fields are populated
 *
 * This enables:
 * - PSBT support (structure matches)
 * - Hardware wallet integration (offline signing)
 * - Transaction batching (inputs fixed)
 */

#include "wallet/unsigned_tx_builder.h"
#include "wallet/transaction.h"
#include "wallet/hd_wallet.h"  // Phase M.3: For CanonicalWalletUTXO type
#include <array>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Key Provider Interface (Hardware Wallet Ready)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Key provider interface for signing
 *
 * Abstraction layer that enables:
 * - Local HD wallet
 * - Hardware wallet (Ledger, Trezor)
 * - Air-gapped signer
 * - Test mocks
 */
class KeyProvider {
public:
    virtual ~KeyProvider() = default;

    /**
     * @brief Get private key for address
     *
     * @param address Address to sign for
     * @return Private key bytes (32 bytes), empty if not found
     */
    virtual std::vector<uint8_t> GetPrivateKey(const std::string& address) const = 0;

    /**
     * @brief Check if key is available
     *
     * @param address Address to check
     * @return true if key available
     */
    virtual bool HasKey(const std::string& address) const = 0;

    /**
     * @brief Sign a P2MR (witness v3) input — post-quantum path.
     *
     * For a P2MR input, there is no 32-byte ECDSA private key; the signing
     * secret is a 32-byte PQ seed that must be decrypted with the wallet
     * master key, fed through KeygenFromSeed, and used to produce an
     * ML-DSA-65 signature. The result is the full canonical witness blob
     * (scheme_id | pubkey | sig | merkle_depth | siblings | leaf_index)
     * that the caller stuffs as the sole witness-stack element.
     *
     * Default implementation returns an empty vector — only PQ-aware
     * providers (e.g. wallet-layer HybridKeyProvider) override.
     *
     * @param script_pubkey The 34-byte P2MR scriptPubKey of the consumed UTXO.
     * @param sighash       BIP-341-style 32-byte sighash the signature signs.
     * @return Canonical witness blob on success; empty vector on failure.
     */
    virtual std::vector<uint8_t> SignP2MR(
        const std::vector<uint8_t>& /*script_pubkey*/,
        const std::array<uint8_t, 32>& /*sighash*/) const {
        return {};
    }
};

/**
 * @brief Simple map-based key provider (for testing/simple wallets)
 */
class MapKeyProvider : public KeyProvider {
public:
    /**
     * @brief Construct from address → private_key_hex map
     */
    explicit MapKeyProvider(const std::map<std::string, std::string>& keys);

    std::vector<uint8_t> GetPrivateKey(const std::string& address) const override;
    bool HasKey(const std::string& address) const override;

private:
    std::map<std::string, std::vector<uint8_t>> keys_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Signed Transaction Result
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Signature metadata (for verification/debugging)
 */
struct SignatureMetadata {
    size_t input_index;
    std::string address;
    bool is_signed;
    std::string error;  // If signing failed

    SignatureMetadata()
        : input_index(0), is_signed(false) {}
};

/**
 * @brief Signed transaction (ready for broadcast)
 */
struct SignedTransaction {
    Transaction tx;                              // Fully signed tx
    std::vector<SignatureMetadata> signatures;   // Signature metadata
    uint64_t fee;                                // Fee (unchanged from unsigned)
    uint64_t change_amount;                      // Change (unchanged from unsigned)

    SignedTransaction()
        : fee(0), change_amount(0) {}
};

/**
 * @brief Result of transaction signing
 */
struct SignResult {
    bool success;
    std::string error;
    SignedTransaction signed_tx;

    SignResult() : success(false) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Signer
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Transaction signer (single responsibility)
 *
 * Milestone 12.5: Clean separation between building and signing.
 *
 * Responsibilities:
 * - Sign UnsignedTransaction with provided keys
 * - Populate scriptSig/witness fields
 * - Local validation (sanity checks)
 * - Hardware wallet abstraction
 *
 * Does NOT:
 * - Change transaction structure (MUST preserve inputs/outputs)
 * - Check mempool policy (that's Milestone 12.6)
 * - Estimate fees (already done in Milestone 12.4)
 * - Select coins (already done in Milestone 12.3)
 */
class TransactionSigner {
public:
    /**
     * @brief Sign unsigned transaction
     *
     * Core rule: Transaction structure MUST NOT change.
     * Only scriptSig/witness fields are populated.
     *
     * @param unsigned_tx Unsigned transaction from UnsignedTxBuilder
     * @param key_provider Key provider (HD wallet, hardware wallet, etc.)
     * @return Sign result with fully signed transaction
     */
    static SignResult Sign(
        const UnsignedTransaction& unsigned_tx,
        const KeyProvider& key_provider
    );

    /**
     * @brief Validate signed transaction locally
     *
     * Wallet-side sanity checks before mempool submission.
     *
     * Checks:
     * - All inputs are signed (witness non-empty)
     * - Sighash flags correct
     * - Script types match UTXO (P2WPKH)
     * - Transaction structure valid
     *
     * Does NOT check:
     * - Mempool policy (ancestor limits, RBF rules, etc.)
     * - Fee rates (already validated in builder)
     * - Full script execution (too expensive)
     *
     * @param signed_tx Signed transaction
     * @param error Error message if validation fails
     * @return true if validation passes
     */
    static bool ValidateLocally(
        const SignedTransaction& signed_tx,
        std::string& error
    );

    /**
     * @brief Verify transaction ID matches unsigned version
     *
     * Ensures signing didn't change transaction structure.
     *
     * @param unsigned_tx Original unsigned transaction
     * @param signed_tx Signed transaction
     * @return true if txids match
     */
    static bool VerifyTxidUnchanged(
        const UnsignedTransaction& unsigned_tx,
        const SignedTransaction& signed_tx
    );

private:
    /**
     * @brief Sign single input
     *
     * @param tx Transaction being signed (modified in-place)
     * @param input_index Index of input to sign
     * @param all_utxos ALL UTXOs being spent (BIP341 requires full set for Taproot sighash)
     * @param private_key Private key for signing
     * @return true if signing succeeded
     */
    static bool SignInput(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& all_utxos,
        const std::vector<uint8_t>& private_key
    );

    /**
     * @brief Validate witness structure
     *
     * @param tx Signed transaction
     * @param error Error message if invalid
     * @return true if witness valid
     */
    static bool ValidateWitness(
        const Transaction& tx,
        std::string& error
    );
};

} // namespace dinero
