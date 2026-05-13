/**
 * Phase 30: Taproot Asset Layer - Proof System Implementation
 */

#include "assets/asset_proof.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <map>

namespace dinero {
namespace assets {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

std::array<uint8_t, 32> sha256_single(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> hash;
    crypto::CSHA256 ctx;
    ctx.Write(data.data(), data.size());
    ctx.Finalize(hash.data());
    return hash;
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoi(byteString, nullptr, 16)));
    }
    return bytes;
}

void writeLE64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

void writeLE32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

uint64_t readLE64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

uint32_t readLE32(const uint8_t* data) {
    uint32_t result = 0;
    for (int i = 3; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

void writeVarBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    size_t len = bytes.size();
    if (len < 0xFD) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    } else {
        out.push_back(0xFE);
        writeLE32(out, static_cast<uint32_t>(len));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> readVarBytes(const uint8_t*& data, size_t& remaining) {
    if (remaining < 1) return {};

    size_t len = data[0];
    data++; remaining--;

    if (len == 0xFD) {
        if (remaining < 2) return {};
        len = data[0] | (static_cast<size_t>(data[1]) << 8);
        data += 2; remaining -= 2;
    } else if (len == 0xFE) {
        if (remaining < 4) return {};
        len = readLE32(data);
        data += 4; remaining -= 4;
    }

    if (remaining < len) return {};
    std::vector<uint8_t> result(data, data + len);
    data += len; remaining -= len;
    return result;
}

void writeVarString(std::vector<uint8_t>& out, const std::string& str) {
    size_t len = str.size();
    if (len < 0xFD) {
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    }
    out.insert(out.end(), str.begin(), str.end());
}

std::string readVarString(const uint8_t*& data, size_t& remaining) {
    auto bytes = readVarBytes(data, remaining);
    return std::string(bytes.begin(), bytes.end());
}

} // anonymous namespace

// ============================================================================
// MerkleNode Implementation
// ============================================================================

std::vector<uint8_t> MerkleNode::serialize() const {
    std::vector<uint8_t> data;
    data.insert(data.end(), hash.begin(), hash.end());
    data.push_back(is_left ? 1 : 0);
    return data;
}

std::optional<MerkleNode> MerkleNode::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 33) return std::nullopt;

    MerkleNode node;
    std::copy(data.begin(), data.begin() + 32, node.hash.begin());
    node.is_left = (data[32] != 0);
    return node;
}

// ============================================================================
// MerkleProof Implementation
// ============================================================================

bool MerkleProof::verify() const {
    return computeRoot() == root;
}

std::array<uint8_t, 32> MerkleProof::computeRoot() const {
    std::array<uint8_t, 32> current = leaf_hash;

    for (const auto& node : path) {
        std::vector<uint8_t> combined;

        // Use Taproot-style lexicographic sorting (same as BuildMerkleTree)
        if (current < node.hash) {
            combined.insert(combined.end(), current.begin(), current.end());
            combined.insert(combined.end(), node.hash.begin(), node.hash.end());
        } else {
            combined.insert(combined.end(), node.hash.begin(), node.hash.end());
            combined.insert(combined.end(), current.begin(), current.end());
        }

        current = sha256_single(combined);
    }

    return current;
}

std::vector<uint8_t> MerkleProof::serialize() const {
    std::vector<uint8_t> data;

    // Leaf hash
    data.insert(data.end(), leaf_hash.begin(), leaf_hash.end());

    // Path length and nodes
    data.push_back(static_cast<uint8_t>(path.size()));
    for (const auto& node : path) {
        auto node_data = node.serialize();
        data.insert(data.end(), node_data.begin(), node_data.end());
    }

    // Root
    data.insert(data.end(), root.begin(), root.end());

    return data;
}

std::optional<MerkleProof> MerkleProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 65) return std::nullopt; // 32 + 1 + 32 minimum

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    MerkleProof proof;

    // Leaf hash
    std::copy(ptr, ptr + 32, proof.leaf_hash.begin());
    ptr += 32; remaining -= 32;

    // Path
    if (remaining < 1) return std::nullopt;
    size_t path_len = *ptr++;
    remaining--;

    for (size_t i = 0; i < path_len; i++) {
        if (remaining < 33) return std::nullopt;
        std::vector<uint8_t> node_data(ptr, ptr + 33);
        auto node = MerkleNode::deserialize(node_data);
        if (!node) return std::nullopt;
        proof.path.push_back(*node);
        ptr += 33; remaining -= 33;
    }

    // Root
    if (remaining < 32) return std::nullopt;
    std::copy(ptr, ptr + 32, proof.root.begin());

    return proof;
}

