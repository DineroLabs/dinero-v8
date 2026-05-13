#pragma once

#include "wallet/transaction.h"
#include "wallet/hd_wallet.h"
#include "wallet/intent_descriptor.h"  // DEFAULT_EXT_COMMITMENT
#include <array>
#include <vector>
#include <string>

namespace dinero {

/**
 * @brief BIP341 Taproot Transaction Signer
 *
 * Signs Taproot key-path spending transactions using BIP340 Schnorr signatures.
 * Integrates with HDWallet for key derivation and signing.
 *
 * Key Features:
 * - BIP341 Taproot sighash computation
 * - BIP340 Schnorr signature generation
 * - Key-path spending (no script tree)
 * - Batch signing for multiple inputs
 */
class TaprootTxSigner {
public:
    /**
     * @brief Sign a Taproot transaction
     *
     * Signs all Taproot inputs in the transaction using BIP340 Schnorr signatures.
     * Supports mixed transactions (some P2WPKH, some P2TR inputs).
     *
     * @param tx Transaction to sign (modified in-place)
     * @param utxos UTXOs being spent (must match tx inputs)
     * @param private_keys Tweaked Taproot private keys for signing
     * @return true if all Taproot inputs were signed successfully
     */
    static bool SignTransaction(
        Transaction& tx,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<std::vector<uint8_t>>& private_keys
    );

    /**
     * @brief Sign a single Taproot input
     *
     * @param tx Transaction being signed
     * @param input_index Index of input to sign
     * @param all_utxos ALL UTXOs being spent (BIP341 requires full set for sighash)
     * @param private_key Tweaked Taproot private key (32 bytes)
     * @return true if signing succeeded
     */
    static bool SignInput(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& all_utxos,
        const std::vector<uint8_t>& private_key
    );

    /**
     * @brief Compute BIP341 Taproot sighash for key-path spending
     *
     * Computes the message hash that gets signed with Schnorr signature.
     * Uses SIGHASH_DEFAULT (0x00) which is equivalent to SIGHASH_ALL.
     *
     * @param tx Transaction being signed
     * @param input_index Index of input being signed
     * @param utxos All UTXOs being spent (for prevouts/amounts/scriptPubKeys hashes)
     * @param sighash_type Sighash type (default: SIGHASH_DEFAULT/0x00)
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> ComputeTaprootSighash(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        uint8_t sighash_type = SIGHASH_DEFAULT
    );

    /**
     * @brief Compute BIP341 Taproot sighash for script-path spending
     *
     * Computes the message hash for script-path spending (different from key-path).
     * Includes the tapleaf hash in the sighash computation.
     *
     * @param tx Transaction being signed
     * @param input_index Index of input being signed
     * @param utxos All UTXOs being spent
     * @param script Script being executed
     * @param leaf_version Tapscript leaf version (default: 0xc0)
     * @param sighash_type Sighash type (default: SIGHASH_DEFAULT/0x00)
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> ComputeScriptPathSighash(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<uint8_t>& script,
        uint8_t leaf_version = 0xc0,
        uint8_t sighash_type = SIGHASH_DEFAULT
    );

    /**
     * @brief Sign a message hash with BIP340 Schnorr signature
     *
     * @param message_hash 32-byte message hash
     * @param private_key 32-byte tweaked private key
     * @return 64-byte Schnorr signature, or empty on failure
     */
    static std::vector<uint8_t> SignSchnorr(
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& private_key
    );

    /**
     * @brief Check if a UTXO is a Taproot output
     *
     * @param utxo UTXO to check
     * @return true if scriptPubKey is P2TR (OP_1 <32-byte-pubkey>)
     */
    static bool IsTaprootUTXO(const CanonicalWalletUTXO& utxo);

    // ========================================================================
    // SigHash V1: Dinero-specific sighash with extension field
    // Tag: "dinero/sighash/v1" (consensus-breaking change from "TapSighash")
    // Key-path message: 207 bytes (175 + 32-byte ext_commitment)
    // Script-path message: 244 bytes (212 + 32-byte ext_commitment)
    // ========================================================================

