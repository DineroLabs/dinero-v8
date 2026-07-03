/**
 * @file utreexo_proof_relay.cpp
 * @brief Phase 34.4: Utreexo Proof Relay Implementation
 *
 * Deterministic serialization for Utreexo proof relay.
 */

#include "consensus/utreexo_proof_relay.h"
#include <cstring>
#include <algorithm>

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════
// Serialization Helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace detail {

size_t WriteCompactSize(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 253) {
        out.push_back(static_cast<uint8_t>(value));
        return 1;
    } else if (value <= 0xFFFF) {
        out.push_back(253);
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
        return 3;
    } else if (value <= 0xFFFFFFFF) {
        out.push_back(254);
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 24));
        return 5;
    } else {
        out.push_back(255);
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>(value >> (i * 8)));
        }
        return 9;
    }
}

bool ReadCompactSize(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
    if (offset >= size) return false;

    uint8_t first = data[offset++];

    if (first < 253) {
        value = first;
        return true;
    } else if (first == 253) {
        if (offset + 2 > size) return false;
        value = data[offset] | (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    } else if (first == 254) {
        if (offset + 4 > size) return false;
        value = data[offset] |
                (static_cast<uint64_t>(data[offset + 1]) << 8) |
                (static_cast<uint64_t>(data[offset + 2]) << 16) |
                (static_cast<uint64_t>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    } else {  // 255
        if (offset + 8 > size) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
        }
        offset += 8;
        return true;
    }
}

void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

bool ReadUint32LE(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
    if (offset + 4 > size) return false;
    value = data[offset] |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool ReadUint64LE(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
    if (offset + 8 > size) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
    }
    offset += 8;
    return true;
}

size_t CompactSizeSize(uint64_t value) {
    if (value < 253) return 1;
    if (value <= 0xFFFF) return 3;
    if (value <= 0xFFFFFFFF) return 5;
    return 9;
}

// Helper: Convert hex string to bytes (32 bytes for txid)
static bool HexToBytes32(const std::string& hex, uint8_t* out) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        std::string byteStr = hex.substr(i * 2, 2);
        char* endptr;
        long val = std::strtol(byteStr.c_str(), &endptr, 16);
        if (*endptr != '\0') return false;
        out[i] = static_cast<uint8_t>(val);
    }
    return true;
}

// Helper: Convert bytes to hex string
static std::string BytesToHex32(const uint8_t* data) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < 32; ++i) {
        result += hex_chars[(data[i] >> 4) & 0x0F];
        result += hex_chars[data[i] & 0x0F];
    }
    return result;
}

} // namespace detail

// ═══════════════════════════════════════════════════════════════════════════
// TxInProof Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> TxInProof::Serialize() const {
    std::vector<uint8_t> out;
    out.reserve(GetSerializeSize());

    // txid: 32 bytes (natural order)
    out.insert(out.end(), outpoint.txid.AsUint256().data, outpoint.txid.AsUint256().data + 32);

    // vout: 4 bytes LE
    detail::WriteUint32LE(out, outpoint.vout);

    // position: 8 bytes LE
    detail::WriteUint64LE(out, proof.position);

    // numLeaves: 8 bytes LE
    detail::WriteUint64LE(out, proof.numLeaves);

    // numSiblings: CompactSize
    detail::WriteCompactSize(out, proof.siblings.size());

    // siblings: 32 bytes each
    for (const auto& sibling : proof.siblings) {
        if (sibling.size() == 32) {
            out.insert(out.end(), sibling.begin(), sibling.end());
        } else {
            // Invalid sibling - pad with zeros
            out.insert(out.end(), 32, 0);
        }
    }

    return out;
}

bool TxInProof::Deserialize(const uint8_t* data, size_t size, TxInProof& out, size_t& bytesRead) {
    size_t offset = 0;

    // txid: 32 bytes
    if (offset + 32 > size) return false;
    uint256 txid_hash;
    std::memcpy(txid_hash.data, data + offset, 32);
    out.outpoint.txid = TxId(txid_hash);
    offset += 32;

    // vout: 4 bytes LE
    if (!detail::ReadUint32LE(data, size, offset, out.outpoint.vout)) return false;

    // position: 8 bytes LE
    if (!detail::ReadUint64LE(data, size, offset, out.proof.position)) return false;

    // numLeaves: 8 bytes LE
    if (!detail::ReadUint64LE(data, size, offset, out.proof.numLeaves)) return false;

    // numSiblings: CompactSize
    uint64_t numSiblings;
    if (!detail::ReadCompactSize(data, size, offset, numSiblings)) return false;

    // Sanity check: max 64 siblings (for trees up to 2^64 leaves)
    if (numSiblings > 64) return false;

    // siblings: 32 bytes each
    out.proof.siblings.clear();
    out.proof.siblings.reserve(numSiblings);
    for (uint64_t i = 0; i < numSiblings; ++i) {
        if (offset + 32 > size) return false;
        UtreexoHash sibling(data + offset, data + offset + 32);
        out.proof.siblings.push_back(std::move(sibling));
        offset += 32;
    }

    bytesRead = offset;
    return true;
}