// ============================================================================
// AssetInclusionProof Implementation
// ============================================================================

bool AssetInclusionProof::verify(const std::vector<uint8_t>& script_pubkey) const {
    // Verify the script path proof leads to expected root
    if (!script_path_proof.verify()) {
        return false;
    }

    // Verify the Taproot commitment
    // The output should be a P2TR with the commitment
    if (script_pubkey.size() != 34) return false; // OP_1 OP_PUSH32 [32-byte pubkey]
    if (script_pubkey[0] != 0x51 || script_pubkey[1] != 0x20) return false;

    // The 32-byte pubkey should match internal_key tweaked with the Merkle root
    // This is a simplified check - full implementation would verify the tweak
    return true;
}

std::vector<uint8_t> AssetInclusionProof::serialize() const {
    std::vector<uint8_t> data;

    // Asset info
    data.insert(data.end(), asset_id.begin(), asset_id.end());
    writeLE64(data, amount);
    data.insert(data.end(), state_hash.begin(), state_hash.end());

    // Taproot proof
    data.insert(data.end(), internal_key.begin(), internal_key.end());
    writeVarBytes(data, control_block);

    // Script path proof
    auto proof_data = script_path_proof.serialize();
    writeVarBytes(data, proof_data);

    // Output reference
    writeVarString(data, txid);
    writeLE32(data, vout);

    return data;
}

std::optional<AssetInclusionProof> AssetInclusionProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 8 + 32 + 32) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    AssetInclusionProof proof;

    // Asset info
    std::copy(ptr, ptr + 32, proof.asset_id.begin());
    ptr += 32; remaining -= 32;

    proof.amount = readLE64(ptr);
    ptr += 8; remaining -= 8;

    std::copy(ptr, ptr + 32, proof.state_hash.begin());
    ptr += 32; remaining -= 32;

    // Internal key
    if (remaining < 32) return std::nullopt;
    std::copy(ptr, ptr + 32, proof.internal_key.begin());
    ptr += 32; remaining -= 32;

    // Control block
    proof.control_block = readVarBytes(ptr, remaining);

    // Script path proof
    auto proof_bytes = readVarBytes(ptr, remaining);
    auto script_proof = MerkleProof::deserialize(proof_bytes);
    if (!script_proof) return std::nullopt;
    proof.script_path_proof = *script_proof;

    // Output reference
    proof.txid = readVarString(ptr, remaining);
    if (remaining < 4) return std::nullopt;
    proof.vout = readLE32(ptr);

    return proof;
}

// ============================================================================
// StateTransitionProof Implementation
// ============================================================================

bool StateTransitionProof::verify() const {
    // Verify the previous state proof
    if (!prev_state_proof.verify()) {
        return false;
    }

    // Verify the transition results in new state
    std::vector<uint8_t> transition_preimage;
    transition_preimage.insert(transition_preimage.end(), prev_state_root.begin(), prev_state_root.end());
    transition_preimage.insert(transition_preimage.end(), transition_data.begin(), transition_data.end());

    auto computed_transition_hash = sha256_single(transition_preimage);
    if (computed_transition_hash != transition_hash) {
        return false;
    }

    return true;
}

bool StateTransitionProof::verifyAuthorization() const {
    // CSFS verification - verify signature over transition hash
    if (signature.empty() || pubkey.empty()) {
        return false;
    }

    // Actual signature verification would use secp256k1
    // This is a placeholder for the verification logic
    return signature.size() == 64 && pubkey.size() == 32;
}

std::vector<uint8_t> StateTransitionProof::serialize() const {
    std::vector<uint8_t> data;

    // Previous state
    data.insert(data.end(), prev_state_root.begin(), prev_state_root.end());
    auto prev_proof_data = prev_state_proof.serialize();
    writeVarBytes(data, prev_proof_data);

    // New state
    data.insert(data.end(), new_state_root.begin(), new_state_root.end());

    // Transition data
    writeVarBytes(data, transition_data);
    data.insert(data.end(), transition_hash.begin(), transition_hash.end());

    // Authorization
    writeVarBytes(data, signature);
    writeVarBytes(data, pubkey);

    // CTV hash
    data.insert(data.end(), ctv_hash.begin(), ctv_hash.end());

    return data;
}

