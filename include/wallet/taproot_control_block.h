#pragma once

#include <array>
#include <vector>
#include <cstdint>

namespace dinero {

/**
 * Taproot Control Block (BIP341)
 *
 * The control block proves that a script belongs to the Taproot script tree.
 * Used for script-path spending.
 *
 * Format:
 *   <1 byte: leaf_version | parity_bit>
 *   <32 bytes: internal_pubkey>
 *   <32 bytes: merkle_proof_node_1>  (optional)
 *   <32 bytes: merkle_proof_node_2>  (optional)
 *   ...
 *
 * The parity bit is the least significant bit of the first byte.
 * leaf_version is in the upper 7 bits (masked with 0xFE).
 */
struct TaprootControlBlock {
    uint8_t leaf_version;                                // Leaf version (e.g., 0xC0 for Tapscript)
    bool output_key_parity;                              // Parity of output key Y (0=even, 1=odd)
    std::array<uint8_t, 32> internal_key;                // 32-byte x-only internal pubkey
    std::vector<std::array<uint8_t, 32>> merkle_path;    // Merkle proof nodes

    /**
     * Default constructor
     */
    TaprootControlBlock();

    /**
     * Serialize control block to bytes
     *
     * @return Serialized control block
     */
    std::vector<uint8_t> serialize() const;

    /**
     * Parse control block from bytes
     *
     * @param data Control block bytes
     * @return true on success
     */
    bool parse(const std::vector<uint8_t>& data);

    /**
     * Create control block for single-leaf tree (no merkle path)
     *
     * @param internal_key 32-byte x-only internal pubkey
     * @param leaf_version Leaf version (0xC0 for Tapscript)
     * @param output_parity Parity of output key Y coordinate
     * @return Constructed control block
     */
    static TaprootControlBlock forSingleLeaf(
        const std::array<uint8_t, 32>& internal_key,
        uint8_t leaf_version,
        bool output_parity);

    /**
     * Verify control block against expected output key
     *
     * Computes the merkle root from the leaf hash and merkle path,
     * then verifies the output key matches.
     *
     * @param leaf_hash 32-byte tapleaf hash of the script
     * @param expected_output_key 32-byte expected output x-only pubkey
     * @return true if control block is valid for this leaf
     */
    bool verify(const std::array<uint8_t, 32>& leaf_hash,
                const std::array<uint8_t, 32>& expected_output_key) const;

    /**
     * Get the minimum valid control block size (no merkle path)
     */
    static constexpr size_t minSize() { return 33; }  // 1 + 32

    /**
     * Check if size is valid for a control block
     * Must be 33 + 32*n where n >= 0
     */
    static bool isValidSize(size_t size);
};

/**
 * Build a witness stack for script-path spending
 *
 * @param signatures Vector of 64-byte Schnorr signatures (one per CHECKSIG)
 * @param script The Tapscript being executed
 * @param control_block The control block proving script inclusion
 * @return Witness stack: [sig1, sig2, ..., script, control_block]
 */
std::vector<std::vector<uint8_t>> buildScriptPathWitness(
    const std::vector<std::vector<uint8_t>>& signatures,
    const std::vector<uint8_t>& script,
    const TaprootControlBlock& control_block);

} // namespace dinero
