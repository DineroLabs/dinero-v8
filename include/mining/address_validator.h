#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace mining {

// ============================================================================
// Address Type Classification (Phase 26.4)
// ============================================================================

/**
 * Dinero address types
 */
enum class AddressType {
    P2PKH,              // Pay-to-PubKey-Hash (legacy base58: D...)
    P2SH,               // Pay-to-Script-Hash (legacy base58)
    P2WPKH,             // Pay-to-Witness-PubKey-Hash (SegWit v0: din1q...)
    P2WSH,              // Pay-to-Witness-Script-Hash (SegWit v0: din1q...)
    P2TR,               // Pay-to-Taproot (SegWit v1: din1p...)
    P2MR,               // Pay-to-ML-DSA-65 Merkle-Root (witness v3: din1r...)
    UNKNOWN_WITNESS,    // Unknown witness version
    UNKNOWN_LEGACY,     // Unknown legacy version
    INVALID             // Invalid address
};

/**
 * Dinero network types
 */
enum class AddressNetwork {
    MAINNET,
    TESTNET,
    REGTEST
};

/**
 * Decoded address information
 */
struct AddressInfo {
    AddressType type;
    AddressNetwork network;
    int witness_version;              // For SegWit addresses (0, 1, etc.)
    std::vector<uint8_t> program;     // Witness program or pubkey hash

    AddressInfo()
        : type(AddressType::INVALID)
        , network(AddressNetwork::MAINNET)
        , witness_version(-1)
    {}
};

// ============================================================================
// Address Validation and Decoding
// ============================================================================

/**
 * Check if address is valid Dinero address
 *
 * Validates both Bech32 SegWit addresses and Base58Check legacy addresses.
 *
 * Supported formats:
 * - Mainnet Bech32: din1q... (P2WPKH), din1p... (P2TR)
 * - Testnet Bech32: tdin1...
 * - Regtest Bech32: rdin1...
 * - Mainnet Base58: D... (P2PKH)
 *
 * @param addr  Address string to validate
 * @return      True if address is valid
 */
bool IsValidDineroAddress(const std::string& addr);

/**
 * Decode Dinero address and extract information
 *
 * Decodes address and populates AddressInfo with:
 * - Address type (P2PKH, P2WPKH, P2TR, etc.)
 * - Network (mainnet, testnet, regtest)
 * - Witness version (for SegWit addresses)
 * - Program/hash data
 *
 * @param addr  Address string to decode
 * @param info  [out] Decoded address information
 * @return      True if address was successfully decoded
 */
bool DecodeAddress(const std::string& addr, AddressInfo& info);

/**
 * Build scriptPubKey from decoded address
 *
 * Creates the appropriate Bitcoin Script output based on address type:
 * - P2PKH:  OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
 * - P2SH:   OP_HASH160 <20-byte-hash> OP_EQUAL
 * - P2WPKH: OP_0 <20-byte-hash>
 * - P2WSH:  OP_0 <32-byte-hash>
 * - P2TR:   OP_1 <32-byte-x-only-pubkey>
 *
 * @param info  Decoded address information
 * @return      scriptPubKey bytes (empty if invalid)
 */
std::vector<uint8_t> BuildScriptPubKey(const AddressInfo& info);

/**
 * Check if address is a Taproot (P2TR) address
 *
 * Dinero mining uses Taproot-only coinbase outputs by policy.
 * This function validates that an address is P2TR (witness version 1).
 *
 * Taproot addresses:
 * - Mainnet: din1p...
 * - Testnet: tdin1p...
 * - Regtest: rdin1p...
 *
 * @param addr  Address string to check
 * @return      True if address is valid P2TR (Taproot)
 */
bool IsTaprootAddress(const std::string& addr);

/**
 * Check if an address is an eligible v7 coinbase destination.
 * v7 accepts Taproot (P2TR, din1p...) or P2MR (din1r...) mining outputs.
 * P2MR coinbases produce quantum-safe UTXOs from day one.
 */
bool IsCoinbaseEligibleAddress(const std::string& addr);

/**
 * Get user-friendly error message for non-Taproot mining address
 *
 * Returns a helpful message explaining that mining requires Taproot
 * addresses and how to generate one.
 *
 * @param addr  The non-Taproot address that was rejected
 * @return      User-friendly error message
 */
std::string GetTaprootRequiredMessage(const std::string& addr);

} // namespace mining
} // namespace dinero