std::optional<StateTransitionProof> StateTransitionProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    StateTransitionProof proof;

    // Previous state root
    std::copy(ptr, ptr + 32, proof.prev_state_root.begin());
    ptr += 32; remaining -= 32;

    // Previous state proof
    auto prev_proof_bytes = readVarBytes(ptr, remaining);
    auto prev_proof = MerkleProof::deserialize(prev_proof_bytes);
    if (!prev_proof) return std::nullopt;
    proof.prev_state_proof = *prev_proof;

    // New state root
    if (remaining < 32) return std::nullopt;
    std::copy(ptr, ptr + 32, proof.new_state_root.begin());
    ptr += 32; remaining -= 32;

    // Transition data
    proof.transition_data = readVarBytes(ptr, remaining);

    // Transition hash
    if (remaining < 32) return std::nullopt;
    std::copy(ptr, ptr + 32, proof.transition_hash.begin());
    ptr += 32; remaining -= 32;

    // Authorization
    proof.signature = readVarBytes(ptr, remaining);
    proof.pubkey = readVarBytes(ptr, remaining);

    // CTV hash
    if (remaining < 32) return std::nullopt;
    std::copy(ptr, ptr + 32, proof.ctv_hash.begin());

    return proof;
}

// ============================================================================
// AssetTransferProof Implementation
// ============================================================================

bool AssetTransferProof::verifyConservation() const {
    // Sum inputs by asset type
    std::map<AssetID, uint64_t> input_totals;
    for (const auto& source : sources) {
        input_totals[source.inclusion.asset_id] += source.inclusion.amount;
    }

    // Sum outputs by asset type
    std::map<AssetID, uint64_t> output_totals;
    for (const auto& dest : destinations) {
        output_totals[dest.asset_id] += dest.amount;
    }

    // Check conservation: inputs >= outputs for each asset type
    for (const auto& [asset_id, input_total] : input_totals) {
        auto it = output_totals.find(asset_id);
        uint64_t output_total = (it != output_totals.end()) ? it->second : 0;

        if (output_total > input_total) {
            return false; // Inflation detected
        }
    }

    // Check no new asset types created
    for (const auto& [asset_id, output_total] : output_totals) {
        if (input_totals.find(asset_id) == input_totals.end()) {
            return false; // New asset type without source
        }
    }

    return true;
}

bool AssetTransferProof::verify() const {
    // Verify conservation
    if (!verifyConservation()) {
        return false;
    }

    // Verify each source inclusion proof
    for (const auto& source : sources) {
        // The inclusion proof verification would need the actual scriptPubKey
        // This is a simplified check
        if (source.witness.empty()) {
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> AssetTransferProof::serialize() const {
    std::vector<uint8_t> data;

    // Sources
    data.push_back(static_cast<uint8_t>(sources.size()));
    for (const auto& source : sources) {
        auto inclusion_data = source.inclusion.serialize();
        writeVarBytes(data, inclusion_data);
        writeVarBytes(data, source.witness);
    }

    // Destinations
    data.push_back(static_cast<uint8_t>(destinations.size()));
    for (const auto& dest : destinations) {
        data.insert(data.end(), dest.asset_id.begin(), dest.asset_id.end());
        writeLE64(data, dest.amount);
        data.insert(data.end(), dest.commitment.begin(), dest.commitment.end());
    }

    return data;
}

std::optional<AssetTransferProof> AssetTransferProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 2) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    AssetTransferProof proof;

    // Sources
    size_t source_count = *ptr++;
    remaining--;

    for (size_t i = 0; i < source_count; i++) {
        SourceProof source;

        auto inclusion_data = readVarBytes(ptr, remaining);
        auto inclusion = AssetInclusionProof::deserialize(inclusion_data);
        if (!inclusion) return std::nullopt;
        source.inclusion = *inclusion;

        source.witness = readVarBytes(ptr, remaining);

        proof.sources.push_back(source);
    }

    // Destinations
    if (remaining < 1) return std::nullopt;
    size_t dest_count = *ptr++;
    remaining--;

    for (size_t i = 0; i < dest_count; i++) {
        DestProof dest;

        if (remaining < 32 + 8 + 32) return std::nullopt;

        std::copy(ptr, ptr + 32, dest.asset_id.begin());
        ptr += 32; remaining -= 32;

        dest.amount = readLE64(ptr);
        ptr += 8; remaining -= 8;

        std::copy(ptr, ptr + 32, dest.commitment.begin());
        ptr += 32; remaining -= 32;

        proof.destinations.push_back(dest);
    }

    return proof;
}

// ============================================================================
// MintProof Implementation
// ============================================================================

bool MintProof::verifyAuthorization() const {
    // Verify CSFS signature over mint parameters
    if (mint_authority_pubkey.empty() || authorization_sig.empty()) {
        return false;
    }

    // Full implementation would verify the signature
    return mint_authority_pubkey.size() == 32 && authorization_sig.size() == 64;
}

std::vector<uint8_t> MintProof::serialize() const {
    std::vector<uint8_t> data;

    data.insert(data.end(), asset_id.begin(), asset_id.end());
    writeLE64(data, mint_amount);
    writeVarString(data, genesis_txid);
    writeLE32(data, genesis_vout);
    writeVarBytes(data, mint_authority_pubkey);
    writeVarBytes(data, authorization_sig);

    return data;
}

std::optional<MintProof> MintProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 8) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    MintProof proof;

    std::copy(ptr, ptr + 32, proof.asset_id.begin());
    ptr += 32; remaining -= 32;

    proof.mint_amount = readLE64(ptr);
    ptr += 8; remaining -= 8;

    proof.genesis_txid = readVarString(ptr, remaining);

    if (remaining < 4) return std::nullopt;
    proof.genesis_vout = readLE32(ptr);
    ptr += 4; remaining -= 4;

    proof.mint_authority_pubkey = readVarBytes(ptr, remaining);
    proof.authorization_sig = readVarBytes(ptr, remaining);

    return proof;
}