    /**
     * @brief Compute Dinero SigHash V1 for key-path spending
     *
     * Uses TaggedHash("dinero/sighash/v1", message) instead of "TapSighash".
     * Appends 32-byte ext_commitment after input_index.
     * CT-aware prevout commitments are composed into the extension automatically.
     *
     * @param tx Transaction being signed
     * @param input_index Index of input being signed
     * @param utxos All UTXOs being spent
     * @param ext_commitment 32-byte extension commitment (zeros for consensus)
     * @param sighash_type Sighash type (default: SIGHASH_DEFAULT)
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> ComputeTaprootSighashV1(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::array<uint8_t, 32>& ext_commitment,
        uint8_t sighash_type = SIGHASH_DEFAULT
    );

    /**
     * @brief Compute Dinero SigHash V1 for script-path spending
     *
     * Uses TaggedHash("dinero/sighash/v1", message) instead of "TapSighash".
     * Appends 32-byte ext_commitment after codesep_pos.
     *
     * @param tx Transaction being signed
     * @param input_index Index of input being signed
     * @param utxos All UTXOs being spent
     * @param script Script being executed
     * @param ext_commitment 32-byte extension commitment
     * @param leaf_version Tapscript leaf version (default: 0xc0)
     * @param sighash_type Sighash type (default: SIGHASH_DEFAULT)
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> ComputeScriptPathSighashV1(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<uint8_t>& script,
        const std::array<uint8_t, 32>& ext_commitment,
        uint8_t leaf_version = 0xc0,
        uint8_t sighash_type = SIGHASH_DEFAULT
    );

    /**
     * @brief Sign a single Taproot input using SigHash V1
     *
     * Uses V1 sighash computation with ext_commitment.
     * For standard key-path spending, pass DEFAULT_EXT_COMMITMENT.
     * For intent-bound signing, pass IntentDescriptor::ComputeExtCommitment().
     * Confidential prevout binding is composed in automatically.
     *
     * @param tx Transaction being signed
     * @param input_index Index of input to sign
     * @param all_utxos ALL UTXOs being spent
     * @param private_key Tweaked Taproot private key (32 bytes)
     * @param ext_commitment 32-byte extension commitment
     * @return true if signing succeeded
     */
    static bool SignInputV1(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& all_utxos,
        const std::vector<uint8_t>& private_key,
        const std::array<uint8_t, 32>& ext_commitment
    );

    // Sighash types (BIP341)
    static constexpr uint8_t SIGHASH_DEFAULT = 0x00;  // Taproot-specific: equivalent to SIGHASH_ALL
    static constexpr uint8_t SIGHASH_ALL = 0x01;
    static constexpr uint8_t SIGHASH_NONE = 0x02;
    static constexpr uint8_t SIGHASH_SINGLE = 0x03;
    static constexpr uint8_t SIGHASH_ANYONECANPAY = 0x80;

private:
    /**
     * @brief Compute BIP341 epoch 0 sighash message (key-path)
     *
     * Constructs the full sighash preimage for Taproot key-path spending.
     * Reference: BIP341 "Common Signature Message"
     */
    static std::vector<uint8_t> ComputeSighashMessage(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        uint8_t sighash_type
    );

    /**
     * @brief Compute BIP341 epoch 0 sighash message (script-path)
     *
     * Constructs the full sighash preimage for Taproot script-path spending.
     * Includes ext_flag=1 and tapleaf hash.
     */
    static std::vector<uint8_t> ComputeScriptPathSighashMessage(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<uint8_t>& tapleaf_hash,
        uint8_t sighash_type
    );

    /**
     * @brief Compute tapleaf hash (BIP341)
     *
     * tapleaf_hash = TaggedHash("TapLeaf", [leaf_version || compact_size(script) || script])
     */
    static std::vector<uint8_t> ComputeTapleafHash(
        const std::vector<uint8_t>& script,
        uint8_t leaf_version = 0xc0
    );

    /**
     * @brief Compute SHA256(SHA256(prevouts))
     */
    static std::vector<uint8_t> ComputePrevoutsHash(const Transaction& tx);

    /**
     * @brief Compute SHA256(SHA256(amounts))
     */
    static std::vector<uint8_t> ComputeAmountsHash(const std::vector<CanonicalWalletUTXO>& utxos);

    /**
     * @brief Compute SHA256(SHA256(scriptPubKeys))
     */
    static std::vector<uint8_t> ComputeScriptPubKeysHash(const std::vector<CanonicalWalletUTXO>& utxos);

    /**
     * @brief Compute SHA256(SHA256(sequences))
     */
    static std::vector<uint8_t> ComputeSequencesHash(const Transaction& tx);

    /**
     * @brief Compute SHA256(SHA256(outputs))
     */
    static std::vector<uint8_t> ComputeOutputsHash(const Transaction& tx);

    /**
     * @brief Tagged hash function (BIP340)
     *
     * SHA256(SHA256(tag) || SHA256(tag) || data)
     */
    static std::vector<uint8_t> TaggedHash(const std::string& tag, const std::vector<uint8_t>& data);

    /**
     * @brief Compute V1 sighash message (key-path) with ext_commitment
     */
    static std::vector<uint8_t> ComputeSighashMessageV1(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::array<uint8_t, 32>& ext_commitment,
        uint8_t sighash_type
    );

    /**
     * @brief Compute V1 sighash message (script-path) with ext_commitment
     */
    static std::vector<uint8_t> ComputeScriptPathSighashMessageV1(
        const Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<uint8_t>& tapleaf_hash,
        const std::array<uint8_t, 32>& ext_commitment,
        uint8_t sighash_type
    );

    /**
     * @brief Serialize uint32_t as little-endian
     */
    static void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value);

    /**
     * @brief Serialize uint64_t as little-endian
     */
    static void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value);
};

} // namespace dinero
