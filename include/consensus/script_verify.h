#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "primitives/transaction.h"
#include "consensus/utxo_entry.h"

namespace dinero {
namespace consensus {

/**
 * P2WPKH (Pay-to-Witness-Public-Key-Hash) signature verification
 * Implements BIP141 (SegWit) and BIP143 (signature hash)
 */
class ScriptVerifier {
public:
    /**
     * Verify a P2WPKH input signature
     *
     * @param tx The transaction being verified
     * @param input_index Index of the input being verified
     * @param utxo The UTXO being spent
     * @param error Output parameter for error message
     * @return true if signature is valid
     */
    static bool VerifyP2WPKH(const Transaction& tx, size_t input_index,
                            const UTXOEntry& utxo, std::string& error);
    
    /**
     * Compute BIP143 signature hash for SegWit v0 (P2WPKH)
     *
     * @param tx The transaction
     * @param input_index Index of input being signed
     * @param script_code The scriptCode (for P2WPKH: OP_DUP OP_HASH160 <hash> OP_EQUALVERIFY OP_CHECKSIG)
     * @param value The value of the output being spent
     * @param hash_type Signature hash type (usually SIGHASH_ALL = 1)
     * @return 32-byte signature hash
     */
    static std::vector<uint8_t> ComputeSignatureHash(const Transaction& tx,
                                                     size_t input_index,
                                                     const std::vector<uint8_t>& script_code,
                                                     uint64_t value,
                                                     uint32_t hash_type = 1);

    /**
     * Compute BIP341 signature hash for Taproot (SegWit v1)
     *
     * @param tx The transaction
     * @param input_index Index of input being signed
     * @param prevout_values Vector of all input values (for all inputs)
     * @param prevout_scripts Vector of all input scriptPubKeys (for all inputs)
     * @param hash_type Signature hash type (usually SIGHASH_DEFAULT = 0 or SIGHASH_ALL = 1)
     * @param tapleaf_hash Optional tapleaf hash for script path spending (empty for key path)
     * @param annex BIP341 annex data (empty if no annex present)
     * @return 32-byte signature hash
     */
    static std::vector<uint8_t> ComputeTaprootSighash(const Transaction& tx,
                                                       size_t input_index,
                                                       const std::vector<uint64_t>& prevout_values,
                                                       const std::vector<std::vector<uint8_t>>& prevout_scripts,
                                                       uint32_t hash_type = 0,
                                                       const std::vector<uint8_t>& tapleaf_hash = {},
                                                       const std::vector<uint8_t>& annex = {});
    
    /**
     * Check if a scriptPubKey is valid P2WPKH format
     * P2WPKH: OP_0 OP_PUSH20 <20-byte-pubkey-hash>
     * 
     * @param script_pubkey The scriptPubKey to check
     * @return true if valid P2WPKH
     */
    static bool IsP2WPKH(const std::vector<uint8_t>& script_pubkey);
    
    /**
     * Extract the 20-byte pubkey hash from a P2WPKH scriptPubKey
     *
     * @param script_pubkey The P2WPKH scriptPubKey
     * @return The 20-byte pubkey hash
     */
    static std::vector<uint8_t> ExtractPubkeyHash(const std::vector<uint8_t>& script_pubkey);

    /**
     * Verify a Taproot (P2TR) input signature (BIP341)
     *
     * @param tx The transaction being verified
     * @param input_index Index of the input being verified
     * @param input_utxos Vector of ALL input UTXOs (required for BIP341 sighash)
     * @param error Output parameter for error message
     * @return true if signature is valid
     */
    static bool VerifyTaproot(const Transaction& tx, size_t input_index,
                             const std::vector<UTXOEntry>& input_utxos, std::string& error);

    /**
     * Check if a scriptPubKey is valid P2TR (Taproot) format
     * P2TR: OP_1 OP_PUSH32 <32-byte-output-key>
     *
     * @param script_pubkey The scriptPubKey to check
     * @return true if valid P2TR
     */
    static bool IsP2TR(const std::vector<uint8_t>& script_pubkey);

    /**
     * Check if a scriptPubKey is valid OP_CTCOMMIT (Confidential Transaction) format
     * OP_CTCOMMIT: OP_2 OP_PUSH32 <32-byte-commitment-hash>
     *
     * @param script_pubkey The scriptPubKey to check
     * @return true if valid OP_CTCOMMIT
     */
    static bool IsOPCTCOMMIT(const std::vector<uint8_t>& script_pubkey);

    /**
     * Verify a confidential output (OP_CTCOMMIT) - witness v2
     *
     * OP_CTCOMMIT outputs don't have signatures to verify (they're unspendable markers).
     * This function validates the format and commitment structure.
     *
     * @param tx The transaction being verified
     * @param input_index Index of the input being verified
     * @param utxo The UTXO being spent
     * @param error Output parameter for error message
     * @return true if valid (always returns true for well-formed OP_CTCOMMIT)
     */
    static bool VerifyOPCTCOMMIT(const Transaction& tx, size_t input_index,
                                 const UTXOEntry& utxo, std::string& error);

private:
    // Hash160 (RIPEMD160(SHA256(data)))
    static std::vector<uint8_t> Hash160(const uint8_t* data, size_t len);
    
    // Double SHA256
    static std::vector<uint8_t> DoubleSHA256(const uint8_t* data, size_t len);
    
    // Hex utilities
    static std::vector<uint8_t> HexDecode(const std::string& hex);
    static std::string HexEncode(const std::vector<uint8_t>& data);
};

} // namespace consensus
} // namespace dinero