// ============================================================================
// BurnProof Implementation
// ============================================================================

bool BurnProof::verify() const {
    // Verify the source exists
    // Full implementation would check the inclusion proof
    return burn_amount > 0;
}

std::vector<uint8_t> BurnProof::serialize() const {
    std::vector<uint8_t> data;

    data.insert(data.end(), asset_id.begin(), asset_id.end());
    writeLE64(data, burn_amount);

    auto source_data = source.serialize();
    writeVarBytes(data, source_data);

    writeVarBytes(data, burn_authority_pubkey);
    writeVarBytes(data, authorization_sig);

    return data;
}

std::optional<BurnProof> BurnProof::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 8) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    BurnProof proof;

    std::copy(ptr, ptr + 32, proof.asset_id.begin());
    ptr += 32; remaining -= 32;

    proof.burn_amount = readLE64(ptr);
    ptr += 8; remaining -= 8;

    auto source_data = readVarBytes(ptr, remaining);
    auto source = AssetInclusionProof::deserialize(source_data);
    if (!source) return std::nullopt;
    proof.source = *source;

    proof.burn_authority_pubkey = readVarBytes(ptr, remaining);
    proof.authorization_sig = readVarBytes(ptr, remaining);

    return proof;
}

// ============================================================================
// AggregateProof Implementation
// ============================================================================

bool AggregateProof::verifyAll() const {
    // Verify all transfers
    for (const auto& transfer : transfers) {
        if (!transfer.verify()) return false;
    }

    // Verify all mints
    for (const auto& mint : mints) {
        if (!mint.verifyAuthorization()) return false;
    }

    // Verify all burns
    for (const auto& burn : burns) {
        if (!burn.verify()) return false;
    }

    return true;
}

std::array<uint8_t, 32> AggregateProof::computeCommitment() const {
    std::vector<uint8_t> data;

    // Count
    writeLE32(data, static_cast<uint32_t>(transfers.size()));
    writeLE32(data, static_cast<uint32_t>(mints.size()));
    writeLE32(data, static_cast<uint32_t>(burns.size()));

    // Hash each proof and add to commitment
    for (const auto& transfer : transfers) {
        auto serialized = transfer.serialize();
        auto hash = sha256_single(serialized);
        data.insert(data.end(), hash.begin(), hash.end());
    }

    for (const auto& mint : mints) {
        auto serialized = mint.serialize();
        auto hash = sha256_single(serialized);
        data.insert(data.end(), hash.begin(), hash.end());
    }

    for (const auto& burn : burns) {
        auto serialized = burn.serialize();
        auto hash = sha256_single(serialized);
        data.insert(data.end(), hash.begin(), hash.end());
    }

    return sha256_single(data);
}

// ============================================================================
// Merkle Tree Utilities
// ============================================================================

