#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace din {

/**
 * @brief Utility to build unsigned transactions for PSBT creation
 * 
 * Creates a transaction with empty scriptSigs and no witnesses,
 * suitable for use as the global unsigned transaction in PSBTs.
 */
class UnsignedTxBuilder {
public:
    struct TxInput {
        std::vector<uint8_t> prev_hash;  // 32 bytes
        uint32_t prev_index;             // 4 bytes
        uint32_t sequence;               // 4 bytes (default 0xFFFFFFFF)
        
        TxInput(const std::vector<uint8_t>& hash, uint32_t idx, uint32_t seq = 0xFFFFFFFF)
            : prev_hash(hash), prev_index(idx), sequence(seq) {}
    };
    
    struct TxOutput {
        uint64_t value;                  // 8 bytes
        std::vector<uint8_t> script_pubkey; // Variable length
        
        TxOutput(uint64_t val, const std::vector<uint8_t>& script)
            : value(val), script_pubkey(script) {}
    };
    
    /**
     * @brief Build unsigned transaction bytes
     * 
     * @param inputs Transaction inputs (prevout references only)
     * @param outputs Transaction outputs (value + scriptPubKey)
     * @param version Transaction version (default 2)
     * @param locktime Transaction locktime (default 0)
     * @return Serialized unsigned transaction bytes
     */
    static std::vector<uint8_t> build(
        const std::vector<TxInput>& inputs,
        const std::vector<TxOutput>& outputs,
        uint32_t version = 2,
        uint32_t locktime = 0
    );
    
    /**
     * @brief Helper to create P2WPKH scriptPubKey
     * 
     * @param pubkey_hash 20-byte hash160 of public key
     * @return P2WPKH scriptPubKey (OP_0 <20-byte-hash>)
     */
    static std::vector<uint8_t> createP2WPKHScript(const std::vector<uint8_t>& pubkey_hash);
    
    /**
     * @brief Helper to create P2PKH scriptPubKey
     * 
     * @param pubkey_hash 20-byte hash160 of public key
     * @return P2PKH scriptPubKey (OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG)
     */
    static std::vector<uint8_t> createP2PKHScript(const std::vector<uint8_t>& pubkey_hash);

private:
    static void put_compact_size(std::vector<uint8_t>& out, uint64_t v);
    static void put_le32(std::vector<uint8_t>& out, uint32_t v);
    static void put_le64(std::vector<uint8_t>& out, uint64_t v);
};

} // namespace din