size_t TxInProof::GetSerializeSize() const {
    return 32 +  // txid
           4 +   // vout
           8 +   // position
           8 +   // numLeaves
           detail::CompactSizeSize(proof.siblings.size()) +
           proof.siblings.size() * 32;
}

bool TxInProof::Verify(const std::vector<UtreexoHash>& roots,
                       uint64_t amount,
                       const std::vector<uint8_t>& scriptPubKey) const {
    // Compute leaf hash for this UTXO
    UtreexoHash computedLeaf = HashUTXOLegacy(outpoint.txid.AsUint256(), outpoint.vout, amount, scriptPubKey);

    // Use the existing proof verification
    return proof.verify(computedLeaf, roots);
}

bool TxInProof::Verify(const std::vector<UtreexoHash>& roots,
                       uint64_t amount,
                       const std::vector<uint8_t>& scriptPubKey,
                       uint32_t created_height,
                       bool is_coinbase) const {
    UtreexoHash computedLeaf = HashUTXOForCreationHeight(
        outpoint.txid.AsUint256(),
        outpoint.vout,
        amount,
        scriptPubKey,
        created_height,
        is_coinbase);

    return proof.verify(computedLeaf, roots);
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockUtreexoProofs Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockUtreexoProofs::Serialize() const {
    std::vector<uint8_t> out;
    out.reserve(GetSerializeSize());

    // version: 1 byte
    out.push_back(version);

    // blockHash: 32 bytes
    uint8_t hashBytes[32];
    if (detail::HexToBytes32(blockHash, hashBytes)) {
        out.insert(out.end(), hashBytes, hashBytes + 32);
    } else {
        out.insert(out.end(), 32, 0);
    }

    // numProofs: CompactSize
    detail::WriteCompactSize(out, proofs.size());

    // proofs: TxInProof[]
    for (const auto& proof : proofs) {
        auto serialized = proof.Serialize();
        out.insert(out.end(), serialized.begin(), serialized.end());
    }

    return out;
}

bool BlockUtreexoProofs::Deserialize(const uint8_t* data, size_t size, BlockUtreexoProofs& out) {
    size_t offset = 0;

    // version: 1 byte
    if (offset >= size) return false;
    out.version = data[offset++];

    // Check version compatibility
    if (out.version > UTREEXO_PROOF_VERSION) {
        // Future version - we can't parse it
        return false;
    }

    // blockHash: 32 bytes
    if (offset + 32 > size) return false;
    out.blockHash = detail::BytesToHex32(data + offset);
    offset += 32;

    // numProofs: CompactSize
    uint64_t numProofs;
    if (!detail::ReadCompactSize(data, size, offset, numProofs)) return false;

    // Sanity check: max 1M proofs per block
    if (numProofs > 1000000) return false;

    // proofs: TxInProof[]
    out.proofs.clear();
    out.proofs.reserve(numProofs);
    for (uint64_t i = 0; i < numProofs; ++i) {
        TxInProof proof;
        size_t bytesRead;
        if (!TxInProof::Deserialize(data + offset, size - offset, proof, bytesRead)) {
            return false;
        }
        out.proofs.push_back(std::move(proof));
        offset += bytesRead;
    }

    return true;
}

bool BlockUtreexoProofs::Deserialize(const std::vector<uint8_t>& data, BlockUtreexoProofs& out) {
    return Deserialize(data.data(), data.size(), out);
}

size_t BlockUtreexoProofs::GetSerializeSize() const {
    size_t size = 1 +  // version
                  32 + // blockHash
                  detail::CompactSizeSize(proofs.size());

    for (const auto& proof : proofs) {
        size += proof.GetSerializeSize();
    }

    return size;
}

BlockUtreexoProofs::Stats BlockUtreexoProofs::GetStats() const {
    Stats stats;
    stats.numProofs = proofs.size();
    stats.totalSiblings = 0;
    for (const auto& proof : proofs) {
        stats.totalSiblings += proof.proof.siblings.size();
    }
    stats.serializedSize = GetSerializeSize();
    stats.avgProofSize = proofs.empty() ? 0.0 :
        static_cast<double>(stats.serializedSize - 33) / proofs.size();
    return stats;
}

} // namespace consensus
} // namespace dinero