std::array<uint8_t, 32> BuildMerkleTree(
    const std::vector<std::array<uint8_t, 32>>& leaves) {

    if (leaves.empty()) {
        return std::array<uint8_t, 32>{};
    }

    if (leaves.size() == 1) {
        return leaves[0];
    }

    std::vector<std::array<uint8_t, 32>> current_level = leaves;

    while (current_level.size() > 1) {
        std::vector<std::array<uint8_t, 32>> next_level;

        for (size_t i = 0; i < current_level.size(); i += 2) {
            std::vector<uint8_t> combined;

            if (i + 1 < current_level.size()) {
                // Pair exists - combine in sorted order (Taproot style)
                if (current_level[i] < current_level[i + 1]) {
                    combined.insert(combined.end(), current_level[i].begin(), current_level[i].end());
                    combined.insert(combined.end(), current_level[i + 1].begin(), current_level[i + 1].end());
                } else {
                    combined.insert(combined.end(), current_level[i + 1].begin(), current_level[i + 1].end());
                    combined.insert(combined.end(), current_level[i].begin(), current_level[i].end());
                }
            } else {
                // Odd element - promote to next level
                next_level.push_back(current_level[i]);
                continue;
            }

            next_level.push_back(sha256_single(combined));
        }

        current_level = next_level;
    }

    return current_level[0];
}

MerkleProof GenerateMerkleProof(
    const std::vector<std::array<uint8_t, 32>>& leaves,
    size_t leaf_index) {

    MerkleProof proof;

    if (leaves.empty() || leaf_index >= leaves.size()) {
        return proof;
    }

    proof.leaf_hash = leaves[leaf_index];

    std::vector<std::array<uint8_t, 32>> current_level = leaves;
    size_t current_index = leaf_index;

    while (current_level.size() > 1) {
        std::vector<std::array<uint8_t, 32>> next_level;

        for (size_t i = 0; i < current_level.size(); i += 2) {
            if (i + 1 < current_level.size()) {
                // Add sibling to proof if we're at the relevant pair
                if (i == current_index || i + 1 == current_index) {
                    MerkleNode node;
                    node.is_left = (current_index == i + 1);

                    if (node.is_left) {
                        node.hash = current_level[i];
                    } else {
                        node.hash = current_level[i + 1];
                    }

                    proof.path.push_back(node);

                    // Update index for next level
                    current_index = i / 2;
                }

                // Compute parent (sorted for Taproot)
                std::vector<uint8_t> combined;
                if (current_level[i] < current_level[i + 1]) {
                    combined.insert(combined.end(), current_level[i].begin(), current_level[i].end());
                    combined.insert(combined.end(), current_level[i + 1].begin(), current_level[i + 1].end());
                } else {
                    combined.insert(combined.end(), current_level[i + 1].begin(), current_level[i + 1].end());
                    combined.insert(combined.end(), current_level[i].begin(), current_level[i].end());
                }
                next_level.push_back(sha256_single(combined));
            } else {
                // Odd element
                if (i == current_index) {
                    current_index = next_level.size();
                }
                next_level.push_back(current_level[i]);
            }
        }

        current_level = next_level;
    }

    proof.root = current_level[0];

    return proof;
}

std::array<uint8_t, 32> TaggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data) {

    // BIP-340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data)
    std::vector<uint8_t> tag_bytes(tag.begin(), tag.end());
    auto tag_hash = sha256_single(tag_bytes);

    std::vector<uint8_t> preimage;
    preimage.insert(preimage.end(), tag_hash.begin(), tag_hash.end());
    preimage.insert(preimage.end(), tag_hash.begin(), tag_hash.end());
    preimage.insert(preimage.end(), data.begin(), data.end());

    return sha256_single(preimage);
}

std::array<uint8_t, 32> TaprootLeafHash(
    uint8_t version,
    const std::vector<uint8_t>& script) {

    std::vector<uint8_t> leaf_data;
    leaf_data.push_back(version);

    // Script length as varint
    if (script.size() < 0xFD) {
        leaf_data.push_back(static_cast<uint8_t>(script.size()));
    } else {
        leaf_data.push_back(0xFD);
        leaf_data.push_back(static_cast<uint8_t>(script.size() & 0xFF));
        leaf_data.push_back(static_cast<uint8_t>((script.size() >> 8) & 0xFF));
    }
    leaf_data.insert(leaf_data.end(), script.begin(), script.end());

    return TaggedHash("TapLeaf", leaf_data);
}

std::array<uint8_t, 32> TaprootBranchHash(
    const std::array<uint8_t, 32>& left,
    const std::array<uint8_t, 32>& right) {

    std::vector<uint8_t> branch_data;

    // Lexicographically sort
    if (left < right) {
        branch_data.insert(branch_data.end(), left.begin(), left.end());
        branch_data.insert(branch_data.end(), right.begin(), right.end());
    } else {
        branch_data.insert(branch_data.end(), right.begin(), right.end());
        branch_data.insert(branch_data.end(), left.begin(), left.end());
    }

    return TaggedHash("TapBranch", branch_data);
}

} // namespace assets
} // namespace dinero
