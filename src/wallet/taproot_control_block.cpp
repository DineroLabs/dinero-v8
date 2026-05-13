#include "wallet/taproot_control_block.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_template_builder.h"
#include <cstring>

namespace dinero {

TaprootControlBlock::TaprootControlBlock()
    : leaf_version(0xC0)  // Default to Tapscript (BIP342)
    , output_key_parity(false) {
    internal_key.fill(0);
}

std::vector<uint8_t> TaprootControlBlock::serialize() const {
    std::vector<uint8_t> result;
    result.reserve(33 + merkle_path.size() * 32);

    // First byte: leaf_version | parity_bit
    // leaf_version occupies upper 7 bits (masked with 0xFE)
    // parity_bit is least significant bit
    uint8_t first_byte = (leaf_version & 0xFE) | (output_key_parity ? 0x01 : 0x00);
    result.push_back(first_byte);

    // Internal key (32 bytes)
    result.insert(result.end(), internal_key.begin(), internal_key.end());

    // Merkle path nodes (32 bytes each)
    for (const auto& node : merkle_path) {
        result.insert(result.end(), node.begin(), node.end());
    }

    return result;
}

bool TaprootControlBlock::parse(const std::vector<uint8_t>& data) {
    // Minimum size: 1 byte header + 32 bytes internal key = 33 bytes
    if (data.size() < 33) {
        return false;
    }

    // Size must be 33 + 32*n
    if ((data.size() - 33) % 32 != 0) {
        return false;
    }

    // Parse first byte
    uint8_t first_byte = data[0];
    leaf_version = first_byte & 0xFE;  // Upper 7 bits
    output_key_parity = (first_byte & 0x01) != 0;  // LSB

    // Parse internal key
    std::copy(data.begin() + 1, data.begin() + 33, internal_key.begin());

    // Parse merkle path
    merkle_path.clear();
    size_t num_nodes = (data.size() - 33) / 32;
    for (size_t i = 0; i < num_nodes; i++) {
        std::array<uint8_t, 32> node;
        std::copy(data.begin() + 33 + i * 32,
                  data.begin() + 33 + (i + 1) * 32,
                  node.begin());
        merkle_path.push_back(node);
    }

    return true;
}

TaprootControlBlock TaprootControlBlock::forSingleLeaf(
    const std::array<uint8_t, 32>& internal_key,
    uint8_t leaf_version,
    bool output_parity) {

    TaprootControlBlock cb;
    cb.internal_key = internal_key;
    cb.leaf_version = leaf_version;
    cb.output_key_parity = output_parity;
    // merkle_path is empty for single-leaf tree
    return cb;
}

bool TaprootControlBlock::verify(const std::array<uint8_t, 32>& leaf_hash,
                                  const std::array<uint8_t, 32>& expected_output_key) const {
    // Start with the leaf hash
    std::array<uint8_t, 32> current = leaf_hash;

    // Walk up the merkle tree
    for (const auto& node : merkle_path) {
        std::array<uint8_t, 32> branch_hash;
        if (!TaprootKeys::ComputeTapBranchHash(current, node, branch_hash)) {
            return false;
        }
        current = branch_hash;
    }

    // current is now the merkle root
    // Compute output key: internal_key + TaggedHash("TapTweak", internal_key || merkle_root) * G
    int computed_parity;
    if (!TaprootKeys::ComputeOutputKeyParity(internal_key, current, computed_parity)) {
        return false;
    }

    // Verify parity matches
    if ((computed_parity != 0) != output_key_parity) {
        return false;
    }

    // Compute the actual tweaked Taproot output key and compare bytes.
    std::vector<uint8_t> computed_output_key =
        TaprootTemplateBuilder::ComputeOutputKey(
            std::vector<uint8_t>(internal_key.begin(), internal_key.end()),
            current);
    if (computed_output_key.size() != expected_output_key.size()) {
        return false;
    }

    return std::equal(computed_output_key.begin(),
                      computed_output_key.end(),
                      expected_output_key.begin());
}

bool TaprootControlBlock::isValidSize(size_t size) {
    if (size < 33) return false;
    return (size - 33) % 32 == 0;
}

std::vector<std::vector<uint8_t>> buildScriptPathWitness(
    const std::vector<std::vector<uint8_t>>& signatures,
    const std::vector<uint8_t>& script,
    const TaprootControlBlock& control_block) {

    std::vector<std::vector<uint8_t>> witness;

    // 1. Add signatures (one per CHECKSIG in the script)
    for (const auto& sig : signatures) {
        witness.push_back(sig);
    }

    // 2. Add the script
    witness.push_back(script);

    // 3. Add the control block
    witness.push_back(control_block.serialize());

    return witness;
}

} // namespace dinero
