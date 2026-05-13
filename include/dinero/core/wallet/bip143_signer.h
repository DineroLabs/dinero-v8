#pragma once

#include <vector>
#include <string>
#include <optional>
#include <cstdint>
#include "wallet/transaction.h"
#include "wallet/utxo_index.h"

namespace dinero {

/**
 * BIP143 SegWit transaction signing implementation
 * Handles P2WPKH (Pay-to-Witness-PubkeyHash) input signing
 */
class BIP143Signer {
public:
    /**
     * Sign a P2WPKH input using BIP143 specification
     * @param tx The transaction to sign
     * @param input_index Index of the input to sign
     * @param utxo The UTXO being spent
     * @param private_key_hex Private key in hex format (32 bytes)
     * @return true if signing succeeded, false otherwise
     */
    static bool SignP2WPKHInput(
        Transaction& tx,
        uint32_t input_index,
        const UTXO& utxo,
        const std::string& private_key_hex
    );

    /**
     * Sign multiple P2WPKH inputs in a transaction
     * @param tx The transaction to sign
     * @param utxos Vector of UTXOs being spent (must match input order)
     * @param private_keys Vector of private keys in hex format
     * @return Number of inputs successfully signed
     */
    static uint32_t SignTransaction(
        Transaction& tx,
        const std::vector<UTXO>& utxos,
        const std::vector<std::string>& private_keys
    );

    /**
     * Verify a P2WPKH signature using BIP143
     * @param tx The transaction containing the signature
     * @param input_index Index of the input to verify
     * @param utxo The UTXO being spent
     * @param public_key_hex Public key in hex format (33 bytes compressed)
     * @return true if signature is valid, false otherwise
     */
    static bool VerifyP2WPKHSignature(
        const Transaction& tx,
        uint32_t input_index,
        const UTXO& utxo,
        const std::string& public_key_hex
    );

private:
    /**
     * Compute BIP143 signature hash for P2WPKH input
     * @param tx The transaction
     * @param input_index Index of the input
     * @param script_code The scriptCode (P2PKH script for P2WPKH)
     * @param amount The amount of the UTXO being spent
     * @param hash_type Signature hash type (SIGHASH_ALL = 1)
     * @return 32-byte signature hash
     */
    static std::vector<uint8_t> ComputeBIP143Hash(
        const Transaction& tx,
        uint32_t input_index,
        const std::vector<uint8_t>& script_code,
        int64_t amount,
        uint32_t hash_type = 1  // SIGHASH_ALL
    );

    /**
     * Create P2PKH scriptCode from pubkey hash
     * For P2WPKH, this is OP_DUP OP_HASH160 <20-byte-pubkey-hash> OP_EQUALVERIFY OP_CHECKSIG
     * @param pubkey_hash 20-byte pubkey hash
     * @return P2PKH script as byte vector
     */
    static std::vector<uint8_t> CreateP2PKHScriptCode(const std::vector<uint8_t>& pubkey_hash);

    /**
     * Serialize transaction for BIP143 hash computation
     * @param tx The transaction
     * @param input_index Index of the input being signed
     * @param script_code The scriptCode
     * @param amount The amount being spent
     * @param hash_type Signature hash type
     * @return Serialized data for hashing
     */
    static std::vector<uint8_t> SerializeForBIP143(
        const Transaction& tx,
        uint32_t input_index,
        const std::vector<uint8_t>& script_code,
        int64_t amount,
        uint32_t hash_type
    );

    /**
     * Compute double SHA256 hash
     * @param data Input data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> DoubleSHA256(const std::vector<uint8_t>& data);

    /**
     * Convert hex string to bytes
     * @param hex Hex string
     * @return Byte vector
     */
    static std::vector<uint8_t> HexToBytes(const std::string& hex);

    /**
     * Convert bytes to hex string
     * @param bytes Byte vector
     * @return Hex string
     */
    static std::string BytesToHex(const std::vector<uint8_t>& bytes);

    /**
     * Serialize uint32_t in little-endian format
     * @param value The value to serialize
     * @return 4-byte vector
     */
    static std::vector<uint8_t> SerializeUInt32LE(uint32_t value);

    /**
     * Serialize int64_t in little-endian format
     * @param value The value to serialize
     * @return 8-byte vector
     */
    static std::vector<uint8_t> SerializeInt64LE(int64_t value);
};

} // namespace dinero
