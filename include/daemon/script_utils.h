#pragma once
#include <vector>
#include <cstdint>

namespace dinero::mining {

// Script opcodes
constexpr uint8_t OP_0 = 0x00;
constexpr uint8_t OP_1 = 0x51;  // Reserved for Taproot (BIP86)
constexpr uint8_t OP_2 = 0x52;  // Used for OP_CTCOMMIT

/**
 * Create a P2WPKH (Pay-to-Witness-PubkeyHash) script
 * Format: OP_0 <20-byte-hash>
 * 
 * @param hash160 The 20-byte ripemd160(sha256(pubkey)) hash
 * @return Script bytes for P2WPKH output
 */
inline std::vector<uint8_t> ScriptForP2WPKH(const std::vector<uint8_t>& hash160) {
    if (hash160.size() != 20) {
        return {}; // Invalid hash size
    }
    
    std::vector<uint8_t> script;
    script.reserve(22); // OP_0 + push(20) + 20 bytes = 22 bytes
    
    script.push_back(OP_0);           // OP_0 (witness version 0)
    script.push_back(20);             // Push 20 bytes
    script.insert(script.end(), hash160.begin(), hash160.end()); // 20-byte hash
    
    return script;
}

/**
 * Create a P2WSH (Pay-to-Witness-Script-Hash) script  
 * Format: OP_0 <32-byte-hash>
 * 
 * @param hash256 The 32-byte sha256(script) hash
 * @return Script bytes for P2WSH output
 */
inline std::vector<uint8_t> ScriptForP2WSH(const std::vector<uint8_t>& hash256) {
    if (hash256.size() != 32) {
        return {}; // Invalid hash size
    }
    
    std::vector<uint8_t> script;
    script.reserve(34); // OP_0 + push(32) + 32 bytes = 34 bytes
    
    script.push_back(OP_0);           // OP_0 (witness version 0)
    script.push_back(32);             // Push 32 bytes
    script.insert(script.end(), hash256.begin(), hash256.end()); // 32-byte hash
    
    return script;
}

/**
 * Validate that a script is a valid P2WPKH script
 * 
 * @param script The script to validate
 * @return true if script is OP_0 <20-bytes>
 */
inline bool IsP2WPKH(const std::vector<uint8_t>& script) {
    return script.size() == 22 && 
           script[0] == OP_0 && 
           script[1] == 20;
}

/**
 * Validate that a script is a valid P2WSH script
 *
 * @param script The script to validate
 * @return true if script is OP_0 <32-bytes>
 */
inline bool IsP2WSH(const std::vector<uint8_t>& script) {
    return script.size() == 34 &&
           script[0] == OP_0 &&
           script[1] == 32;
}

/**
 * Create an OP_CTCOMMIT (Confidential Transaction Commitment) script
 * Format: OP_2 <32-byte-commitment-hash>
 *
 * This is the production script type for confidential transaction outputs.
 * Uses witness version 2 to distinguish from:
 * - P2WPKH/P2WSH (witness v0, OP_0)
 * - Taproot P2TR (witness v1, OP_1) - reserved for future BIP86 support
 *
 * The 32-byte commitment hash is: SHA256(Ristretto255_commitment)
 * where Ristretto255_commitment is the 32-byte Pedersen commitment point.
 *
 * @param commitment_hash The 32-byte SHA256 hash of the Ristretto255 commitment
 * @return Script bytes for OP_CTCOMMIT output (34 bytes total)
 */
inline std::vector<uint8_t> ScriptForCTCommit(const std::vector<uint8_t>& commitment_hash) {
    if (commitment_hash.size() != 32) {
        return {}; // Invalid hash size
    }

    std::vector<uint8_t> script;
    script.reserve(34); // OP_2 + push(32) + 32 bytes = 34 bytes

    script.push_back(OP_2);           // OP_2 (witness version 2 for CT)
    script.push_back(32);             // Push 32 bytes
    script.insert(script.end(), commitment_hash.begin(), commitment_hash.end()); // 32-byte commitment hash

    return script;
}

/**
 * Validate that a script is a valid OP_CTCOMMIT script
 *
 * @param script The script to validate
 * @return true if script is OP_2 <32-bytes>
 */
inline bool IsCTCommit(const std::vector<uint8_t>& script) {
    return script.size() == 34 &&
           script[0] == OP_2 &&
           script[1] == 32;
}

} // namespace dinero::mining
