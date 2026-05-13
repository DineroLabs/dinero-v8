#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace din {

/**
 * @brief Taproot sighash computation (BIP-341)
 * 
 * Implements Taproot signature hash computation for:
 * - Key path spending (simple 1-of-1)
 * - Script path spending (complex scripts with Merkle proofs)
 */
class TaprootSighash {
public:
    /**
     * @brief Sighash type for Taproot
     */
    enum class SighashType : uint8_t {
        DEFAULT = 0x00,  // SIGHASH_DEFAULT (0x00)
        ALL = 0x01,      // SIGHASH_ALL (0x01)
        NONE = 0x02,     // SIGHASH_NONE (0x02)
        SINGLE = 0x03,   // SIGHASH_SINGLE (0x03)
        ANYONECANPAY = 0x80  // SIGHASH_ANYONECANPAY (0x80)
    };
    
    /**
     * @brief Compute Taproot sighash for key path spending
     * 
     * @param unsigned_tx Unsigned transaction bytes
     * @param input_index Index of input being signed
     * @param prevouts_hash Hash of all previous outputs
     * @param amounts_hash Hash of all input amounts
     * @param script_pubkeys_hash Hash of all scriptPubKeys
     * @param tapleaf_hash Hash of tapleaf (for script path)
     * @param key_version Key version (0x00 for key path)
     * @param codesep_pos Code separator position
     * @param sighash_type Sighash type
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> computeSighash(
        const std::vector<uint8_t>& unsigned_tx,
        uint32_t input_index,
        const std::vector<uint8_t>& prevouts_hash,
        const std::vector<uint8_t>& amounts_hash,
        const std::vector<uint8_t>& script_pubkeys_hash,
        const std::vector<uint8_t>& tapleaf_hash,
        uint8_t key_version,
        uint32_t codesep_pos,
        SighashType sighash_type
    );
    
    /**
     * @brief Compute Taproot sighash for script path spending
     * 
     * @param unsigned_tx Unsigned transaction bytes
     * @param input_index Index of input being signed
     * @param prevouts_hash Hash of all previous outputs
     * @param amounts_hash Hash of all input amounts
     * @param script_pubkeys_hash Hash of all scriptPubKeys
     * @param tapleaf_hash Hash of tapleaf
     * @param key_version Key version (0x00 for script path)
     * @param codesep_pos Code separator position
     * @param sighash_type Sighash type
     * @return 32-byte sighash
     */
    static std::vector<uint8_t> computeScriptPathSighash(
        const std::vector<uint8_t>& unsigned_tx,
        uint32_t input_index,
        const std::vector<uint8_t>& prevouts_hash,
        const std::vector<uint8_t>& amounts_hash,
        const std::vector<uint8_t>& script_pubkeys_hash,
        const std::vector<uint8_t>& tapleaf_hash,
        uint8_t key_version,
        uint32_t codesep_pos,
        SighashType sighash_type
    );
    
    /**
     * @brief Compute hash of previous outputs
     * 
     * @param prevouts Vector of previous output data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> computePrevoutsHash(
        const std::vector<std::vector<uint8_t>>& prevouts
    );
    
    /**
     * @brief Compute hash of input amounts
     * 
     * @param amounts Vector of input amounts (8 bytes each)
     * @return 32-byte hash
     */
    static std::vector<uint8_t> computeAmountsHash(
        const std::vector<std::vector<uint8_t>>& amounts
    );
    
    /**
     * @brief Compute hash of scriptPubKeys
     * 
     * @param script_pubkeys Vector of scriptPubKey data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> computeScriptPubkeysHash(
        const std::vector<std::vector<uint8_t>>& script_pubkeys
    );
    
    /**
     * @brief Compute tapleaf hash
     * 
     * @param script Script bytes
     * @param leaf_version Leaf version (0xC0 for Taproot)
     * @return 32-byte tapleaf hash
     */
    static std::vector<uint8_t> computeTapleafHash(
        const std::vector<uint8_t>& script,
        uint8_t leaf_version = 0xC0
    );
    
    /**
     * @brief Compute Merkle root of script tree
     * 
     * @param scripts Vector of scripts
     * @param leaf_version Leaf version for all scripts
     * @return 32-byte Merkle root
     */
    static std::vector<uint8_t> computeScriptTreeMerkleRoot(
        const std::vector<std::vector<uint8_t>>& scripts,
        uint8_t leaf_version = 0xC0
    );

private:
    /**
     * @brief Tagged hash function (BIP-340)
     */
    static std::vector<uint8_t> taggedHash(
        const std::string& tag,
        const std::vector<uint8_t>& data
    );
    
    /**
     * @brief Compute transaction hash
     */
    static std::vector<uint8_t> computeTxHash(
        const std::vector<uint8_t>& unsigned_tx,
        SighashType sighash_type
    );
    
    /**
     * @brief Serialize sighash data
     */
    static std::vector<uint8_t> serializeSighashData(
        const std::vector<uint8_t>& unsigned_tx,
        uint32_t input_index,
        const std::vector<uint8_t>& prevouts_hash,
        const std::vector<uint8_t>& amounts_hash,
        const std::vector<uint8_t>& script_pubkeys_hash,
        const std::vector<uint8_t>& tapleaf_hash,
        uint8_t key_version,
        uint32_t codesep_pos,
        SighashType sighash_type
    );
};

} // namespace din
