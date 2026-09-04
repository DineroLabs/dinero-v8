/**
 * Phase 34.1: Utreexo Core Accumulator Implementation
 *
 * This implements a dynamic hash-based accumulator using Merkle forests.
 * Based on the MIT Utreexo paper (https://eprint.iacr.org/2019/611.pdf)
 *
 * Key insight: Any number N can be represented as sum of powers of 2.
 * Each power of 2 is a perfect binary Merkle tree.
 * Example: 5 UTXOs = 4 + 1 = tree(4) + tree(1) = 2 roots
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_canonical_roots_activation.h"  // Apr 13 2026 Stage 3 fork — cloneForHeight
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/utreexo_stump.h"
#include "consensus/script_interpreter.h"  // For SHA256_Hash
#include "consensus/outpoint.h"            // For OutPoint (ephemeral UTXO scan)
#include "primitives/block.h"
#include "primitives/transaction.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <set>
#include <map>
#include <iostream>
#include <limits>

// Phase 9.2: zstd compression
#include "third_party/zstd/lib/zstd.h"

namespace dinero {
namespace consensus {

namespace {

bool checked_add_size_t(size_t a, size_t b, size_t& out) {
    if (a > std::numeric_limits<size_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_mul_size_t(size_t a, size_t b, size_t& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a > std::numeric_limits<size_t>::max() / b) {
        return false;
    }
    out = a * b;
    return true;
}

bool read_u32_le_at(const std::vector<uint8_t>& data, size_t offset, uint32_t& out) {
    size_t end = 0;
    if (!checked_add_size_t(offset, 4, end) || end > data.size()) {
        return false;
    }
    out = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

bool parse_block_proof_end(const std::vector<uint8_t>& data, size_t proof_offset, size_t& proof_end) {
    if (proof_offset >= data.size()) {
        return false;
    }

    size_t cursor = proof_offset;
    const uint8_t version = data[cursor];

    // Version 4/5/6: [version][numLeaves][numTargets][targets][positions][numProofHashes][proofHashes]
    if (version == 4 || version == 5 || version == 6) {
        if (!checked_add_size_t(cursor, 1, cursor)) return false;   // version
        if (!checked_add_size_t(cursor, 8, cursor)) return false;   // numLeaves

        uint32_t num_targets = 0;
        if (!read_u32_le_at(data, cursor, num_targets)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        size_t targets_bytes = 0;
        size_t positions_bytes = 0;
        if (!checked_mul_size_t(static_cast<size_t>(num_targets), 32, targets_bytes)) return false;
        if (!checked_mul_size_t(static_cast<size_t>(num_targets), 8, positions_bytes)) return false;

        if (!checked_add_size_t(cursor, targets_bytes, cursor)) return false;
        if (!checked_add_size_t(cursor, positions_bytes, cursor)) return false;

        uint32_t num_proof_hashes = 0;
        if (!read_u32_le_at(data, cursor, num_proof_hashes)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        size_t proof_hash_bytes = 0;
        if (!checked_mul_size_t(static_cast<size_t>(num_proof_hashes), 32, proof_hash_bytes)) return false;
        if (!checked_add_size_t(cursor, proof_hash_bytes, cursor)) return false;

        if (cursor > data.size()) {
            return false;
        }
        proof_end = cursor;
        return true;
    }

    // Version 2 (compressed dictionary format):
    // [version][dict_size][dict hashes][num_targets][target_idx][num_proofs][proof_idx]
    if (version == 2) {
        if (!checked_add_size_t(cursor, 1, cursor)) return false;  // version

        uint32_t dict_size = 0;
        if (!read_u32_le_at(data, cursor, dict_size)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        size_t dict_bytes = 0;
        if (!checked_mul_size_t(static_cast<size_t>(dict_size), 32, dict_bytes)) return false;
        if (!checked_add_size_t(cursor, dict_bytes, cursor)) return false;

        uint32_t num_targets = 0;
        if (!read_u32_le_at(data, cursor, num_targets)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        size_t target_idx_bytes = 0;
        if (!checked_mul_size_t(static_cast<size_t>(num_targets), 4, target_idx_bytes)) return false;
        if (!checked_add_size_t(cursor, target_idx_bytes, cursor)) return false;

        uint32_t num_proofs = 0;
        if (!read_u32_le_at(data, cursor, num_proofs)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        size_t proof_idx_bytes = 0;
        if (!checked_mul_size_t(static_cast<size_t>(num_proofs), 4, proof_idx_bytes)) return false;
        if (!checked_add_size_t(cursor, proof_idx_bytes, cursor)) return false;

        if (cursor > data.size()) {
            return false;
        }
        proof_end = cursor;
        return true;
    }

    // Version 3 (zstd framed):
    // [version][uncompressed_size][compressed_size][zstd_data]
    if (version == 3) {
        if (!checked_add_size_t(cursor, 1, cursor)) return false;  // version

        uint32_t uncompressed_size = 0;
        if (!read_u32_le_at(data, cursor, uncompressed_size)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        uint32_t compressed_size = 0;
        if (!read_u32_le_at(data, cursor, compressed_size)) return false;
        if (!checked_add_size_t(cursor, 4, cursor)) return false;

        // Policy limit to avoid oversized allocations/decompression bombs.
        if (uncompressed_size > MAX_UTREEXO_PROOF_BYTES) {
            return false;
        }

        if (!checked_add_size_t(cursor, static_cast<size_t>(compressed_size), cursor)) return false;
        if (cursor > data.size()) {
            return false;
        }
        proof_end = cursor;
        return true;
    }

    // Legacy format (no explicit version):
    // [numTargets][targets][numProofHashes][proofHashes]
    uint32_t num_targets = 0;
    if (!read_u32_le_at(data, cursor, num_targets)) return false;
    if (!checked_add_size_t(cursor, 4, cursor)) return false;

    size_t targets_bytes = 0;
    if (!checked_mul_size_t(static_cast<size_t>(num_targets), 32, targets_bytes)) return false;
    if (!checked_add_size_t(cursor, targets_bytes, cursor)) return false;

    uint32_t num_proof_hashes = 0;
    if (!read_u32_le_at(data, cursor, num_proof_hashes)) return false;
    if (!checked_add_size_t(cursor, 4, cursor)) return false;

    size_t proof_hash_bytes = 0;
    if (!checked_mul_size_t(static_cast<size_t>(num_proof_hashes), 32, proof_hash_bytes)) return false;
    if (!checked_add_size_t(cursor, proof_hash_bytes, cursor)) return false;

    if (cursor > data.size()) {
        return false;
    }
    proof_end = cursor;
    return true;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Hash Functions
// ═══════════════════════════════════════════════════════════════════════════

UtreexoHash HashNode(const UtreexoHash& left, const UtreexoHash& right) {
    // Domain-separated internal node hash (consensus-critical):
    // SHA256("DINERO-UTREEXO-NODE-v1" || left || right)
    // Domain tag prevents second-preimage attacks where a leaf hash
    // could collide with an internal node hash.
    static const char* DOMAIN_TAG = "DINERO-UTREEXO-NODE-v1"; // 22 bytes
    std::vector<uint8_t> combined;
    combined.reserve(22 + 64);
    combined.insert(combined.end(), DOMAIN_TAG, DOMAIN_TAG + 22);
    combined.insert(combined.end(), left.begin(), left.end());
    combined.insert(combined.end(), right.begin(), right.end());

    return SHA256_Hash(combined);
}

UtreexoHash HashUTXOLegacy(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey) {
    // Domain-separated UTXO leaf hash (consensus-critical, active from genesis):
    // SHA256("DINERO-UTXO-LEAF-v1" || txid || vout || amount || scriptPubKey)
    // See: DINERO-UTREEXO-SPEC.md §3
    static const char* DOMAIN_TAG = "DINERO-UTXO-LEAF-v1"; // 19 bytes
    std::vector<uint8_t> data;

    // 0. Domain separation tag (19 bytes)
    data.insert(data.end(), DOMAIN_TAG, DOMAIN_TAG + 19);

    // 1. txid (32 bytes) - already in binary format
    data.insert(data.end(), txid.data, txid.data + 32);

    // 2. vout (4 bytes, little-endian)
    data.push_back(vout & 0xFF);
    data.push_back((vout >> 8) & 0xFF);
    data.push_back((vout >> 16) & 0xFF);
    data.push_back((vout >> 24) & 0xFF);

    // 3. amount (8 bytes, little-endian)
    data.push_back(amount & 0xFF);
    data.push_back((amount >> 8) & 0xFF);
    data.push_back((amount >> 16) & 0xFF);
    data.push_back((amount >> 24) & 0xFF);
    data.push_back((amount >> 32) & 0xFF);
    data.push_back((amount >> 40) & 0xFF);
    data.push_back((amount >> 48) & 0xFF);
    data.push_back((amount >> 56) & 0xFF);

    // 4. scriptPubKey length (CompactSize varint)
    uint64_t scriptLen = scriptPubKey.size();
    if (scriptLen < 253) {
        data.push_back(static_cast<uint8_t>(scriptLen));
    } else if (scriptLen <= 0xFFFF) {
        data.push_back(0xFD);
        data.push_back(scriptLen & 0xFF);
        data.push_back((scriptLen >> 8) & 0xFF);
    } else {
        data.push_back(0xFE);
        data.push_back(scriptLen & 0xFF);
        data.push_back((scriptLen >> 8) & 0xFF);
        data.push_back((scriptLen >> 16) & 0xFF);
        data.push_back((scriptLen >> 24) & 0xFF);
    }

    // 5. scriptPubKey (variable length)
    data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());

    return SHA256_Hash(data);
}

UtreexoHash HashUTXOV2(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey,
                 uint32_t created_height, bool is_coinbase) {
    // Experimental maturity-bound UTXO leaf hash:
    // SHA256("DINERO-UTXO-LEAF-v2" || txid || vout || amount ||
    //        scriptPubKey || created_height || flags)
    static const char* DOMAIN_TAG = "DINERO-UTXO-LEAF-v2"; // 19 bytes
    std::vector<uint8_t> data;

    data.insert(data.end(), DOMAIN_TAG, DOMAIN_TAG + 19);
    data.insert(data.end(), txid.data, txid.data + 32);

    data.push_back(vout & 0xFF);
    data.push_back((vout >> 8) & 0xFF);
    data.push_back((vout >> 16) & 0xFF);
    data.push_back((vout >> 24) & 0xFF);

    data.push_back(amount & 0xFF);
    data.push_back((amount >> 8) & 0xFF);
    data.push_back((amount >> 16) & 0xFF);
    data.push_back((amount >> 24) & 0xFF);
    data.push_back((amount >> 32) & 0xFF);
    data.push_back((amount >> 40) & 0xFF);
    data.push_back((amount >> 48) & 0xFF);
    data.push_back((amount >> 56) & 0xFF);

    uint64_t scriptLen = scriptPubKey.size();
    if (scriptLen < 253) {
        data.push_back(static_cast<uint8_t>(scriptLen));
    } else if (scriptLen <= 0xFFFF) {
        data.push_back(0xFD);
        data.push_back(scriptLen & 0xFF);
        data.push_back((scriptLen >> 8) & 0xFF);
    } else {
        data.push_back(0xFE);
        data.push_back(scriptLen & 0xFF);
        data.push_back((scriptLen >> 8) & 0xFF);
        data.push_back((scriptLen >> 16) & 0xFF);
        data.push_back((scriptLen >> 24) & 0xFF);
    }
    data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());

    data.push_back(created_height & 0xFF);
    data.push_back((created_height >> 8) & 0xFF);
    data.push_back((created_height >> 16) & 0xFF);
    data.push_back((created_height >> 24) & 0xFF);

    uint8_t flags = is_coinbase ? 0x01 : 0x00;
    data.push_back(flags);

    return SHA256_Hash(data);
}

UtreexoHash HashUTXOForCreationHeight(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey,
                 uint32_t created_height, bool is_coinbase) {
    if (IsUtreexoMaturityLeafActive(created_height)) {
        return HashUTXOV2(txid, vout, amount, scriptPubKey, created_height, is_coinbase);
    }
    return HashUTXOLegacy(txid, vout, amount, scriptPubKey);
}

// ═══════════════════════════════════════════════════════════════════════════
// UtreexoProof
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoProof::verify(const UtreexoHash& leafHash,
                         const std::vector<UtreexoHash>& roots) const {
    if (position >= numLeaves) {
        return false;
    }

    // Compute root by hashing up the Merkle path
    UtreexoHash currentHash = leafHash;
    uint64_t currentPos = position;

    // Empty siblings is valid for single-leaf trees
    for (const UtreexoHash& sibling : siblings) {
        if (currentPos % 2 == 0) {
            // We're on the left, sibling is on the right
            currentHash = HashNode(currentHash, sibling);
        } else {
            // We're on the right, sibling is on the left
            currentHash = HashNode(sibling, currentHash);
        }
        currentPos /= 2;
    }

    // Check if computed root matches any of the forest roots
    for (const UtreexoHash& root : roots) {
        if (currentHash == root) {
            return true;
        }
    }

    return false;
}

std::vector<uint8_t> UtreexoProof::serialize() const {
    std::vector<uint8_t> data;

    // 1. Number of siblings (4 bytes)
    uint32_t numSiblings = siblings.size();
    data.push_back(numSiblings & 0xFF);
    data.push_back((numSiblings >> 8) & 0xFF);
    data.push_back((numSiblings >> 16) & 0xFF);
    data.push_back((numSiblings >> 24) & 0xFF);

    // 2. Sibling hashes (32 bytes each)
    for (const UtreexoHash& sibling : siblings) {
        data.insert(data.end(), sibling.begin(), sibling.end());
    }

    // 3. Position (8 bytes)
    data.push_back(position & 0xFF);
    data.push_back((position >> 8) & 0xFF);
    data.push_back((position >> 16) & 0xFF);
    data.push_back((position >> 24) & 0xFF);
    data.push_back((position >> 32) & 0xFF);
    data.push_back((position >> 40) & 0xFF);
    data.push_back((position >> 48) & 0xFF);
    data.push_back((position >> 56) & 0xFF);

    // 4. Number of leaves (8 bytes)
    data.push_back(numLeaves & 0xFF);
    data.push_back((numLeaves >> 8) & 0xFF);
    data.push_back((numLeaves >> 16) & 0xFF);
    data.push_back((numLeaves >> 24) & 0xFF);
    data.push_back((numLeaves >> 32) & 0xFF);
    data.push_back((numLeaves >> 40) & 0xFF);
    data.push_back((numLeaves >> 48) & 0xFF);
    data.push_back((numLeaves >> 56) & 0xFF);

    return data;
}

UtreexoProof UtreexoProof::deserialize(const std::vector<uint8_t>& data) {
    UtreexoProof proof;

    if (data.size() < 20) {  // Min: 4 (numSiblings) + 8 (position) + 8 (numLeaves)
        return proof;
    }

    size_t offset = 0;

    // 1. Number of siblings
    uint32_t numSiblings = data[offset] | (data[offset+1] << 8) |
                          (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // 2. Sibling hashes
    // numSiblings is attacker-controlled (peer-relayed proof); compute the
    // required length in size_t with overflow checks so a crafted count like
    // 0x08000000 can't wrap `numSiblings * 32` to a small value and let the
    // per-sibling loop read off the end of `data` (OOB read → crash/DoS).
    // Matches the checked-multiply pattern used by the other deserializers.
    size_t siblings_bytes = 0;
    size_t required = 0;
    if (!checked_mul_size_t(numSiblings, 32, siblings_bytes) ||
        !checked_add_size_t(offset, siblings_bytes, required) ||
        !checked_add_size_t(required, 16, required) ||
        data.size() < required) {
        return proof;
    }

    for (uint32_t i = 0; i < numSiblings; i++) {
        UtreexoHash sibling(data.begin() + offset, data.begin() + offset + 32);
        proof.siblings.push_back(sibling);
        offset += 32;
    }

    // 3. Position
    proof.position = data[offset] | ((uint64_t)data[offset+1] << 8) |
                    ((uint64_t)data[offset+2] << 16) | ((uint64_t)data[offset+3] << 24) |
                    ((uint64_t)data[offset+4] << 32) | ((uint64_t)data[offset+5] << 40) |
                    ((uint64_t)data[offset+6] << 48) | ((uint64_t)data[offset+7] << 56);
    offset += 8;

    // 4. Number of leaves
    proof.numLeaves = data[offset] | ((uint64_t)data[offset+1] << 8) |
                     ((uint64_t)data[offset+2] << 16) | ((uint64_t)data[offset+3] << 24) |
                     ((uint64_t)data[offset+4] << 32) | ((uint64_t)data[offset+5] << 40) |
                     ((uint64_t)data[offset+6] << 48) | ((uint64_t)data[offset+7] << 56);

    return proof;
}

// ═══════════════════════════════════════════════════════════════════════════
// SpentOutputData (Spent Output Metadata for Stateless Validation)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> SpentOutputData::serialize(uint8_t format_version) const {
    std::vector<uint8_t> data;

    // 1. Value (8 bytes, little-endian)
    data.push_back(value & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 24) & 0xFF);
    data.push_back((value >> 32) & 0xFF);
    data.push_back((value >> 40) & 0xFF);
    data.push_back((value >> 48) & 0xFF);
    data.push_back((value >> 56) & 0xFF);

    // 2. ScriptPubKey length (4 bytes)
    uint32_t spk_len = scriptPubKey.size();
    data.push_back(spk_len & 0xFF);
    data.push_back((spk_len >> 8) & 0xFF);
    data.push_back((spk_len >> 16) & 0xFF);
    data.push_back((spk_len >> 24) & 0xFF);

    // 3. ScriptPubKey bytes
    data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());

    if (format_version >= 6) {
        data.push_back(created_height & 0xFF);
        data.push_back((created_height >> 8) & 0xFF);
        data.push_back((created_height >> 16) & 0xFF);
        data.push_back((created_height >> 24) & 0xFF);
        data.push_back(is_coinbase ? 0x01 : 0x00);
    }

    if (format_version >= 5) {
        // 4. Confidential flags
        data.push_back(is_confidential ? 0x01 : 0x00);

        // 5. Commitment length (4 bytes)
        uint32_t commitment_len = commitment.size();
        data.push_back(commitment_len & 0xFF);
        data.push_back((commitment_len >> 8) & 0xFF);
        data.push_back((commitment_len >> 16) & 0xFF);
        data.push_back((commitment_len >> 24) & 0xFF);

        // 6. Commitment bytes
        data.insert(data.end(), commitment.begin(), commitment.end());
    }

    return data;
}

SpentOutputData SpentOutputData::deserialize(const std::vector<uint8_t>& data, size_t& offset,
                                             uint8_t format_version) {
    SpentOutputData spent;
    const size_t start_offset = offset;

    if (data.size() < offset + 12) {  // Min: 8 (value) + 4 (length)
        offset = start_offset;
        return spent;
    }

    // 1. Value (8 bytes, little-endian)
    spent.value = data[offset] | ((uint64_t)data[offset+1] << 8) |
                  ((uint64_t)data[offset+2] << 16) | ((uint64_t)data[offset+3] << 24) |
                  ((uint64_t)data[offset+4] << 32) | ((uint64_t)data[offset+5] << 40) |
                  ((uint64_t)data[offset+6] << 48) | ((uint64_t)data[offset+7] << 56);
    offset += 8;

    // 2. ScriptPubKey length (4 bytes)
    uint32_t spk_len = data[offset] | (data[offset+1] << 8) |
                      (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // 3. ScriptPubKey bytes
    if (data.size() < offset + spk_len) {
        offset = start_offset;
        return spent;  // Invalid data
    }

    spent.scriptPubKey = std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + spk_len);
    offset += spk_len;

    if (format_version >= 6) {
        if (data.size() < offset + 5) {
            offset = start_offset;
            return spent;
        }

        spent.created_height =
            static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
        offset += 4;
        spent.is_coinbase = (data[offset++] & 0x01) != 0;
    }

    if (format_version >= 5) {
        if (data.size() < offset + 5) {
            offset = start_offset;
            return spent;
        }

        spent.is_confidential = (data[offset++] & 0x01) != 0;

        uint32_t commitment_len = data[offset] | (data[offset+1] << 8) |
                                 (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        if (data.size() < offset + commitment_len) {
            offset = start_offset;
            return spent;
        }

        spent.commitment = std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + commitment_len);
        offset += commitment_len;
    }

    return spent;
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockUtreexoProof (Phase 1: Block-Level Proof Structures)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockUtreexoProof::serialize() const {
    std::vector<uint8_t> data;

    // Version 5: Stateless proof format with positions
    // ─────────────────────────────────────────────────
    // Format:
    // [version: 1 byte = 4]
    // [numLeaves: 8 bytes]
    // [numTargets: 4 bytes]
    // [targets: 32 bytes each]
    // [positions: 8 bytes each, same count as targets]
    // [numProofHashes: 4 bytes]
    // [proof_hashes: 32 bytes each]
    // ─────────────────────────────────────────────────

    // 1. Version (1 byte)
    data.push_back(format_version);  // Version 6 = stateless format + maturity-bound metadata

    // 2. numLeaves (8 bytes) - total leaves in forest at proof time
    data.push_back(numLeaves & 0xFF);
    data.push_back((numLeaves >> 8) & 0xFF);
    data.push_back((numLeaves >> 16) & 0xFF);
    data.push_back((numLeaves >> 24) & 0xFF);
    data.push_back((numLeaves >> 32) & 0xFF);
    data.push_back((numLeaves >> 40) & 0xFF);
    data.push_back((numLeaves >> 48) & 0xFF);
    data.push_back((numLeaves >> 56) & 0xFF);

    // 3. Number of targets (4 bytes)
    uint32_t numTargets = targets.size();
    data.push_back(numTargets & 0xFF);
    data.push_back((numTargets >> 8) & 0xFF);
    data.push_back((numTargets >> 16) & 0xFF);
    data.push_back((numTargets >> 24) & 0xFF);

    // 4. Target hashes (32 bytes each)
    for (const UtreexoHash& target : targets) {
        data.insert(data.end(), target.begin(), target.end());
    }

    // 5. Positions (8 bytes each, same count as targets)
    for (uint64_t pos : positions) {
        data.push_back(pos & 0xFF);
        data.push_back((pos >> 8) & 0xFF);
        data.push_back((pos >> 16) & 0xFF);
        data.push_back((pos >> 24) & 0xFF);
        data.push_back((pos >> 32) & 0xFF);
        data.push_back((pos >> 40) & 0xFF);
        data.push_back((pos >> 48) & 0xFF);
        data.push_back((pos >> 56) & 0xFF);
    }

    // 6. Number of proof hashes (4 bytes)
    uint32_t numProofHashes = proof_hashes.size();
    data.push_back(numProofHashes & 0xFF);
    data.push_back((numProofHashes >> 8) & 0xFF);
    data.push_back((numProofHashes >> 16) & 0xFF);
    data.push_back((numProofHashes >> 24) & 0xFF);

    // 7. Proof hashes (32 bytes each)
    for (const UtreexoHash& proof_hash : proof_hashes) {
        data.insert(data.end(), proof_hash.begin(), proof_hash.end());
    }

    return data;
}

BlockUtreexoProof BlockUtreexoProof::deserialize(const std::vector<uint8_t>& data) {
    BlockUtreexoProof proof;

    // ═══════════════════════════════════════════════════════════════════════
    // Non-consensus DoS protection. Reject before allocating any buffers.
    // Does not affect proof validity - this is POLICY only.
    // ═══════════════════════════════════════════════════════════════════════
    if (data.size() > MAX_UTREEXO_PROOF_BYTES) {
        return proof;  // Reject oversized proof
    }

    if (data.empty()) {
        return proof;
    }

    size_t offset = 0;

    // Check version byte
    uint8_t version = data[0];

    if (version == 4 || version == 5 || version == 6) {
        // ─────────────────────────────────────────────────────────────────────
        // Version 4/5: Stateless proof format with positions
        // ─────────────────────────────────────────────────────────────────────
        offset = 1;  // Skip version byte
        proof.format_version = version;

        // Minimum size: version(1) + numLeaves(8) + numTargets(4) + numProofHashes(4) = 17
        if (data.size() < 17) {
            return proof;
        }

        // 1. numLeaves (8 bytes)
        proof.numLeaves = static_cast<uint64_t>(data[offset]) |
                         (static_cast<uint64_t>(data[offset+1]) << 8) |
                         (static_cast<uint64_t>(data[offset+2]) << 16) |
                         (static_cast<uint64_t>(data[offset+3]) << 24) |
                         (static_cast<uint64_t>(data[offset+4]) << 32) |
                         (static_cast<uint64_t>(data[offset+5]) << 40) |
                         (static_cast<uint64_t>(data[offset+6]) << 48) |
                         (static_cast<uint64_t>(data[offset+7]) << 56);
        offset += 8;

        // 2. Number of targets (4 bytes)
        uint32_t numTargets = data[offset] | (data[offset+1] << 8) |
                             (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // Overflow check: targets (32 bytes each) + positions (8 bytes each)
        uint64_t targets_bytes = static_cast<uint64_t>(numTargets) * 32;
        uint64_t positions_bytes = static_cast<uint64_t>(numTargets) * 8;
        if (targets_bytes > SIZE_MAX || positions_bytes > SIZE_MAX ||
            data.size() < offset + targets_bytes + positions_bytes + 4) {
            return proof;  // Overflow or insufficient data
        }

        // 3. Target hashes (32 bytes each)
        for (uint32_t i = 0; i < numTargets; i++) {
            UtreexoHash target(data.begin() + offset, data.begin() + offset + 32);
            proof.targets.push_back(target);
            offset += 32;
        }

        // 4. Positions (8 bytes each)
        for (uint32_t i = 0; i < numTargets; i++) {
            uint64_t pos = static_cast<uint64_t>(data[offset]) |
                          (static_cast<uint64_t>(data[offset+1]) << 8) |
                          (static_cast<uint64_t>(data[offset+2]) << 16) |
                          (static_cast<uint64_t>(data[offset+3]) << 24) |
                          (static_cast<uint64_t>(data[offset+4]) << 32) |
                          (static_cast<uint64_t>(data[offset+5]) << 40) |
                          (static_cast<uint64_t>(data[offset+6]) << 48) |
                          (static_cast<uint64_t>(data[offset+7]) << 56);
            proof.positions.push_back(pos);
            offset += 8;
        }

        // 5. Number of proof hashes (4 bytes)
        if (data.size() < offset + 4) {
            return proof;
        }
        uint32_t numProofHashes = data[offset] | (data[offset+1] << 8) |
                                 (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // 6. Proof hashes (32 bytes each)
        uint64_t proof_bytes = static_cast<uint64_t>(numProofHashes) * 32;
        if (proof_bytes > SIZE_MAX || data.size() < offset + proof_bytes) {
            return proof;
        }

        for (uint32_t i = 0; i < numProofHashes; i++) {
            UtreexoHash proof_hash(data.begin() + offset, data.begin() + offset + 32);
            proof.proof_hashes.push_back(proof_hash);
            offset += 32;
        }

    } else {
        // ─────────────────────────────────────────────────────────────────────
        // Legacy format (version 0-3): No positions, backward compatibility
        // ─────────────────────────────────────────────────────────────────────
        if (data.size() < 8) {  // Min: 4 (numTargets) + 4 (numProofHashes)
            return proof;
        }

        // 1. Number of targets (first 4 bytes, version byte is reinterpreted)
        uint32_t numTargets = data[offset] | (data[offset+1] << 8) |
                             (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // 2. Target hashes
        uint64_t targets_bytes = static_cast<uint64_t>(numTargets) * 32;
        if (targets_bytes > SIZE_MAX || data.size() < offset + targets_bytes + 4) {
            return proof;
        }

        for (uint32_t i = 0; i < numTargets; i++) {
            UtreexoHash target(data.begin() + offset, data.begin() + offset + 32);
            proof.targets.push_back(target);
            offset += 32;
        }

        // 3. Number of proof hashes
        if (data.size() < offset + 4) {
            return proof;
        }

        uint32_t numProofHashes = data[offset] | (data[offset+1] << 8) |
                                 (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // 4. Proof hashes
        uint64_t proof_bytes = static_cast<uint64_t>(numProofHashes) * 32;
        if (proof_bytes > SIZE_MAX || data.size() < offset + proof_bytes) {
            return proof;
        }

        for (uint32_t i = 0; i < numProofHashes; i++) {
            UtreexoHash proof_hash(data.begin() + offset, data.begin() + offset + 32);
            proof.proof_hashes.push_back(proof_hash);
            offset += 32;
        }

        // Legacy proofs don't have positions - they need stateful verification
        // Leave positions empty, caller must check isValid() before stateless verification
    }

    return proof;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9.1: Compression (Hash Deduplication)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockUtreexoProof::serializeCompressed() const {
    std::vector<uint8_t> data;

    // Phase 9.1: Build hash dictionary (deduplicate)
    std::vector<UtreexoHash> dictionary;
    std::map<UtreexoHash, uint32_t> hash_to_index;

    auto getOrAddIndex = [&](const UtreexoHash& hash) -> uint32_t {
        auto it = hash_to_index.find(hash);
        if (it != hash_to_index.end()) {
            return it->second;
        }
        uint32_t index = dictionary.size();
        dictionary.push_back(hash);
        hash_to_index[hash] = index;
        return index;
    };

    // Build dictionary from targets
    std::vector<uint32_t> target_indices;
    for (const UtreexoHash& target : targets) {
        target_indices.push_back(getOrAddIndex(target));
    }

    // Build dictionary from proof_hashes
    std::vector<uint32_t> proof_indices;
    for (const UtreexoHash& proof_hash : proof_hashes) {
        proof_indices.push_back(getOrAddIndex(proof_hash));
    }

    // Write version (2 = compressed)
    data.push_back(2);

    // Write dictionary size (4 bytes)
    uint32_t dict_size = dictionary.size();
    data.push_back(dict_size & 0xFF);
    data.push_back((dict_size >> 8) & 0xFF);
    data.push_back((dict_size >> 16) & 0xFF);
    data.push_back((dict_size >> 24) & 0xFF);

    // Write dictionary hashes (32 bytes each)
    for (const UtreexoHash& hash : dictionary) {
        data.insert(data.end(), hash.begin(), hash.end());
    }

    // Write target count (4 bytes)
    uint32_t num_targets = target_indices.size();
    data.push_back(num_targets & 0xFF);
    data.push_back((num_targets >> 8) & 0xFF);
    data.push_back((num_targets >> 16) & 0xFF);
    data.push_back((num_targets >> 24) & 0xFF);

    // Write target indices (4 bytes each)
    for (uint32_t idx : target_indices) {
        data.push_back(idx & 0xFF);
        data.push_back((idx >> 8) & 0xFF);
        data.push_back((idx >> 16) & 0xFF);
        data.push_back((idx >> 24) & 0xFF);
    }

    // Write proof count (4 bytes)
    uint32_t num_proofs = proof_indices.size();
    data.push_back(num_proofs & 0xFF);
    data.push_back((num_proofs >> 8) & 0xFF);
    data.push_back((num_proofs >> 16) & 0xFF);
    data.push_back((num_proofs >> 24) & 0xFF);

    // Write proof indices (4 bytes each)
    for (uint32_t idx : proof_indices) {
        data.push_back(idx & 0xFF);
        data.push_back((idx >> 8) & 0xFF);
        data.push_back((idx >> 16) & 0xFF);
        data.push_back((idx >> 24) & 0xFF);
    }

    return data;
}

BlockUtreexoProof BlockUtreexoProof::deserializeCompressed(const std::vector<uint8_t>& data) {
    BlockUtreexoProof proof;

    // ═══════════════════════════════════════════════════════════════════════
    // Non-consensus DoS protection. Reject before allocating any buffers.
    // ═══════════════════════════════════════════════════════════════════════
    if (data.size() > MAX_UTREEXO_PROOF_BYTES) {
        return proof;  // Reject oversized proof
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Medium Priority Fix: Proof compression v2 - index validation hardening
    // ═══════════════════════════════════════════════════════════════════════

    if (data.size() < 1) return proof;

    size_t offset = 0;

    // Read version
    uint8_t version = data[offset++];
    if (version != 2) {
        // Invalid version for compressed format
        return proof;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read dictionary size (4 bytes)
    // ─────────────────────────────────────────────────────────────────────────
    if (data.size() < offset + 4) return proof;
    uint32_t dict_size = data[offset] | (data[offset+1] << 8) |
                        (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // Security: Limit dictionary size (use constant, not magic number)
    if (dict_size > MAX_PROOF_DICTIONARY_SIZE) {
        return proof;  // Dictionary too large - reject
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read dictionary (32 bytes each)
    // ─────────────────────────────────────────────────────────────────────────
    uint64_t dict_bytes = static_cast<uint64_t>(dict_size) * 32;
    if (dict_bytes > SIZE_MAX || data.size() < offset + dict_bytes) {
        return proof;
    }

    std::vector<UtreexoHash> dictionary;
    dictionary.reserve(dict_size);
    for (uint32_t i = 0; i < dict_size; i++) {
        UtreexoHash hash(data.begin() + offset, data.begin() + offset + 32);
        dictionary.push_back(std::move(hash));
        offset += 32;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read target count (4 bytes)
    // ─────────────────────────────────────────────────────────────────────────
    if (data.size() < offset + 4) return proof;
    uint32_t num_targets = data[offset] | (data[offset+1] << 8) |
                          (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // Security: Limit target count
    if (num_targets > MAX_PROOF_TARGETS) {
        return proof;  // Too many targets - reject
    }

    // Security: Empty dictionary with non-empty targets is invalid
    if (dict_size == 0 && num_targets > 0) {
        return proof;  // Cannot have targets without dictionary entries
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read target indices (4 bytes each)
    // ─────────────────────────────────────────────────────────────────────────
    uint64_t target_indices_bytes = static_cast<uint64_t>(num_targets) * 4;
    if (target_indices_bytes > SIZE_MAX || data.size() < offset + target_indices_bytes) {
        return proof;
    }

    proof.targets.reserve(num_targets);
    for (uint32_t i = 0; i < num_targets; i++) {
        uint32_t idx = data[offset] | (data[offset+1] << 8) |
                      (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // Security: Validate index is in bounds
        if (idx >= dict_size) {
            // Clear partial results and fail hard
            proof.targets.clear();
            return proof;
        }

        proof.targets.push_back(dictionary[idx]);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read proof hash count (4 bytes)
    // ─────────────────────────────────────────────────────────────────────────
    if (data.size() < offset + 4) {
        proof.targets.clear();
        return proof;
    }
    uint32_t num_proofs = data[offset] | (data[offset+1] << 8) |
                         (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // Security: Limit proof hash count
    if (num_proofs > MAX_PROOF_HASHES) {
        proof.targets.clear();
        return proof;  // Too many proof hashes - reject
    }

    // Security: Empty dictionary with non-empty proofs is invalid
    if (dict_size == 0 && num_proofs > 0) {
        proof.targets.clear();
        return proof;  // Cannot have proof hashes without dictionary entries
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read proof indices (4 bytes each)
    // ─────────────────────────────────────────────────────────────────────────
    uint64_t proof_indices_bytes = static_cast<uint64_t>(num_proofs) * 4;
    if (proof_indices_bytes > SIZE_MAX || data.size() < offset + proof_indices_bytes) {
        proof.targets.clear();
        return proof;
    }

    proof.proof_hashes.reserve(num_proofs);
    for (uint32_t i = 0; i < num_proofs; i++) {
        uint32_t idx = data[offset] | (data[offset+1] << 8) |
                      (data[offset+2] << 16) | (data[offset+3] << 24);
        offset += 4;

        // Security: Validate index is in bounds
        if (idx >= dict_size) {
            // Clear partial results and fail hard
            proof.targets.clear();
            proof.proof_hashes.clear();
            return proof;
        }

        proof.proof_hashes.push_back(dictionary[idx]);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Security: Check for leftover bytes (no truncated/padded data allowed)
    // ─────────────────────────────────────────────────────────────────────────
    if (offset != data.size()) {
        // Leftover unread bytes indicate malformed input
        proof.targets.clear();
        proof.proof_hashes.clear();
        return proof;
    }

    return proof;
}

size_t BlockUtreexoProof::estimateCompressedSize() const {
    // Count unique hashes
    std::set<UtreexoHash> unique_hashes;
    for (const UtreexoHash& target : targets) {
        unique_hashes.insert(target);
    }
    for (const UtreexoHash& proof_hash : proof_hashes) {
        unique_hashes.insert(proof_hash);
    }

    size_t dict_size = unique_hashes.size();

    // Format: version(1) + dict_size(4) + dictionary(32*D) +
    //         num_targets(4) + target_indices(4*T) +
    //         num_proofs(4) + proof_indices(4*P)
    return 1 + 4 + (dict_size * 32) + 4 + (targets.size() * 4) + 4 + (proof_hashes.size() * 4);
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9.2: zstd Compression Framing
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockUtreexoProof::serializeCompressedWithZstd() const {
    // Phase 9.2: Wrap deduplicated proof in zstd frame

    // Step 1: Get deduplicated (v2) proof
    std::vector<uint8_t> deduped = serializeCompressed();

    // Step 2: Check threshold (only compress if > 256 bytes)
    const size_t COMPRESSION_THRESHOLD = 256;
    if (deduped.size() <= COMPRESSION_THRESHOLD) {
        // Too small to benefit from zstd - return v2 format
        return deduped;
    }

    // Step 3: Compress with zstd (level 3 = fast, good ratio)
    const int ZSTD_LEVEL = 3;
    size_t max_compressed_size = ZSTD_compressBound(deduped.size());
    std::vector<uint8_t> compressed_data(max_compressed_size);

    size_t compressed_size = ZSTD_compress(
        compressed_data.data(),
        compressed_data.size(),
        deduped.data(),
        deduped.size(),
        ZSTD_LEVEL
    );

    // Check for compression errors
    if (ZSTD_isError(compressed_size)) {
        // Compression failed - return v2 format as fallback
        return deduped;
    }

    compressed_data.resize(compressed_size);

    // Step 4: Build v3 wire format
    std::vector<uint8_t> result;

    // Version byte (3 = deduplicated + zstd)
    result.push_back(3);

    // Uncompressed size (4 bytes)
    uint32_t uncompressed = deduped.size();
    result.push_back(uncompressed & 0xFF);
    result.push_back((uncompressed >> 8) & 0xFF);
    result.push_back((uncompressed >> 16) & 0xFF);
    result.push_back((uncompressed >> 24) & 0xFF);

    // Compressed size (4 bytes)
    uint32_t compressed = compressed_size;
    result.push_back(compressed & 0xFF);
    result.push_back((compressed >> 8) & 0xFF);
    result.push_back((compressed >> 16) & 0xFF);
    result.push_back((compressed >> 24) & 0xFF);

    // Compressed data
    result.insert(result.end(), compressed_data.begin(), compressed_data.end());

    return result;
}

BlockUtreexoProof BlockUtreexoProof::deserializeCompressedWithZstd(const std::vector<uint8_t>& data) {
    BlockUtreexoProof proof;

    // ═══════════════════════════════════════════════════════════════════════
    // Non-consensus DoS protection. Reject before allocating any buffers.
    // ═══════════════════════════════════════════════════════════════════════
    if (data.size() > MAX_UTREEXO_PROOF_BYTES) {
        return proof;  // Reject oversized proof
    }

    if (data.size() < 1) return proof;

    size_t offset = 0;

    // Read version
    uint8_t version = data[offset++];

    // Phase 9.2: Handle v3 (zstd compressed) or fallback to v2
    if (version == 2) {
        // v2 format (deduplicated only) - pass through to v2 deserializer
        return deserializeCompressed(data);
    }

    if (version != 3) {
        // Invalid version
        return proof;
    }

    // Read uncompressed size (4 bytes)
    if (data.size() < offset + 4) return proof;
    uint32_t uncompressed_size =
        static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;

    // Read compressed size (4 bytes)
    if (data.size() < offset + 4) return proof;
    uint32_t compressed_size =
        static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;

    // Security: Decompression bomb protection
    const uint32_t MAX_PROOF_SIZE = 100 * 1024;  // 100 KB
    if (uncompressed_size > MAX_PROOF_SIZE) {
        // Reject oversized proofs
        return proof;
    }

    // Security: Reject suspicious compression ratios (> 100:1)
    if (compressed_size > 0 && uncompressed_size / compressed_size > 100) {
        // Suspiciously high compression ratio
        return proof;
    }

    // Validate compressed data size
    if (data.size() < offset + compressed_size) return proof;

    // Decompress with zstd
    std::vector<uint8_t> decompressed(uncompressed_size);
    size_t actual_decompressed_size = ZSTD_decompress(
        decompressed.data(),
        decompressed.size(),
        data.data() + offset,
        compressed_size
    );

    // Check for decompression errors
    if (ZSTD_isError(actual_decompressed_size)) {
        // Decompression failed
        return proof;
    }

    // Verify size matches
    if (actual_decompressed_size != uncompressed_size) {
        // Size mismatch
        return proof;
    }

    // Pass decompressed v2 data to v2 deserializer
    return deserializeCompressed(decompressed);
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockUtreexoData (Phase 1: Block-Level Proof Structures)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockUtreexoData::serialize() const {
    std::vector<uint8_t> data;

    // 1. Accumulator root before (32 bytes)
    // If empty, write 32 zero bytes
    if (accumulator_root_before.size() == 32) {
        data.insert(data.end(), accumulator_root_before.begin(), accumulator_root_before.end());
    } else {
        // Write 32 zero bytes if root is empty
        data.insert(data.end(), 32, 0x00);
    }

    // 2. Spend proof (variable length)
    std::vector<uint8_t> proof_bytes = spend_proof.serialize();
    data.insert(data.end(), proof_bytes.begin(), proof_bytes.end());

    // 3. Spent outputs count (4 bytes)
    uint32_t spent_count = spent_outputs.size();
    data.push_back(spent_count & 0xFF);
    data.push_back((spent_count >> 8) & 0xFF);
    data.push_back((spent_count >> 16) & 0xFF);
    data.push_back((spent_count >> 24) & 0xFF);

    // 4. Spent outputs (variable length each)
    for (const auto& spent : spent_outputs) {
        const uint8_t spent_format_version = proof_bytes.empty() ? 5 : proof_bytes[0];
        std::vector<uint8_t> spent_bytes = spent.serialize(spent_format_version);
        data.insert(data.end(), spent_bytes.begin(), spent_bytes.end());
    }

    return data;
}

BlockUtreexoData BlockUtreexoData::deserialize(const std::vector<uint8_t>& data) {
    BlockUtreexoData utreexo_data;

    if (data.size() < 32 + 4) {  // Min: root + spent_count (with potentially empty proof)
        return utreexo_data;
    }

    size_t offset = 0;

    // 1. Accumulator root before (32 bytes)
    utreexo_data.accumulator_root_before = UtreexoHash(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;

    // 2. Spend proof (variable-length, versioned format)
    size_t proof_end = 0;
    if (!parse_block_proof_end(data, offset, proof_end)) {
        return BlockUtreexoData();  // Malformed proof payload
    }
    std::vector<uint8_t> proof_bytes(data.begin() + offset, data.begin() + proof_end);
    const uint8_t spent_format_version = proof_bytes.empty() ? 5 : proof_bytes[0];
    utreexo_data.spend_proof = BlockUtreexoProof::deserialize(proof_bytes);
    offset = proof_end;

    // 3. Spent outputs count (4 bytes)
    uint32_t spent_count = 0;
    if (!read_u32_le_at(data, offset, spent_count)) {
        return BlockUtreexoData();  // Malformed spent output count
    }
    offset += 4;

    // Policy limit to avoid pathological allocations from malformed payloads.
    if (spent_count > MAX_PROOF_TARGETS) {
        return BlockUtreexoData();
    }

    // 4. Spent outputs (variable length each)
    for (uint32_t i = 0; i < spent_count; i++) {
        const size_t before = offset;
        SpentOutputData spent = SpentOutputData::deserialize(data, offset, spent_format_version);
        if (offset <= before) {
            return BlockUtreexoData();  // Parse made no progress → malformed payload
        }
        utreexo_data.spent_outputs.push_back(spent);
    }

    return utreexo_data;
}

// ═══════════════════════════════════════════════════════════════════════════
// UtreexoForest
// ═══════════════════════════════════════════════════════════════════════════

namespace {
// Invariant: roots_[h].has_value() must match ((numLeaves_ >> h) & 1).
//
// ════════════════════════════════════════════════════════════════════════════
// Apr 13 2026 investigation (Option A of the Bug #4 follow-up):
// ════════════════════════════════════════════════════════════════════════════
// This was the diagnostic that pinpointed the root cause of the cached-roots
// staleness that made `proof.verify` fail on every covenant spend.
//
// Instrumentation showed: `recomputePath()` (called by remove / removeAtKnownPosition)
// sets `roots_[h] = nullopt` when the sole leaf in an h=0 tree is deleted,
// even though `numLeaves_` still has bit h set. The next `add()` sees the
// empty slot and takes the "place" branch instead of "merge", which then
// cascades into extra ghost roots at h=0..h-1 on subsequent adds. That is
// exactly the state the Bug #4 session captured:
//
//     roots_[h=0,1,2,3,8,10,13] populated  with numLeaves=9488
//     (expected:  roots_[h=4,8,10,13])
//
// FIX IS A CONSENSUS CHANGE: `recomputePath()` would need to preserve the
// invariant, e.g. by using a canonical "zero-sentinel" hash for all-deleted
// subtrees instead of nullopt. That changes the `utreexo_root` hash computed
// by `getCommitment()` and therefore cannot be deployed without coordinated
// fleet-wide upgrade + activation height. The pragmatic workaround for the
// user flow is already in place: `removeAtKnownPosition()` sidesteps the
// broken `proof.verify` path for trusted internal callers (ComputeUtreexoRootPure
// and ConnectBlockInternal).
//
// Helper retained (but not currently called from any hot path) so it is
// trivially re-enabled when the consensus-compatible fix is written.
// ════════════════════════════════════════════════════════════════════════════
[[maybe_unused]] void CheckRootsInvariant(
    const char* caller,
    const std::vector<std::optional<UtreexoHash>>& roots,
    uint64_t numLeaves) {
    bool ok = true;
    std::ostringstream bad;
    for (size_t h = 0; h < roots.size(); ++h) {
        const bool expected = ((numLeaves >> h) & 1ULL) != 0ULL;
        const bool actual = roots[h].has_value();
        if (expected != actual) {
            ok = false;
            bad << " h=" << h << ":exp=" << expected << ":act=" << actual;
        }
    }
    for (size_t h = roots.size(); h < 64; ++h) {
        const bool expected = ((numLeaves >> h) & 1ULL) != 0ULL;
        if (expected) {
            ok = false;
            bad << " h=" << h << ":exp=1:act=OOR";
        }
    }
    if (!ok) {
        std::cerr << "❌ [Utreexo Invariant] " << caller
                  << " numLeaves=" << numLeaves
                  << " roots_.size=" << roots.size()
                  << " diverged:" << bad.str() << std::endl;
    }
}
} // anonymous namespace

UtreexoForest::UtreexoForest() : numLeaves_(0) {}

// ═══════════════════════════════════════════════════════════════════════════
// Single-source-of-truth fork-aware clone factory (Apr 13 2026 Stage 3).
// ═══════════════════════════════════════════════════════════════════════════
// Every consensus call site that needs a forest to compute a commitment or
// proof for a SPECIFIC block height MUST use this factory. It consolidates
// what used to be scattered `IsUtreexoCanonicalRootsActive(height)` +
// `setCanonicalEmptyRoots(true)` + `rebuildRoots()` triples into one call.
// See docs in the header declaration.
UtreexoForest UtreexoForest::cloneForHeight(uint32_t height) const {
    UtreexoForest copy = clone();
    if (IsUtreexoCanonicalRootsActive(height) && !copy.canonical_empty_roots_) {
        copy.canonical_empty_roots_ = true;
        copy.rebuildRoots();
    }
    return copy;
}

std::string UtreexoForest::dumpInternalState() const {
    // Stable text format. Sorted where iteration order would otherwise
    // be hash-map-dependent. Goal: byte-identical output for byte-
    // identical forest internal state, regardless of how the forest
    // got there.
    std::ostringstream out;
    auto hex32 = [](const UtreexoHash& h) {
        std::ostringstream s;
        for (size_t i = 0; i < std::min(h.size(), size_t(32)); ++i) {
            s << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<int>(h[i]);
        }
        return s.str();
    };

    out << "canonical_empty_roots=" << (canonical_empty_roots_ ? 1 : 0) << "\n";
    out << "numLeaves=" << numLeaves_ << "\n";
    out << "active_leaves=" << (numLeaves_ - deleted_positions_.size()) << "\n";
    out << "deleted_count=" << deleted_positions_.size() << "\n";
    out << "leaf_positions_count=" << leaf_positions_.size() << "\n";
    out << "nodes_size=" << nodes_.size() << "\n";
    out << "roots_size=" << roots_.size() << "\n";
    out << "commitment=" << hex32(getCommitment()) << "\n";

    // roots_[h]: distinguish nullopt from optional(value) explicitly
    for (size_t h = 0; h < roots_.size(); ++h) {
        out << "roots[" << h << "]=";
        if (roots_[h].has_value()) {
            out << "value:" << hex32(roots_[h].value());
        } else {
            out << "nullopt";
        }
        out << "\n";
    }

    // deleted_positions_ (sorted)
    {
        std::vector<uint64_t> sorted_deleted(deleted_positions_.begin(),
                                              deleted_positions_.end());
        std::sort(sorted_deleted.begin(), sorted_deleted.end());
        for (uint64_t pos : sorted_deleted) {
            out << "deleted_position=" << pos << "\n";
        }
    }

    // nodes_ — only emit positions that have a value, sorted
    for (size_t pos = 0; pos < nodes_.size(); ++pos) {
        if (nodes_[pos].has_value()) {
            out << "node[" << pos << "]=" << hex32(nodes_[pos].value()) << "\n";
        }
    }

    // leaf_positions_ — sorted by hash for determinism
    {
        std::vector<std::pair<UtreexoHash, uint64_t>> sorted_lp(
            leaf_positions_.begin(), leaf_positions_.end());
        std::sort(sorted_lp.begin(), sorted_lp.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [hash, pos] : sorted_lp) {
            out << "leaf[" << hex32(hash) << "]=" << pos << "\n";
        }
    }

    return out.str();
}

std::string UtreexoForest::describeAddFailure(const UtreexoHash& leafHash) const {
    // One-line diagnostic for add()-failure sites. Mirrors the checks inside
    // add() so the caller can tell at a glance whether the rejection was
    // capacity, a live-duplicate, or a stale map entry that add() would
    // self-heal on retry.
    auto hex32 = [](const UtreexoHash& h) {
        std::ostringstream s;
        for (size_t i = 0; i < std::min(h.size(), size_t(32)); ++i) {
            s << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<int>(h[i]);
        }
        return s.str();
    };

    std::ostringstream out;
    const uint64_t capacity_room =
        (numLeaves_ < MAX_UTREEXO_LEAVES) ? (MAX_UTREEXO_LEAVES - numLeaves_) : 0;

    out << "leaf=" << hex32(leafHash)
        << " numLeaves=" << numLeaves_
        << " capacity_room=" << capacity_room;

    if (capacity_room == 0) {
        out << " reason=at_capacity";
        return out.str();
    }

    auto it = leaf_positions_.find(leafHash);
    if (it == leaf_positions_.end()) {
        // No map entry — add() should not have failed for duplicate reasons.
        // Capacity is fine (checked above). Most likely overflow in
        // checked_add(numLeaves_, 1) — extremely unlikely below 2^40 leaves.
        out << " reason=no_map_entry_unexpected";
        return out.str();
    }

    const uint64_t existing_pos = it->second;
    const bool in_nodes_range = existing_pos < nodes_.size();
    const bool node_has_value = in_nodes_range && nodes_[existing_pos].has_value();
    const bool node_matches =
        node_has_value && nodes_[existing_pos].value() == leafHash;
    const bool deleted = deleted_positions_.count(existing_pos) != 0;

    out << " existing_pos=" << existing_pos
        << " in_nodes_range=" << (in_nodes_range ? 1 : 0)
        << " node_has_value=" << (node_has_value ? 1 : 0)
        << " node_matches=" << (node_matches ? 1 : 0)
        << " deleted=" << (deleted ? 1 : 0);

    if (node_has_value) {
        out << " existing_node=" << hex32(nodes_[existing_pos].value());
    }

    if (in_nodes_range && node_matches && !deleted) {
        out << " reason=duplicate_live_leaf";
    } else {
        // add() would have self-healed and continued — if we got here, the
        // failure is from somewhere else (caller saw an earlier failure mode
        // or the snapshot diverged from the forest add() saw).
        out << " reason=stale_map_entry_only";
    }
    return out.str();
}

UtreexoForest::~UtreexoForest() {}

uint64_t UtreexoForest::add(const UtreexoHash& leafHash) {
    // ═══════════════════════════════════════════════════════════════════════
    // Medium Priority Fix: Integer overflow checks in position arithmetic
    // ═══════════════════════════════════════════════════════════════════════

    // Check bounds BEFORE incrementing
    if (numLeaves_ >= MAX_UTREEXO_LEAVES) {
        // Forest is at maximum capacity - cannot add more leaves
        // Return UINT64_MAX as error indicator (callers should check)
        return UINT64_MAX;
    }

    uint64_t position = numLeaves_;

    // Checked resize - verify position + 1 doesn't overflow
    uint64_t newSize;
    if (!checked_add(position, 1, newSize)) {
        return UINT64_MAX;  // Overflow in resize calculation
    }

    // Consensus hardening: live leaf hashes must be unique in the forest.
    // A duplicate hash would make leaf_positions_ ambiguous (hash -> multiple positions),
    // which can break deterministic spend removal.
    auto existing_it = leaf_positions_.find(leafHash);
    if (existing_it != leaf_positions_.end()) {
        const uint64_t existing_pos = existing_it->second;
        const bool existing_live =
            existing_pos < nodes_.size() &&
            nodes_[existing_pos].has_value() &&
            nodes_[existing_pos].value() == leafHash &&
            deleted_positions_.count(existing_pos) == 0;

        if (existing_live) {
            std::cerr << "❌ [Utreexo Add] Duplicate live leaf hash insertion rejected"
                      << " (existing_pos=" << existing_pos
                      << ", new_pos=" << position << ")" << std::endl;
            // Diagnostic: dump both leaf hashes so we can verify they truly match
            std::cerr << "   [DIAG] new_leaf_hash:      ";
            for (size_t i = 0; i < leafHash.size(); ++i)
                std::cerr << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(leafHash[i]);
            std::cerr << std::dec << std::endl;
            std::cerr << "   [DIAG] existing_leaf_hash:  ";
            const auto& ex = nodes_[existing_pos].value();
            for (size_t i = 0; i < ex.size(); ++i)
                std::cerr << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(ex[i]);
            std::cerr << std::dec << std::endl;
            return UINT64_MAX;
        }

        // Defensive self-heal: stale map entry (deleted/cleared node) should not block add.
        leaf_positions_.erase(existing_it);
    }

    // Store the leaf
    if (position >= nodes_.size()) {
        nodes_.resize(newSize);
    }
    nodes_[position] = leafHash;  // std::optional assignment

    // Store leaf position mapping (for proof generation)
    leaf_positions_[leafHash] = position;

    // Add leaf using binary carry logic (like adding 1 in binary)
    // Loop is bounded by MAX_TREE_HEIGHT (40), not 64
    std::optional<UtreexoHash> carry = leafHash;
    std::vector<std::optional<UtreexoHash>> newRoots;

    for (size_t h = 0; h < MAX_TREE_HEIGHT && carry.has_value(); h++) {
        // Ensure newRoots is large enough
        while (newRoots.size() <= h) {
            newRoots.push_back(std::nullopt);
        }

        // Check if there was a root at this height
        if (h < roots_.size() && roots_[h].has_value()) {
            // Merge with existing root and carry to next height
            carry = parentHash(roots_[h].value(), carry.value());
        } else {
            // No existing root, place carry here
            newRoots[h] = carry;
            carry = std::nullopt;
        }
    }

    // Copy over any remaining roots from higher heights
    for (size_t h = newRoots.size(); h < roots_.size(); h++) {
        if (roots_[h].has_value()) {
            while (newRoots.size() <= h) {
                newRoots.push_back(std::nullopt);
            }
            newRoots[h] = roots_[h];
        }
    }

    roots_ = newRoots;

    // Checked increment - guaranteed safe due to MAX_UTREEXO_LEAVES check above
    numLeaves_++;

    return position;
}

void UtreexoForest::mergeRoots() {
    // Merge adjacent roots of same height using binary carry logic
    // roots_ are ordered from smallest (height 0) to largest

    // Keep merging the two smallest roots while they have the same height
    while (roots_.size() >= 2) {
        // Check if the two smallest roots are the same height
        // Two trees have same height if they represent consecutive positions in binary

        // Get the two smallest roots (must have values)
        if (!roots_[0].has_value() || !roots_[1].has_value()) {
            break;  // Cannot merge with empty roots
        }
        UtreexoHash left = roots_[0].value();
        UtreexoHash right = roots_[1].value();

        // Determine if they should be merged
        // This happens when we have 2 trees at the same height
        // We can check this by looking at the binary representation of numLeaves_

        // Calculate what height the first root should be
        // The first root (smallest) corresponds to the least significant 1-bit in numLeaves_
        uint64_t n = numLeaves_;
        int firstSetBit = 0;
        while ((n & (1ULL << firstSetBit)) == 0 && firstSetBit < 64) {
            firstSetBit++;
        }

        // If numLeaves_ has form ...110 (two consecutive 1s), merge
        // This means bit positions firstSetBit and firstSetBit+1 are both set
        if ((n & (1ULL << firstSetBit)) && (n & (1ULL << (firstSetBit + 1)))) {
            // Merge left and right into parent
            UtreexoHash parent = parentHash(left, right);

            // Remove the two smallest roots and add the parent
            roots_.erase(roots_.begin());
            roots_.erase(roots_.begin());

            // Insert parent at the correct position (maintaining sorted order)
            // Since we merged two height-h trees into height-(h+1),
            // the new root should go after other height-(h+1) roots
            roots_.insert(roots_.begin(), parent);
        } else {
            // No more merges needed
            break;
        }
    }
}

bool UtreexoForest::remove(const UtreexoHash& leafHash, const UtreexoProof& proof) {
    auto dumpHash = [](const UtreexoHash& h) {
        std::ostringstream oss;
        for (size_t b = 0; b < std::min(h.size(), size_t(8)); b++)
            oss << std::hex << std::setfill('0') << std::setw(2) << (int)h[b];
        return oss.str();
    };

    // 1. Verify the proof is valid
    if (!proof.verify(leafHash, getRoots())) {
        std::cerr << "⚠️  [Utreexo Remove] STEP1 proof.verify FAILED  leaf="
                  << dumpHash(leafHash) << "  pos=" << proof.position
                  << "  siblings=" << proof.siblings.size()
                  << "  numLeaves_=" << numLeaves_ << std::endl;
        // Walk the proof manually and print the computed root + all forest roots.
        UtreexoHash cur = leafHash;
        uint64_t curPos = proof.position;
        for (size_t si = 0; si < proof.siblings.size(); ++si) {
            const UtreexoHash& sib = proof.siblings[si];
            std::cerr << "    sib[" << si << "]=" << dumpHash(sib)
                      << " curPos=" << curPos
                      << " dir=" << (curPos % 2 == 0 ? "L" : "R") << std::endl;
            cur = (curPos % 2 == 0) ? HashNode(cur, sib) : HashNode(sib, cur);
            curPos /= 2;
        }
        std::cerr << "    computed=" << dumpHash(cur) << std::endl;
        auto live_roots = getRoots();
        for (size_t ri = 0; ri < live_roots.size(); ++ri) {
            std::cerr << "    root[" << ri << "]=" << dumpHash(live_roots[ri]) << std::endl;
        }
        // Also dump the indexed roots so we know which heights are populated.
        for (size_t ri = 0; ri < roots_.size(); ++ri) {
            if (roots_[ri].has_value()) {
                std::cerr << "    roots_[h=" << ri << "]=" << dumpHash(roots_[ri].value()) << std::endl;
            }
        }
        return false;
    }

    // 2. Check position validity
    if (proof.position >= numLeaves_) {
        std::cerr << "⚠️  [Utreexo Remove] STEP2 position>=numLeaves  pos="
                  << proof.position << "  numLeaves_=" << numLeaves_ << std::endl;
        return false;
    }

    // 3. Check not already deleted
    if (isDeleted(proof.position)) {
        std::cerr << "⚠️  [Utreexo Remove] STEP3 already deleted  pos="
                  << proof.position << std::endl;
        return false;  // Already removed
    }

    // 4. Check that the leaf at this position matches
    if (proof.position < nodes_.size()) {
        if (!nodes_[proof.position].has_value() || nodes_[proof.position].value() != leafHash) {
            std::cerr << "⚠️  [Utreexo Remove] STEP4 leaf mismatch  pos="
                      << proof.position
                      << "  expected=" << dumpHash(leafHash)
                      << "  stored="
                      << (nodes_[proof.position].has_value() ? dumpHash(nodes_[proof.position].value()) : "<empty>")
                      << std::endl;
            return false;  // Leaf doesn't match or is empty
        }
    }

    // 5. Mark position as deleted
    deleted_positions_.insert(proof.position);

    // 6. Clear the leaf hash from nodes_ (set to std::nullopt)
    if (proof.position < nodes_.size()) {
        nodes_[proof.position] = std::nullopt;  // Explicitly empty
    }

    // 7. Remove from leaf_positions_ map
    leaf_positions_.erase(leafHash);

    // 8. Recompute parent hashes along the path to root
    recomputePath(proof.position);

#ifdef ENABLE_UTREEXO_INVARIANT_CHECKS
    if (!validateLeafIndexConsistency()) {
        std::cerr << "❌ [Utreexo Remove] leaf_positions_ invariant failed after remove()" << std::endl;
        return false;
    }
#endif

    return true;
}

bool UtreexoForest::removeAtKnownPosition(uint64_t position, const UtreexoHash& leafHash) {
    // Trusted internal variant of remove() that skips the proof.verify step.
    // Still enforces the structural invariants — bounds, not-deleted, and
    // leaf-hash match. See the header comment for rationale.

    if (position >= numLeaves_) {
        return false;
    }
    if (isDeleted(position)) {
        return false;
    }
    if (position < nodes_.size()) {
        if (!nodes_[position].has_value() || nodes_[position].value() != leafHash) {
            return false;
        }
    } else {
        // Beyond the materialized nodes_ range — structurally impossible for
        // a live leaf, treat as failure.
        return false;
    }

    deleted_positions_.insert(position);

    if (position < nodes_.size()) {
        nodes_[position] = std::nullopt;
    }

    leaf_positions_.erase(leafHash);

    recomputePath(position);

#ifdef ENABLE_UTREEXO_INVARIANT_CHECKS
    if (!validateLeafIndexConsistency()) {
        std::cerr << "❌ [Utreexo removeAtKnownPosition] leaf_positions_ invariant failed" << std::endl;
        return false;
    }
#endif

    return true;
}

bool UtreexoForest::removeAtKnownPositions(
    const std::vector<std::pair<uint64_t, UtreexoHash>>& removals) {
    std::unordered_set<uint64_t> unique_positions;
    unique_positions.reserve(removals.size());

    // Validate the complete request first so callers never observe a partial
    // block transition when one input is stale or malformed.
    for (const auto& [position, leaf_hash] : removals) {
        if (position >= numLeaves_ || position >= nodes_.size() ||
            isDeleted(position) || !nodes_[position].has_value() ||
            nodes_[position].value() != leaf_hash ||
            !unique_positions.insert(position).second) {
            return false;
        }
    }

    for (const auto& [position, leaf_hash] : removals) {
        deleted_positions_.insert(position);
        nodes_[position] = std::nullopt;
        leaf_positions_.erase(leaf_hash);
    }

    if (!removals.empty()) {
        rebuildRoots();
    }

#ifdef ENABLE_UTREEXO_INVARIANT_CHECKS
    if (!validateLeafIndexConsistency()) {
        std::cerr << "❌ [Utreexo removeAtKnownPositions] leaf_positions_ invariant failed"
                  << std::endl;
        return false;
    }
#endif

    return true;
}

// Recompute parent hashes along path from position to root after a removal
void UtreexoForest::recomputePath(uint64_t position) {
    // ═══════════════════════════════════════════════════════════════════════
    // Medium Priority Fix: Bounded position arithmetic
    // ═══════════════════════════════════════════════════════════════════════

    // Early bounds check
    if (position >= numLeaves_ || numLeaves_ > MAX_UTREEXO_LEAVES) {
        return;  // Invalid position or corrupted state
    }

    // Find which tree contains this position by scanning from MSB to LSB
    std::vector<std::pair<uint8_t, uint64_t>> trees; // (height, start_pos)
    uint64_t n = numLeaves_;

    // Find highest set bit (bounded by MAX_TREE_HEIGHT)
    int maxBit = MAX_TREE_HEIGHT - 1;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) {
        maxBit--;
    }

    // Scan from MSB to LSB to build tree position map
    // Use checked arithmetic to prevent overflow in currentPos accumulation
    uint64_t currentPos = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            trees.push_back({(uint8_t)h, currentPos});
            // Checked addition for position accumulation
            uint64_t treeSize;
            if (!checked_shift_left(1ULL, static_cast<uint8_t>(h), treeSize)) {
                return;  // Height too large
            }
            uint64_t newPos;
            if (!checked_add(currentPos, treeSize, newPos)) {
                return;  // Position overflow
            }
            currentPos = newPos;
        }
    }

    // Find which tree contains our position and recompute its root
    for (const auto& [height, treeStart] : trees) {
        // Checked tree size calculation
        uint64_t treeSize;
        if (!checked_shift_left(1ULL, height, treeSize)) {
            continue;  // Skip invalid tree
        }

        // Checked boundary calculation
        uint64_t treeEnd;
        if (!checked_add(treeStart, treeSize, treeEnd)) {
            continue;  // Skip tree with overflowing boundary
        }

        if (position >= treeStart && position < treeEnd) {
            // Recompute root hash for this tree
            // computeSubtreeHash returns std::optional<UtreexoHash>
            auto newRoot = computeSubtreeHash(treeStart, treeSize);

            // Update the root in roots_ array
            if (height < roots_.size()) {
                roots_[height] = newRoot;  // Assign optional directly
            }
            break;
        }
    }
}

// Helper: Compute hash of a subtree rooted at 'start' with 'size' leaves
//
// PRE-STAGE3 BEHAVIOR
// -------------------
// Returns std::nullopt for fully-deleted subtrees. `recomputePath()` then
// stores nullopt in `roots_[h]`, which desyncs `roots_` from `numLeaves_`'s
// bit pattern and causes the `add()` → "place-instead-of-merge" cascade
// that broke covenant spends. This is the Apr 13 2026 Bug #4 root cause.
//
// STAGE 3 BEHAVIOR (canonical_empty_roots_ == true, activated at
// UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET = 2870)
// --------------------------------------------------------------
// Returns a deterministic "zero-filled subtree" hash instead of nullopt for
// fully-deleted subtrees at size >= 1. This preserves the invariant
// `roots_[h].has_value() ⟺ bit h of numLeaves_` and makes both
// proof.verify() and add()'s binary-carry logic consistent with the live
// nodes_ state. CONSENSUS CHANGE — must flip fleet-wide at the same height.
std::optional<UtreexoHash> UtreexoForest::computeSubtreeHash(uint64_t start, uint64_t size) const {
    static const UtreexoHash ZERO_HASH(32, 0);

    if (size == 0) {
        return std::nullopt;
    }

    // Bounds check - prevent overflow in later calculations
    if (start >= MAX_UTREEXO_LEAVES || size > MAX_UTREEXO_LEAVES) {
        return std::nullopt;  // Invalid parameters
    }

    if (size == 1) {
        // Base case: single leaf
        // Check if this position is deleted
        if (isDeleted(start)) {
            // Stage 3: canonical zero-sentinel instead of nullopt.
            return canonical_empty_roots_ ? std::optional<UtreexoHash>(ZERO_HASH)
                                          : std::nullopt;
        }
        if (start < nodes_.size() && nodes_[start].has_value()) {
            return nodes_[start];  // Return the optional (with value)
        }
        // Live position with no hash set — treat identically to deleted.
        return canonical_empty_roots_ ? std::optional<UtreexoHash>(ZERO_HASH)
                                      : std::nullopt;
    }

    // Recursive case: hash left and right subtrees
    uint64_t halfSize = size / 2;

    // Checked addition for right subtree start position
    uint64_t rightStart;
    if (!checked_add(start, halfSize, rightStart)) {
        return std::nullopt;  // Overflow
    }

    auto leftHash = computeSubtreeHash(start, halfSize);
    auto rightHash = computeSubtreeHash(rightStart, halfSize);

    // Handle partial deletions - hash with zero for empty siblings
    // This maintains proof compatibility
    if (leftHash.has_value() && rightHash.has_value()) {
        return parentHash(leftHash.value(), rightHash.value());
    } else if (leftHash.has_value()) {
        // Right is empty - hash with zero to maintain proof compatibility
        return parentHash(leftHash.value(), ZERO_HASH);
    } else if (rightHash.has_value()) {
        // Left is empty - hash with zero to maintain proof compatibility
        return parentHash(ZERO_HASH, rightHash.value());
    } else {
        // Both empty.
        //
        // Pre-Stage3: return nullopt (cascades up so recomputePath stores
        // nullopt in roots_[h], which triggered the bug).
        //
        // Stage 3: UNREACHABLE. In canonical mode the size=1 base case
        // always returns ZERO_HASH (never nullopt), so neither recursive
        // call can yield nullopt, so this branch never fires. We still
        // guard it for defense-in-depth and return nullopt so any
        // accidental path is loud (rather than silently producing the
        // wrong canonical hash for size > 2).
        return std::nullopt;
    }
}

std::optional<UtreexoHash> UtreexoForest::computeSubtreeHashCached(
    uint64_t start, uint64_t size, SubtreeHashCache& cache) const {
    if (size == 0 || (size & (size - 1)) != 0) {
        return std::nullopt;
    }

    uint8_t level = 0;
    for (uint64_t remaining = size; remaining > 1; remaining >>= 1) {
        ++level;
    }
    if (level >= cache.size()) {
        return std::nullopt;
    }

    auto& level_cache = cache[level];
    auto cached = level_cache.find(start);
    if (cached != level_cache.end()) {
        return cached->second;
    }

    std::optional<UtreexoHash> result;
    if (size == 1) {
        result = computeSubtreeHash(start, size);
    } else {
        static const UtreexoHash ZERO_HASH(32, 0);
        const uint64_t half_size = size / 2;
        uint64_t right_start = 0;
        if (!checked_add(start, half_size, right_start)) {
            return std::nullopt;
        }
        auto left = computeSubtreeHashCached(start, half_size, cache);
        auto right = computeSubtreeHashCached(right_start, half_size, cache);
        if (left.has_value() && right.has_value()) {
            result = parentHash(left.value(), right.value());
        } else if (left.has_value()) {
            result = parentHash(left.value(), ZERO_HASH);
        } else if (right.has_value()) {
            result = parentHash(ZERO_HASH, right.value());
        }
    }

    level_cache.emplace(start, result);
    return result;
}

std::optional<UtreexoProof> UtreexoForest::proveWithCache(
    uint64_t position, SubtreeHashCache* cache) const {
    // ═══════════════════════════════════════════════════════════════════════
    // Medium Priority Fix: Bounded position arithmetic
    // ═══════════════════════════════════════════════════════════════════════

    // Early bounds check
    if (position >= numLeaves_ || numLeaves_ > MAX_UTREEXO_LEAVES) {
        return std::nullopt;
    }

    // Cannot prove deleted positions
    if (isDeleted(position)) {
        return std::nullopt;
    }

    UtreexoProof proof;
    proof.position = position;
    proof.numLeaves = numLeaves_;

    // Find which tree contains this position
    // Trees are ordered by position: largest tree first (MSB to LSB)
    // Build tree position map by scanning from MSB to LSB
    std::vector<std::pair<uint8_t, uint64_t>> trees; // (height, start_pos)
    uint64_t n = numLeaves_;

    // Find highest set bit (bounded by MAX_TREE_HEIGHT)
    int maxBit = MAX_TREE_HEIGHT - 1;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) {
        maxBit--;
    }

    // Scan from MSB to LSB with checked arithmetic
    uint64_t currentPos = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            trees.push_back({(uint8_t)h, currentPos});
            // Checked addition for position accumulation
            uint64_t treeSize;
            if (!checked_shift_left(1ULL, static_cast<uint8_t>(h), treeSize)) {
                return std::nullopt;  // Height too large
            }
            uint64_t newPos;
            if (!checked_add(currentPos, treeSize, newPos)) {
                return std::nullopt;  // Position overflow
            }
            currentPos = newPos;
        }
    }

    // Find which tree contains our position
    uint64_t treeStart = 0;
    uint8_t treeHeight = 0;
    bool found = false;
    for (const auto& tree : trees) {
        // Checked tree size calculation
        uint64_t treeSize;
        if (!checked_shift_left(1ULL, tree.first, treeSize)) {
            continue;  // Skip invalid tree
        }
        // Checked boundary calculation
        uint64_t treeEnd;
        if (!checked_add(tree.second, treeSize, treeEnd)) {
            continue;  // Skip tree with overflowing boundary
        }
        if (position >= tree.second && position < treeEnd) {
            treeHeight = tree.first;
            treeStart = tree.second;
            found = true;
            break;
        }
    }

    if (!found) {
        return std::nullopt;  // Position not in any tree
    }

    // Walk up the tree, collecting sibling hashes
    // posInTree is safe: position >= treeStart is verified above
    uint64_t posInTree = position - treeStart;
    uint64_t subtreeSize = 1;

    for (uint8_t level = 0; level < treeHeight && level < MAX_TREE_HEIGHT; level++) {
        // Determine sibling's absolute position
        uint64_t siblingStart;
        uint64_t siblingOffset;

        if ((posInTree / subtreeSize) & 1) {
            // We're the right child, sibling is on the left
            // Safe subtraction: posInTree >= subtreeSize when we're right child
            if (posInTree < subtreeSize) {
                return std::nullopt;  // Shouldn't happen, but safety check
            }
            siblingOffset = posInTree - subtreeSize;
        } else {
            // We're the left child, sibling is on the right
            // Checked addition
            if (!checked_add(posInTree, subtreeSize, siblingOffset)) {
                return std::nullopt;  // Overflow
            }
        }

        // Checked addition for absolute position
        if (!checked_add(treeStart, siblingOffset, siblingStart)) {
            return std::nullopt;  // Overflow
        }

        // Compute sibling hash
        auto siblingHashOpt = cache
            ? computeSubtreeHashCached(siblingStart, subtreeSize, *cache)
            : computeSubtreeHash(siblingStart, subtreeSize);
        // If sibling is empty/deleted, use zero hash as placeholder
        // This maintains proof compatibility
        static const UtreexoHash ZERO_HASH(32, 0);
        proof.siblings.push_back(siblingHashOpt.value_or(ZERO_HASH));

        // Move up: position in tree becomes parent position
        // This arithmetic is safe: dividing makes values smaller, then multiplying
        // by at most 2 * subtreeSize which we check below
        posInTree = (posInTree / subtreeSize / 2) * subtreeSize * 2;

        // Checked doubling of subtreeSize
        uint64_t newSubtreeSize;
        if (!checked_mul(subtreeSize, 2, newSubtreeSize)) {
            return std::nullopt;  // Subtree size overflow
        }
        subtreeSize = newSubtreeSize;
    }

    return proof;
}

std::optional<UtreexoProof> UtreexoForest::prove(uint64_t position) const {
    return proveWithCache(position, nullptr);
}

std::vector<std::optional<UtreexoProof>> UtreexoForest::proveMany(
    const std::vector<uint64_t>& positions) const {
    // One shared subtree-hash cache across the whole batch: sibling subtrees
    // overlap heavily between proofs, so memoization collapses the cost from
    // O(positions × forest) to O(forest) hashing (block-92742 incident).
    SubtreeHashCache subtree_cache(MAX_TREE_HEIGHT + 1);
    std::vector<std::optional<UtreexoProof>> proofs;
    proofs.reserve(positions.size());
    for (uint64_t position : positions) {
        proofs.push_back(proveWithCache(position, &subtree_cache));
    }
    return proofs;
}

UtreexoHash UtreexoForest::getCommitment() const {
    // Canonical commitment v2: SHA256(numLeaves_LE64 || slot[0] || slot[1] || ... || slot[63])
    // Each slot is 32 bytes: the root hash if present, or 32 zero bytes if absent.
    // numLeaves is committed so the forest shape is unambiguous.
    // Total preimage: 8 + 64*32 = 2056 bytes.
    static constexpr size_t NUM_SLOTS = 64;
    static const UtreexoHash ZERO_ROOT(32, 0);

    std::vector<uint8_t> preimage;
    preimage.reserve(8 + NUM_SLOTS * 32);

    // 1. numLeaves as 8 bytes little-endian
    uint64_t n = numLeaves_;
    for (int i = 0; i < 8; ++i) {
        preimage.push_back(static_cast<uint8_t>(n & 0xFF));
        n >>= 8;
    }

    // 2. 64 fixed root slots (32 bytes each)
    for (size_t h = 0; h < NUM_SLOTS; ++h) {
        if (h < roots_.size() && roots_[h].has_value()) {
            const auto& root = roots_[h].value();
            preimage.insert(preimage.end(), root.begin(), root.end());
        } else {
            preimage.insert(preimage.end(), ZERO_ROOT.begin(), ZERO_ROOT.end());
        }
    }

    return SHA256_Hash(preimage);
}

uint8_t UtreexoForest::getTreeHeight(uint64_t position) const {
    if (position >= numLeaves_) {
        return 0;
    }

    // Scan from MSB to LSB to find which tree contains this position
    uint64_t n = numLeaves_;

    // Find highest set bit
    int maxBit = 63;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) {
        maxBit--;
    }

    // Scan from MSB to LSB
    uint64_t currentPos = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            uint64_t treeSize = (1ULL << h);
            if (position >= currentPos && position < currentPos + treeSize) {
                return (uint8_t)h;
            }
            currentPos += treeSize;
        }
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Batched Proof Generation (Miner-Side)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<UtreexoHash> UtreexoForest::generateBatchProof(const std::vector<UtreexoHash>& targets) const {
    // ────────────────────────────────────────────────────────────────────────
    // MINER-SIDE: Generate batched Merkle proof for multiple leaves
    // ────────────────────────────────────────────────────────────────────────
    // This function generates a minimal set of proof_hashes that proves
    // all target leaves exist in the accumulator.
    //
    // Algorithm:
    // 1. Find position of each target leaf (using leaf_positions_ map)
    // 2. Generate individual proof for each target
    // 3. Combine and deduplicate proof hashes (batching efficiency)
    // 4. Return minimal set of proof_hashes
    // ────────────────────────────────────────────────────────────────────────

    std::vector<UtreexoHash> proof_hashes;

    // Handle empty targets
    if (targets.empty()) {
        std::cout << "ℹ️  [Utreexo Proof Gen] No targets - empty proof" << std::endl;
        return proof_hashes;
    }

    std::cout << "🔍 [Utreexo Proof Gen] Generating batch proof for " << targets.size() << " targets" << std::endl;

    // Use set for automatic deduplication
    std::set<UtreexoHash> unique_proof_hashes;

    size_t found_count = 0;
    size_t missing_count = 0;

    // Step 1: For each target, find its position and generate proof
    for (const UtreexoHash& target : targets) {
        // Find leaf position
        auto it = leaf_positions_.find(target);
        if (it == leaf_positions_.end()) {
            std::cout << "⚠️  [Utreexo Proof Gen] Target not found in forest: ";
            for (size_t i = 0; i < std::min(size_t(8), target.size()); i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)target[i];
            }
            std::cout << std::dec << "..." << std::endl;
            missing_count++;
            continue;
        }

        uint64_t position = it->second;
        found_count++;

        // Step 2: Generate proof for this position
        auto proof_opt = prove(position);
        if (!proof_opt.has_value()) {
            std::cout << "⚠️  [Utreexo Proof Gen] Failed to generate proof for position " << position << std::endl;
            continue;
        }

        const UtreexoProof& proof = proof_opt.value();

        // Step 3: Add all sibling hashes to deduplicated set
        for (const UtreexoHash& sibling : proof.siblings) {
            unique_proof_hashes.insert(sibling);
        }
    }

    // Step 4: Convert set to vector
    proof_hashes.assign(unique_proof_hashes.begin(), unique_proof_hashes.end());

    std::cout << "✅ [Utreexo Proof Gen] Generated proof:" << std::endl;
    std::cout << "   Targets:       " << targets.size() << std::endl;
    std::cout << "   Found:         " << found_count << std::endl;
    std::cout << "   Missing:       " << missing_count << std::endl;
    std::cout << "   Proof hashes:  " << proof_hashes.size() << " (deduplicated)" << std::endl;
    std::cout << "   Unique hashes: " << unique_proof_hashes.size() << std::endl;

    return proof_hashes;
}

// ═══════════════════════════════════════════════════════════════════════════
// Complete Block Proof Generation (Stateless-Compatible)
// ═══════════════════════════════════════════════════════════════════════════

BlockUtreexoProof UtreexoForest::generateBlockProof(const std::vector<UtreexoHash>& targets) const {
    // ────────────────────────────────────────────────────────────────────────
    // MINER-SIDE: Generate complete block proof with positions
    // ────────────────────────────────────────────────────────────────────────
    // This generates a proof that can be verified statelessly (without UTXO DB).
    // Includes positions for each target so verifiers don't need leaf_positions_.
    //
    // Returns BlockUtreexoProof with:
    // - targets: Leaf hashes being proven
    // - positions: Leaf positions (one per target)
    // - proof_hashes: Deduplicated Merkle siblings
    // - numLeaves: Total leaves in forest at proof time
    // ────────────────────────────────────────────────────────────────────────

    BlockUtreexoProof block_proof;
    block_proof.numLeaves = numLeaves_;

    // Handle empty targets
    if (targets.empty()) {
        std::cout << "ℹ️  [Block Proof Gen] No targets - empty proof" << std::endl;
        return block_proof;
    }

    std::cout << "🔍 [Block Proof Gen] Generating block proof for " << targets.size()
              << " targets (numLeaves=" << numLeaves_ << ")" << std::endl;

    size_t found_count = 0;
    size_t missing_count = 0;
    SubtreeHashCache subtree_cache(MAX_TREE_HEIGHT + 1);

    // For each target, find position and generate proof
    // Proof hashes are stored in per-target sequential order:
    //   [target0_sibling0, target0_sibling1, ..., target1_sibling0, ...]
    // This matches the consumption order in verifyBatchProofStateless().
    for (const UtreexoHash& target : targets) {
        // Find leaf position
        auto it = leaf_positions_.find(target);
        if (it == leaf_positions_.end()) {
            std::cout << "⚠️  [Block Proof Gen] Target not found in forest: ";
            for (size_t i = 0; i < std::min(size_t(8), target.size()); i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)target[i];
            }
            std::cout << std::dec << "..." << std::endl;
            missing_count++;

            // Add placeholder position (0) and target for consistency
            // This will cause verification to fail, which is correct behavior
            block_proof.targets.push_back(target);
            block_proof.positions.push_back(0);
            continue;
        }

        uint64_t position = it->second;
        found_count++;

        // Add target and its position
        block_proof.targets.push_back(target);
        block_proof.positions.push_back(position);

        // Generate individual proof for this position
        auto proof_opt = proveWithCache(position, &subtree_cache);
        if (!proof_opt.has_value()) {
            std::cout << "⚠️  [Block Proof Gen] Failed to generate proof for position " << position << std::endl;
            continue;
        }

        const UtreexoProof& proof = proof_opt.value();

        // Append siblings in path order (leaf→root) for this target
        for (const UtreexoHash& sibling : proof.siblings) {
            block_proof.proof_hashes.push_back(sibling);
        }
    }

    std::cout << "✅ [Block Proof Gen] Generated block proof:" << std::endl;
    std::cout << "   Targets:       " << block_proof.targets.size() << std::endl;
    std::cout << "   Positions:     " << block_proof.positions.size() << std::endl;
    std::cout << "   Found:         " << found_count << std::endl;
    std::cout << "   Missing:       " << missing_count << std::endl;
    std::cout << "   Proof hashes:  " << block_proof.proof_hashes.size() << " (per-target sequential)" << std::endl;
    std::cout << "   numLeaves:     " << block_proof.numLeaves << std::endl;

    return block_proof;
}

BlockUtreexoProof UtreexoForest::generateBlockProof(
    const std::vector<UtreexoHash>& targets,
    uint8_t format_version) const {
    BlockUtreexoProof proof = generateBlockProof(targets);
    proof.format_version = format_version;
    return proof;
}

UtreexoHash UtreexoForest::parentHash(const UtreexoHash& left, const UtreexoHash& right) const {
    return HashNode(left, right);
}

std::optional<uint64_t> UtreexoForest::findLeafPosition(const UtreexoHash& leafHash) const {
    // Try fast lookup first
    auto it = leaf_positions_.find(leafHash);
    if (it != leaf_positions_.end()) {
        const uint64_t position = it->second;
        const bool live_match =
            position < nodes_.size() &&
            position < numLeaves_ &&
            nodes_[position].has_value() &&
            nodes_[position].value() == leafHash &&
            deleted_positions_.count(position) == 0;
        if (live_match) {
            return position;
        }
    }

    // Fallback: linear search through nodes_ (slower but works if map not available)
    for (uint64_t i = 0; i < numLeaves_ && i < nodes_.size(); i++) {
        if (deleted_positions_.count(i) != 0) {
            continue;
        }
        if (nodes_[i].has_value() && nodes_[i].value() == leafHash) {
            return i;
        }
    }

    return std::nullopt;
}

bool UtreexoForest::validateLeafIndexConsistency() const {
    for (const auto& [leafHash, position] : leaf_positions_) {
        if (position >= numLeaves_ || position >= nodes_.size()) {
            return false;
        }
        if (deleted_positions_.count(position) != 0) {
            return false;
        }
        if (!nodes_[position].has_value() || nodes_[position].value() != leafHash) {
            return false;
        }
    }

    for (uint64_t i = 0; i < numLeaves_ && i < nodes_.size(); ++i) {
        if (deleted_positions_.count(i) != 0 || !nodes_[i].has_value()) {
            continue;
        }
        auto it = leaf_positions_.find(nodes_[i].value());
        if (it == leaf_positions_.end() || it->second != i) {
            return false;
        }
    }

    for (uint64_t pos : deleted_positions_) {
        if (pos >= numLeaves_) {
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> UtreexoForest::serialize() const {
    std::vector<uint8_t> data;

    // Version 3: option-aware serialization + canonical_empty_roots_ flag.
    //
    // v2 omitted `canonical_empty_roots_`, so a snapshot taken post-fork
    // (where empty subtrees commit as ZERO_HASH) round-tripped through
    // deserialize() with the flag reset to false. The deserialize-time
    // rebuildRoots() then recomputed subtree hashes under the legacy
    // flag-off rule and produced nullopt where the stored roots held
    // ZERO_HASH; the post-rebuild self-check failed and silently
    // returned an empty forest. Restore()'s only feedback was a stderr
    // warning, so the daemon proceeded with a wiped forest.
    //
    // v3 prepends the flag byte after the version byte. v2 payloads
    // remain readable (flag defaults to false, which is the pre-fork
    // mode they were written under).
    //
    // Audit gap #10 — debug-only injection knob. With the env var
    //   DINERO_FOREST_SERIALIZE_LEGACY_V2=1
    // set at the moment of this call, the function emits the v2
    // layout (no flag byte) so a regtest fixture or unit test
    // can synthesize the exact pre-a72053a9a failure shape from
    // the LA 2026-04-28 audit. Test-only; production daemons
    // never set this var.
    //
    // Read per call (not cached as a static) so unit tests that
    // setenv()/unsetenv() mid-test can flip the knob between
    // serialize() calls in the same process. Cost: one getenv()
    // per per-block forest checkpoint write — negligible at
    // mainnet block cadence.
    const char* legacy_env = std::getenv("DINERO_FOREST_SERIALIZE_LEGACY_V2");
    const bool inject_legacy_v2 =
        legacy_env != nullptr && *legacy_env != '\0' && *legacy_env != '0';
    if (inject_legacy_v2) {
        data.push_back(2);  // Force the pre-fix v2 layout.
    } else {
        data.push_back(3);  // Version byte
        data.push_back(canonical_empty_roots_ ? 1 : 0);
    }

    // 1. Number of leaves (8 bytes)
    data.push_back(numLeaves_ & 0xFF);
    data.push_back((numLeaves_ >> 8) & 0xFF);
    data.push_back((numLeaves_ >> 16) & 0xFF);
    data.push_back((numLeaves_ >> 24) & 0xFF);
    data.push_back((numLeaves_ >> 32) & 0xFF);
    data.push_back((numLeaves_ >> 40) & 0xFF);
    data.push_back((numLeaves_ >> 48) & 0xFF);
    data.push_back((numLeaves_ >> 56) & 0xFF);

    // 2. Number of roots (4 bytes) - only non-empty roots
    auto nonEmptyRoots = getRoots();
    uint32_t numRoots = nonEmptyRoots.size();
    data.push_back(numRoots & 0xFF);
    data.push_back((numRoots >> 8) & 0xFF);
    data.push_back((numRoots >> 16) & 0xFF);
    data.push_back((numRoots >> 24) & 0xFF);

    // 3. Root hashes (32 bytes each) - only non-empty roots
    for (const UtreexoHash& root : nonEmptyRoots) {
        data.insert(data.end(), root.begin(), root.end());
    }

    // 4. Number of internal nodes (4 bytes) - Phase 4: Complete state serialization
    uint32_t numNodes = nodes_.size();
    data.push_back(numNodes & 0xFF);
    data.push_back((numNodes >> 8) & 0xFF);
    data.push_back((numNodes >> 16) & 0xFF);
    data.push_back((numNodes >> 24) & 0xFF);

    // 5. Internal node hashes with presence flags (1 + 32 bytes each if present)
    // This is the key change: we now explicitly serialize whether a node exists
    for (const auto& node : nodes_) {
        if (node.has_value()) {
            data.push_back(1);  // Present flag
            data.insert(data.end(), node.value().begin(), node.value().end());
        } else {
            data.push_back(0);  // Absent flag
        }
    }

    // 6. Number of deleted positions (4 bytes) - For UTXO removal
    uint32_t numDeleted = deleted_positions_.size();
    data.push_back(numDeleted & 0xFF);
    data.push_back((numDeleted >> 8) & 0xFF);
    data.push_back((numDeleted >> 16) & 0xFF);
    data.push_back((numDeleted >> 24) & 0xFF);

    // 7. Deleted positions (8 bytes each) - Track spent UTXOs.
    // Emitted SORTED: deleted_positions_ is an unordered_set whose
    // iteration order depends on its mutation history, so two forests with
    // identical contents could serialize different bytes (observed as the
    // continuous-vs-checkpoint+delta-replay byte divergence in the forest
    // checkpoint delta campaign equivalence suite). Sorting makes the
    // persisted form canonical; deserialize rebuilds the set and never
    // cared about order, and pre-fix blobs remain readable.
    std::vector<uint64_t> sorted_deleted(deleted_positions_.begin(),
                                         deleted_positions_.end());
    std::sort(sorted_deleted.begin(), sorted_deleted.end());
    for (uint64_t pos : sorted_deleted) {
        data.push_back(pos & 0xFF);
        data.push_back((pos >> 8) & 0xFF);
        data.push_back((pos >> 16) & 0xFF);
        data.push_back((pos >> 24) & 0xFF);
        data.push_back((pos >> 32) & 0xFF);
        data.push_back((pos >> 40) & 0xFF);
        data.push_back((pos >> 48) & 0xFF);
        data.push_back((pos >> 56) & 0xFF);
    }

    return data;
}

UtreexoForest UtreexoForest::deserialize(const std::vector<uint8_t>& data) {
    UtreexoForest forest;

    if (data.size() < 1) {
        return forest;
    }

    size_t offset = 0;

    // Rooted-husk hazard (on-device 2026-07-16, height 62742): the payload
    // parses numLeaves_/roots_ first, so returning the partial forest on a
    // short read yields an object whose getCommitment() matches the original
    // accumulator while nodes_/leaf_positions_ are missing — it passes every
    // root check and fails every leaf lookup, permanently wedging the first
    // spend after a restore. Deserialization is all-or-nothing: any short
    // payload past the header fails LOUDLY with an empty forest, matching
    // the tail-validation convention.
    auto refuse_partial = [](const char* what) {
        std::cerr << "❌ [Utreexo Deserialize] " << what
                  << " — refusing partial forest (rooted-husk hazard)" << std::endl;
        return UtreexoForest();
    };


    // Version byte handling:
    //   v3: 1 byte version + 1 byte canonical_empty_roots_ flag
    //   v2: 1 byte version, no flag (flag defaults to false)
    //   v1: no version byte, no flag
    uint8_t version = data[0];
    bool isVersion2OrLater = (version == 2 || version == 3);

    if (isVersion2OrLater) {
        offset = 1;  // Skip version byte
    }

    if (version == 3) {
        if (data.size() < offset + 1) {
            return forest;
        }
        forest.canonical_empty_roots_ = (data[offset] != 0);
        offset += 1;
    }
    // v1 and v2 leave canonical_empty_roots_ at its default (false). For
    // post-fork chainstates that were checkpointed under v2, the missing
    // flag is recovered by the caller via the same setCanonicalEmptyRoots
    // hook ConnectBlockInternal uses on first sight of an active height.
    const bool isVersion2 = isVersion2OrLater;

    if (data.size() < offset + 12) {  // Min: 8 (numLeaves) + 4 (numRoots)
        return forest;
    }

    // 1. Number of leaves
    forest.numLeaves_ = data[offset] | ((uint64_t)data[offset+1] << 8) |
                       ((uint64_t)data[offset+2] << 16) | ((uint64_t)data[offset+3] << 24) |
                       ((uint64_t)data[offset+4] << 32) | ((uint64_t)data[offset+5] << 40) |
                       ((uint64_t)data[offset+6] << 48) | ((uint64_t)data[offset+7] << 56);
    offset += 8;

    // 2. Number of roots
    uint32_t numRoots = data[offset] | (data[offset+1] << 8) |
                       (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // 3. Root hashes (only non-empty ones were serialized)
    // FIX: Check for integer overflow before multiplication
    uint64_t roots_bytes = static_cast<uint64_t>(numRoots) * 32;
    if (roots_bytes > SIZE_MAX || data.size() < offset + roots_bytes) {
        return refuse_partial("roots section short or overflowing");
    }

    std::vector<UtreexoHash> nonEmptyRoots;
    for (uint32_t i = 0; i < numRoots; i++) {
        UtreexoHash root(data.begin() + offset, data.begin() + offset + 32);
        nonEmptyRoots.push_back(root);
        offset += 32;
    }

    // Rebuild full roots_ array with empties in correct positions
    // Based on binary representation of numLeaves_
    uint64_t n = forest.numLeaves_;
    size_t rootIndex = 0;
    for (uint8_t h = 0; h < 64 && n > 0; h++) {
        if (n & 1) {
            // There's a tree at this height
            if (rootIndex < nonEmptyRoots.size()) {
                // Make sure roots_ is large enough
                while (forest.roots_.size() <= h) {
                    forest.roots_.push_back(std::nullopt);
                }
                forest.roots_[h] = nonEmptyRoots[rootIndex];
                rootIndex++;
            }
        }
        n >>= 1;
    }

    // 4. Number of internal nodes (Phase 4: Complete state deserialization)
    if (data.size() < offset + 4) {
        // Pre-Phase-4 payloads (roots only, no nodes_) restore as EXACTLY the
        // rooted husk described above. Every live writer emits the full v3
        // state; a roots-only payload is either ancient or truncated — refuse.
        return refuse_partial("payload ends after roots section (no nodes_)");
    }

    uint32_t numNodes = data[offset] | (data[offset+1] << 8) |
                       (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // 5. Internal node hashes - format depends on version
    if (isVersion2) {
        // Version 2: Each node has 1-byte presence flag + 32 bytes if present
        for (uint32_t i = 0; i < numNodes; i++) {
            if (data.size() < offset + 1) {
                return refuse_partial("nodes section truncated (presence flag)");
            }
            uint8_t present = data[offset++];
            if (present) {
                if (data.size() < offset + 32) {
                    return refuse_partial("nodes section truncated (node hash)");
                }
                UtreexoHash node(data.begin() + offset, data.begin() + offset + 32);
                forest.nodes_.push_back(node);
                offset += 32;
            } else {
                forest.nodes_.push_back(std::nullopt);
            }
        }
    } else {
        // Version 1 (legacy): 32 bytes per node, empty vector means absent
        uint64_t nodes_bytes = static_cast<uint64_t>(numNodes) * 32;
        if (nodes_bytes > SIZE_MAX || data.size() < offset + nodes_bytes) {
            return refuse_partial("legacy nodes section short or overflowing");
        }

        for (uint32_t i = 0; i < numNodes; i++) {
            UtreexoHash node(data.begin() + offset, data.begin() + offset + 32);
            // In legacy format, check if node is all zeros (32 zeros = empty)
            bool allZeros = true;
            for (int j = 0; j < 32 && allZeros; j++) {
                if (node[j] != 0) allZeros = false;
            }
            if (allZeros) {
                forest.nodes_.push_back(std::nullopt);
            } else {
                forest.nodes_.push_back(node);
            }
            offset += 32;
        }
    }

    // 6. Number of deleted positions (4 bytes) - For UTXO removal
    if (data.size() < offset + 4) {
        // A payload without the deleted-positions section skips the tombstones
        // AND the stored-vs-rebuilt roots cross-check below — a truncation here
        // silently resurrects spent leaves under the original commitment. All
        // live writers emit the full v3 state — refuse.
        return refuse_partial("payload ends before deleted-positions section");
    }

    uint32_t numDeleted = data[offset] | (data[offset+1] << 8) |
                         (data[offset+2] << 16) | (data[offset+3] << 24);
    offset += 4;

    // 7. Deleted positions (8 bytes each) - Track spent UTXOs
    // FIX: Check for integer overflow before multiplication
    uint64_t deleted_bytes = static_cast<uint64_t>(numDeleted) * 8;
    if (deleted_bytes > SIZE_MAX || data.size() < offset + deleted_bytes) {
        return refuse_partial("deleted-positions section short or overflowing");
    }

    for (uint32_t i = 0; i < numDeleted; i++) {
        uint64_t pos = data[offset] | ((uint64_t)data[offset+1] << 8) |
                      ((uint64_t)data[offset+2] << 16) | ((uint64_t)data[offset+3] << 24) |
                      ((uint64_t)data[offset+4] << 32) | ((uint64_t)data[offset+5] << 40) |
                      ((uint64_t)data[offset+6] << 48) | ((uint64_t)data[offset+7] << 56);
        forest.deleted_positions_.insert(pos);
        offset += 8;
    }

    // Rebuild leaf_positions_ map from nodes_ (excluding deleted positions)
    // The first numLeaves_ entries in nodes_ are the leaves
    for (uint64_t i = 0; i < forest.numLeaves_ && i < forest.nodes_.size(); i++) {
        // Skip deleted positions
        if (forest.isDeleted(i)) {
            continue;
        }
        if (forest.nodes_[i].has_value()) {
            const auto inserted = forest.leaf_positions_.emplace(forest.nodes_[i].value(), i);
            if (!inserted.second) {
                std::cerr << "❌ [Utreexo Deserialize] Duplicate live leaf hash in payload" << std::endl;
                return UtreexoForest();
            }
        }
    }

    if (!forest.validateLeafIndexConsistency()) {
        std::cerr << "❌ [Utreexo Deserialize] leaf_positions_ invariant failed" << std::endl;
        return UtreexoForest();
    }

    // Audit gap #10c: a v2 payload (no flag byte → flag defaulted to
    // false) whose source had canonical_empty_roots_=true with at
    // least one fully-deleted root contains stored ZERO_HASH cells
    // for those drained roots. rebuildRoots() under flag=false
    // recomputes those subtrees as nullopt instead of ZERO_HASH, so
    // the stored-vs-rebuilt comparison fails and the forest gets
    // silently wiped (returned default-constructed). Smallest
    // reproducible shape: numLeaves=1, leaf 0 drained — the
    // entire forest is one fully-deleted size-1 root.
    //
    // Fix: try flag=false first (which is correct for genuine pre-
    // canonical-roots-fork v1/v2 payloads). If the rebuild mismatch
    // shows up AND we're reading a v2 payload (no flag byte
    // available), retry with flag=true. If THAT also mismatches,
    // the data really is inconsistent and we return empty.
    //
    // Cost: at most one extra rebuildRoots() call on v2 payloads
    // that came from a flag=true source. v3 payloads carry the
    // flag explicitly and skip this fallback entirely.
    const auto serialized_commitment = forest.getCommitment();
    auto stored_roots = forest.roots_;
    forest.rebuildRoots();
    if (forest.roots_ != stored_roots) {
        if (version == 2) {
            // Maybe the v2 payload came from a flag=true source.
            // Retry the rebuild under that assumption.
            forest.canonical_empty_roots_ = true;
            forest.rebuildRoots();
            if (forest.roots_ != stored_roots) {
                std::cerr << "❌ [Utreexo Deserialize] Serialized roots do not match"
                          << " node/deletion state under either flag value"
                          << std::endl;
                return UtreexoForest();
            }
            // The flag=true rebuild matched; honor the inferred flag.
            // Without this, ConnectBlockInternal's first-sight
            // setCanonicalEmptyRoots hook still flips it back at the
            // next active block, but in the meantime the forest is
            // operating with the wrong flag value.
        } else {
            std::cerr << "❌ [Utreexo Deserialize] Serialized roots do not match node/deletion state" << std::endl;
            return UtreexoForest();
        }
    }
    if (forest.getCommitment() != serialized_commitment) {
        std::cerr << "❌ [Utreexo Deserialize] Commitment changed after root rebuild" << std::endl;
        return UtreexoForest();
    }

    return forest;
}

UtreexoForest::Stats UtreexoForest::getStats() const {
    Stats stats;
    stats.numLeaves = numLeaves_;
    stats.numRoots = roots_.size();
    stats.totalSize = roots_.size() * 32 + nodes_.size() * 32;

    // Average proof size = log2(numLeaves) * 32 bytes
    if (numLeaves_ > 0) {
        uint8_t height = 0;
        uint64_t n = numLeaves_;
        while (n > 1) {
            n >>= 1;
            height++;
        }
        stats.avgProofSize = height * 32 + 16;  // siblings + metadata
    } else {
        stats.avgProofSize = 0;
    }

    return stats;
}

// ═══════════════════════════════════════════════════════════════════════════
// Batch Operations
// ═══════════════════════════════════════════════════════════════════════════
// Batched Proof Verification (Consensus-Critical)
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoForest::verifyBatchProof(const std::vector<UtreexoHash>& targets,
                                     const std::vector<UtreexoHash>& proof_hashes) const {
    // ────────────────────────────────────────────────────────────────────────
    // CONSENSUS-CRITICAL: Block-level Utreexo proof verification
    // ────────────────────────────────────────────────────────────────────────
    // This function verifies that all targets (spent UTXO leaf hashes) existed
    // in the accumulator using a batched Merkle proof.
    //
    // Algorithm Overview:
    // 1. Validate inputs (empty checks, sanity checks)
    // 2. Verify proof structure (sufficient proof_hashes for targets)
    // 3. Reconstruct Merkle roots from targets + proof_hashes
    // 4. Compare reconstructed roots with actual forest roots
    // ────────────────────────────────────────────────────────────────────────

    // ═════════════════════════════════════════════════════════════════════════
    // Step 1: Input Validation
    // ═════════════════════════════════════════════════════════════════════════

    // Empty targets is valid (coinbase-only blocks have no spends)
    if (targets.empty()) {
        // Proof should also be empty for coinbase-only blocks
        if (!proof_hashes.empty()) {
            std::cout << "❌ [Utreexo Verify] Empty targets but non-empty proof" << std::endl;
            return false;
        }
        std::cout << "✅ [Utreexo Verify] Empty proof (coinbase-only block)" << std::endl;
        return true;
    }

    // Targets present but empty forest = invalid state
    if (isEmpty()) {
        std::cout << "❌ [Utreexo Verify] Cannot verify targets against empty forest" << std::endl;
        return false;
    }

    // Basic sanity: proof_hashes should be non-empty when targets exist
    // (unless all targets are roots, which is rare)
    std::cout << "🔍 [Utreexo Verify] Verifying " << targets.size()
              << " targets with " << proof_hashes.size() << " proof hashes" << std::endl;

    // ═════════════════════════════════════════════════════════════════════════
    // Step 2: Structural Validation
    // ═════════════════════════════════════════════════════════════════════════

    // Validate hash sizes (all should be 32 bytes)
    for (const UtreexoHash& target : targets) {
        if (target.size() != 32) {
            std::cout << "❌ [Utreexo Verify] Invalid target hash size: " << target.size() << std::endl;
            return false;
        }
    }

    for (const UtreexoHash& proof_hash : proof_hashes) {
        if (proof_hash.size() != 32) {
            std::cout << "❌ [Utreexo Verify] Invalid proof hash size: " << proof_hash.size() << std::endl;
            return false;
        }
    }

    // Check for duplicate targets (invalid proof structure)
    std::set<UtreexoHash> unique_targets(targets.begin(), targets.end());
    if (unique_targets.size() != targets.size()) {
        std::cout << "❌ [Utreexo Verify] Duplicate targets in proof" << std::endl;
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Step 3: Cryptographic Verification (Full Implementation)
    // ═════════════════════════════════════════════════════════════════════════
    //
    // Verify each target using its Merkle path.
    // Algorithm:
    // 1. Find position of each target in forest
    // 2. Generate expected proof for that position
    // 3. Verify all required siblings are in proof_hashes
    // 4. Verify Merkle path computes to a valid root
    //
    // ═════════════════════════════════════════════════════════════════════════

    // Convert proof_hashes to set for O(1) lookup
    std::set<UtreexoHash> proof_set(proof_hashes.begin(), proof_hashes.end());

    size_t verified_count = 0;
    size_t failed_count = 0;

    // Verify each target
    for (const UtreexoHash& target : targets) {
        // Step 1: Find position of this target
        auto position_opt = findLeafPosition(target);
        if (!position_opt.has_value()) {
            std::cout << "❌ [Utreexo Verify] Target not found in forest: ";
            for (size_t i = 0; i < std::min(size_t(8), target.size()); i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)target[i];
            }
            std::cout << std::dec << "..." << std::endl;
            return false;
        }

        uint64_t position = position_opt.value();

        // Step 2: Generate expected proof for this position
        auto expected_proof_opt = prove(position);
        if (!expected_proof_opt.has_value()) {
            std::cout << "❌ [Utreexo Verify] Failed to generate proof for position " << position << std::endl;
            return false;
        }

        const UtreexoProof& expected_proof = expected_proof_opt.value();

        // Step 3: Verify all required siblings are in proof_hashes
        for (const UtreexoHash& required_sibling : expected_proof.siblings) {
            if (proof_set.find(required_sibling) == proof_set.end()) {
                std::cout << "❌ [Utreexo Verify] Missing required sibling hash" << std::endl;
                return false;
            }
        }

        // Step 4: Verify the Merkle path using existing verification
        if (!expected_proof.verify(target, getRoots())) {
            std::cout << "❌ [Utreexo Verify] Merkle path verification failed for target" << std::endl;
            return false;
        }

        verified_count++;
    }

    std::cout << "✅ [Utreexo Verify] Cryptographic verification passed" << std::endl;
    std::cout << "   Verified: " << verified_count << "/" << targets.size() << " targets" << std::endl;

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Stateless Batch Proof Verification (Consensus-Critical)
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoForest::verifyBatchProofStateless(
    const std::vector<UtreexoHash>& targets,
    const std::vector<uint64_t>& positions,
    const std::vector<UtreexoHash>& proof_hashes,
    uint64_t proofNumLeaves,
    const std::vector<UtreexoHash>& expectedRoots) const {
    // ────────────────────────────────────────────────────────────────────────
    // CONSENSUS-CRITICAL: Stateless block-level Utreexo proof verification
    // ────────────────────────────────────────────────────────────────────────
    // This function verifies proofs WITHOUT requiring local UTXO database.
    // All information needed is provided in the proof itself.
    //
    // Algorithm:
    // 1. Validate inputs (sizes match, positions valid, no duplicates)
    // 2. Structural validation (hash sizes, position bounds)
    // 3. For each target, extract siblings from proof_hashes (sequential
    //    order), construct individual UtreexoProof, verify against roots
    // ────────────────────────────────────────────────────────────────────────

    // ═════════════════════════════════════════════════════════════════════════
    // Step 1: Input Validation
    // ═════════════════════════════════════════════════════════════════════════

    // Empty targets is valid (coinbase-only blocks have no spends)
    if (targets.empty()) {
        if (!proof_hashes.empty() || !positions.empty()) {
            std::cout << "❌ [Utreexo Stateless] Empty targets but non-empty proof/positions" << std::endl;
            return false;
        }
        std::cout << "✅ [Utreexo Stateless] Empty proof (coinbase-only block)" << std::endl;
        return true;
    }

    // Positions must match targets (one position per target)
    if (positions.size() != targets.size()) {
        std::cout << "❌ [Utreexo Stateless] Position count mismatch: "
                  << positions.size() << " != " << targets.size() << std::endl;
        return false;
    }

    // proofNumLeaves must be non-zero for non-empty targets
    if (proofNumLeaves == 0) {
        std::cout << "❌ [Utreexo Stateless] Zero numLeaves with non-empty targets" << std::endl;
        return false;
    }

    // Medium Priority Fix: Bound numLeaves to prevent overflow in position arithmetic
    if (proofNumLeaves > MAX_UTREEXO_LEAVES) {
        std::cout << "❌ [Utreexo Stateless] numLeaves exceeds maximum: "
                  << proofNumLeaves << " > " << MAX_UTREEXO_LEAVES << std::endl;
        return false;
    }

    // expectedRoots must be provided
    if (expectedRoots.empty()) {
        std::cout << "❌ [Utreexo Stateless] No expected roots provided" << std::endl;
        return false;
    }

    std::cout << "🔍 [Utreexo Stateless] Verifying " << targets.size()
              << " targets with " << proof_hashes.size() << " proof hashes"
              << " (numLeaves=" << proofNumLeaves << ")" << std::endl;

    // ═════════════════════════════════════════════════════════════════════════
    // Step 2: Structural Validation
    // ═════════════════════════════════════════════════════════════════════════

    // Validate hash sizes
    for (const UtreexoHash& target : targets) {
        if (target.size() != 32) {
            std::cout << "❌ [Utreexo Stateless] Invalid target hash size: " << target.size() << std::endl;
            return false;
        }
    }

    for (const UtreexoHash& proof_hash : proof_hashes) {
        if (proof_hash.size() != 32) {
            std::cout << "❌ [Utreexo Stateless] Invalid proof hash size: " << proof_hash.size() << std::endl;
            return false;
        }
    }

    // Validate positions (must be < proofNumLeaves)
    for (uint64_t pos : positions) {
        if (pos >= proofNumLeaves) {
            std::cout << "❌ [Utreexo Stateless] Invalid position " << pos
                      << " >= numLeaves " << proofNumLeaves << std::endl;
            return false;
        }
    }

    // Positions must be unique (one spend per leaf position).
    std::unordered_set<uint64_t> unique_positions(positions.begin(), positions.end());
    if (unique_positions.size() != positions.size()) {
        std::cout << "❌ [Utreexo Stateless] Duplicate positions in proof" << std::endl;
        return false;
    }

    // Check for duplicate targets
    std::set<UtreexoHash> unique_targets(targets.begin(), targets.end());
    if (unique_targets.size() != targets.size()) {
        std::cout << "❌ [Utreexo Stateless] Duplicate targets in proof" << std::endl;
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Step 3: Verify each target by constructing individual Merkle proofs
    // ═════════════════════════════════════════════════════════════════════════
    // Proof hashes are stored in per-target sequential order:
    //   [target0_sib0, target0_sib1, ..., target1_sib0, ...]
    // Each target consumes siblings equal to its tree height.

    size_t verified_count = 0;
    size_t proof_hash_idx = 0;

    for (size_t i = 0; i < targets.size(); i++) {
        const UtreexoHash& target = targets[i];
        uint64_t position = positions[i];

        // Construct a UtreexoProof for this target
        UtreexoProof individual_proof;
        individual_proof.position = position;
        individual_proof.numLeaves = proofNumLeaves;

        // Determine tree height for this position
        std::vector<std::pair<uint64_t, uint64_t>> trees;
        uint64_t n = proofNumLeaves;
        uint64_t tree_start = 0;

        for (int h = 63; h >= 0; h--) {
            if ((n >> h) & 1) {
                uint64_t tree_size = 1ULL << h;
                trees.push_back({tree_start, tree_size});
                tree_start += tree_size;
            }
        }

        // Find tree containing position
        uint64_t tree_size = 0;
        for (const auto& [start, size] : trees) {
            if (position >= start && position < start + size) {
                tree_size = size;
                break;
            }
        }

        // Calculate path length (tree height)
        int path_length = 0;
        while ((1ULL << path_length) < tree_size) {
            path_length++;
        }

        // Extract siblings from proof_hashes (stored in per-target sequential order)
        // Each target's siblings appear consecutively: [t0_sib0, t0_sib1, ..., t1_sib0, ...]
        for (int j = 0; j < path_length && proof_hash_idx < proof_hashes.size(); j++) {
            individual_proof.siblings.push_back(proof_hashes[proof_hash_idx++]);
        }

        // Verify the individual proof
        if (!individual_proof.verify(target, expectedRoots)) {
            std::cout << "❌ [Utreexo Stateless] Proof verification failed for target " << i << std::endl;
            return false;
        }

        verified_count++;
    }

    std::cout << "✅ [Utreexo Stateless] Verification passed" << std::endl;
    std::cout << "   Verified: " << verified_count << "/" << targets.size() << " targets" << std::endl;

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoBatchUpdate::apply(UtreexoForest& forest) const {
    // Remove UTXOs first
    for (const auto& [leafHash, proof] : removes) {
        if (!forest.remove(leafHash, proof)) {
            return false;  // Removal failed
        }
    }

    // Add new UTXOs
    for (const UtreexoHash& leafHash : adds) {
        if (forest.add(leafHash) == UINT64_MAX) {
            return false;  // Duplicate/capacity failure
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 4: Delta-Based Undo Operations
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoForest::restoreDeletedLeaf(uint64_t position, const UtreexoHash& leafHash) {
    // ────────────────────────────────────────────────────────────────────────
    // Phase 4: Restore a deleted leaf (inverse of remove())
    // ────────────────────────────────────────────────────────────────────────
    // This is called during DisconnectBlock to undo a leaf deletion.
    //
    // Algorithm:
    // 1. Verify position was actually deleted
    // 2. Restore leaf hash to nodes_[position]
    // 3. Remove position from deleted_positions_
    // 4. Restore leaf_positions_ mapping (hash → position)
    // 5. Recompute path to root (to update parent hashes)
    // ────────────────────────────────────────────────────────────────────────

    // Validate inputs
    if (position >= numLeaves_) {
        std::cout << "❌ [Utreexo Restore] Invalid position: " << position
                  << " (numLeaves=" << numLeaves_ << ")" << std::endl;
        return false;
    }

    if (leafHash.size() != 32) {
        std::cout << "❌ [Utreexo Restore] Invalid leaf hash size: " << leafHash.size() << std::endl;
        return false;
    }

    // Verify position was deleted
    if (!isDeleted(position)) {
        std::cout << "❌ [Utreexo Restore] Position " << position << " was not deleted" << std::endl;
        return false;
    }

    std::cout << "🔄 [Utreexo Restore] Restoring leaf at position " << position << std::endl;

    // 1. Validate leaf lookup map won't collide with another live leaf.
    // Do this before mutating any state so a failed restore cannot leave the
    // forest half-restored.
    auto existing_it = leaf_positions_.find(leafHash);
    if (existing_it != leaf_positions_.end() && existing_it->second != position &&
        !isDeleted(existing_it->second)) {
        std::cout << "❌ [Utreexo Restore] Duplicate live leaf hash collision at position "
                  << existing_it->second << std::endl;
        return false;
    }

    // 2. Restore leaf hash to nodes_
    if (position >= nodes_.size()) {
        nodes_.resize(position + 1);
    }
    nodes_[position] = leafHash;

    // 3. Remove from deleted set
    deleted_positions_.erase(position);

    // 4. Restore leaf lookup map
    leaf_positions_[leafHash] = position;

    // 5. Recompute path to root (updates parent hashes and roots)
    recomputePath(position);

#ifdef ENABLE_UTREEXO_INVARIANT_CHECKS
    if (!validateLeafIndexConsistency()) {
        std::cout << "❌ [Utreexo Restore] leaf_positions_ invariant failed after restoreDeletedLeaf()" << std::endl;
        return false;
    }
#endif

    std::cout << "✅ [Utreexo Restore] Leaf restored at position " << position << std::endl;

    return true;
}

bool UtreexoForest::removeLastNLeaves(uint64_t count) {
    // ────────────────────────────────────────────────────────────────────────
    // Phase 4: Remove last N added leaves (inverse of add())
    // ────────────────────────────────────────────────────────────────────────
    // This is called during DisconnectBlock to undo leaf additions.
    //
    // During ConnectBlock, new UTXOs are added via add() which appends
    // to position numLeaves++. To undo, we remove these leaves in reverse.
    //
    // Algorithm:
    // 1. Validate we have enough leaves
    // 2. For each of the last N positions (in reverse):
    //    a. Verify position is not deleted (sanity check)
    //    b. Get leaf hash from nodes_[position]
    //    c. Remove from leaf_positions_ map
    //    d. Clear from nodes_ (optional, for cleanliness)
    // 3. Decrement numLeaves_ by N
    // 4. Recompute roots based on new numLeaves
    // ────────────────────────────────────────────────────────────────────────

    if (count == 0) {
        return true;  // Nothing to do
    }

    if (count > numLeaves_) {
        std::cout << "❌ [Utreexo Remove] Cannot remove " << count
                  << " leaves (only have " << numLeaves_ << ")" << std::endl;
        return false;
    }

    std::cout << "🔄 [Utreexo Remove] Removing last " << count << " leaves" << std::endl;
    std::cout << "   numLeaves before: " << numLeaves_ << std::endl;

    // Track active leaves removed (for logging)
    uint64_t active_removed = 0;

    // Audit gap #5 (tightened + transactional): removeLastNLeaves runs
    // in TWO PASSES so a contract-violation discovered mid-range never
    // leaves the forest with positions numLeaves_-1..numLeaves_-K
    // cleared and the rest still live. Pre-fix the function mutated
    // in-place and returned false on the first violation, leaving an
    // inconsistent forest the caller had no way to roll back.
    //
    // PASS 1 — validate the whole range without mutating anything.
    //   Two contract clauses checked at every position:
    //     (a) position is NOT in deleted_positions_ (else this block's
    //         adds did not live here; caller passed wrong count)
    //     (b) nodes_[position] has a leaf hash (else adds were not
    //         monotonic at numLeaves_ when count was computed)
    //   Any violation: log, return false, forest UNCHANGED. Caller
    //   sees a clean "either everything will roll back or nothing
    //   has" guarantee — the §1 atomic-unit law applied at the
    //   single-function granularity.
    //
    // PASS 2 — apply the mutations. By construction PASS 2 cannot
    //   encounter a contract violation (PASS 1 verified every
    //   position) so it cannot leave the forest partially mutated.
    //
    // Cost: one extra walk over the [numLeaves_-count, numLeaves_)
    // range. count is the number of UTXOs added by ONE block, so
    // even on a heavy block the second walk is O(thousands) at most.
    for (uint64_t i = 0; i < count; i++) {
        const uint64_t position = numLeaves_ - 1 - i;

        if (isDeleted(position)) {
            std::cerr << "❌ [Utreexo Remove] removeLastNLeaves invariant violated: "
                      << "position " << position
                      << " is in deleted_positions_ — caller passed the wrong "
                      << "count or the legacy disconnect path was reached for a "
                      << "block whose adds did not append at the end. Forest "
                      << "left UNCHANGED." << std::endl;
            return false;
        }

        if (position >= nodes_.size() || !nodes_[position].has_value()) {
            std::cerr << "❌ [Utreexo Remove] removeLastNLeaves invariant violated: "
                      << "position " << position
                      << " has no leaf hash — adds were not monotonic at "
                      << "numLeaves at the time the caller's count was computed. "
                      << "Forest left UNCHANGED." << std::endl;
            return false;
        }
    }

    // PASS 2 — mutate. Cannot fail given PASS 1 verified the range.
    for (uint64_t i = 0; i < count; i++) {
        const uint64_t position = numLeaves_ - 1 - i;
        active_removed++;
        const UtreexoHash& leafHash = nodes_[position].value();
        leaf_positions_.erase(leafHash);
        nodes_[position] = std::nullopt;
    }

    // Decrement numLeaves
    numLeaves_ -= count;

    // Truncate nodes_ if possible (optional optimization)
    if (nodes_.size() > numLeaves_) {
        nodes_.resize(numLeaves_);
    }

    // Deleted positions beyond the new frontier are now impossible. Keep the
    // tombstone set aligned with numLeaves_ so reused positions don't look
    // permanently deleted after rollback.
    for (auto it = deleted_positions_.begin(); it != deleted_positions_.end(); ) {
        if (*it >= numLeaves_) {
            it = deleted_positions_.erase(it);
        } else {
            ++it;
        }
    }

    // Recompute roots based on new numLeaves
    // The roots_ array represents the binary decomposition of numLeaves
    // We need to rebuild it based on the new count
    rebuildRoots();

#ifdef ENABLE_UTREEXO_INVARIANT_CHECKS
    if (!validateLeafIndexConsistency()) {
        std::cout << "❌ [Utreexo Remove] leaf_positions_ invariant failed after removeLastNLeaves()" << std::endl;
        return false;
    }
#endif

    std::cout << "✅ [Utreexo Remove] Removed " << count << " leaves" << std::endl;
    std::cout << "   Active removed: " << active_removed << std::endl;
    std::cout << "   numLeaves after: " << numLeaves_ << std::endl;
    std::cout << "   numRoots after: " << getNumRoots() << std::endl;

    return true;
}

void UtreexoForest::rebuildRoots() {
    // ────────────────────────────────────────────────────────────────────────
    // Rebuild roots_ array based on current numLeaves and node state
    // ────────────────────────────────────────────────────────────────────────
    // The roots_ array must match the binary representation of numLeaves.
    // For example:
    // - numLeaves = 5 = 0b101 → 2 roots (at heights 0 and 2)
    // - numLeaves = 8 = 0b1000 → 1 root (at height 3)
    //
    // We scan the binary representation and compute root hashes for each
    // tree in the forest.
    // ────────────────────────────────────────────────────────────────────────

    roots_.clear();

    if (numLeaves_ == 0) {
        return;  // Empty forest, no roots
    }

    // Find which trees exist based on binary representation
    std::vector<std::pair<uint8_t, uint64_t>> trees;  // (height, start_pos)
    uint64_t position = 0;

    // Scan from MSB to LSB to find set bits
    for (int h = 63; h >= 0; h--) {
        if ((numLeaves_ >> h) & 1) {
            uint64_t tree_size = (1ULL << h);
            trees.push_back({static_cast<uint8_t>(h), position});
            position += tree_size;
        }
    }

    // Compute root for each tree
    for (const auto& [height, start_pos] : trees) {
        uint64_t tree_size = (1ULL << height);
        auto root = computeSubtreeHash(start_pos, tree_size);

        // Ensure roots_ has space
        while (roots_.size() <= height) {
            roots_.push_back(std::nullopt);
        }

        roots_[height] = root;  // Assign optional directly
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UtreexoTransitionProof Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::vector<UtreexoHash> UtreexoTransitionProof::computeAdditionHashes(
    const dinero::Block& block,
    uint32_t block_height
) {
    // Pre-scan: identify intra-block ephemeral UTXOs (outputs created and
    // spent within the same block).  These never enter the Utreexo forest.
    std::unordered_map<OutPoint, size_t> intra_block_outputs;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        TxId txid = block.vtx[tx_idx].GetTxid();
        for (uint32_t n = 0; n < block.vtx[tx_idx].vout.size(); n++) {
            intra_block_outputs[OutPoint(txid, n)] = tx_idx;
        }
    }
    std::unordered_set<OutPoint> ephemeral_outputs;
    for (size_t i = 0; i < block.vtx.size(); i++) {
        if (i == 0) continue;  // coinbase has no inputs
        for (const auto& input : block.vtx[i].vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(outpoint)) {
                ephemeral_outputs.insert(outpoint);
            }
        }
    }

    std::vector<UtreexoHash> additions;

    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();

        for (size_t n = 0; n < tx.vout.size(); ++n) {
            // Skip outputs consumed within this block (ephemeral — never enter forest)
            if (ephemeral_outputs.count(OutPoint(txid, static_cast<uint32_t>(n)))) {
                continue;
            }

            const auto& output = tx.vout[n];

            UtreexoHash leafHash = HashUTXOForCreationHeight(
                txid.AsUint256(),
                static_cast<uint32_t>(n),
                output.value.GetUna(),
                std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end()),
                block_height,
                tx.IsCoinbase()
            );

            additions.push_back(leafHash);
        }
    }

    return additions;
}

// const& entry point: the caller keeps its forest, so we take our own copy and
// hand ownership to the rvalue overload below. Exactly one deep copy.
UtreexoTransitionProof UtreexoTransitionProof::generate(
    const UtreexoForest& forest_before,
    const dinero::Block& block,
    const BlockUtreexoProof& spend_proof,
    uint32_t block_height
) {
    return generate(forest_before.clone(), block, spend_proof, block_height);
}

UtreexoTransitionProof UtreexoTransitionProof::generate(
    UtreexoForest&& forest_before,
    const dinero::Block& block,
    const BlockUtreexoProof& spend_proof,
    uint32_t block_height
) {
    UtreexoTransitionProof tp;

    // Pre-state (height-indexed roots)
    tp.roots_before = forest_before.getIndexedRoots();
    tp.num_leaves_before = forest_before.getNumLeaves();

    // Deletion proof fields
    tp.deletion_targets = spend_proof.targets;
    tp.deletion_positions = spend_proof.positions;
    tp.deletion_proof_hashes = spend_proof.proof_hashes;

    // Adopt the caller's forest for the non-destructive simulation. The
    // pre-state above was already read from it, so moving here is safe and
    // saves a full deep copy. (Reachable only via the const& overload's clone
    // or a caller that owns a private forest — never the live one.)
    UtreexoForest snapshot = std::move(forest_before);

    // PASS 1 (batched): resolve each spend target to its live position via
    // the clone's leaf index, then remove them all in ONE call so the roots
    // are rebuilt once. The old per-target prove()+remove() pair rehashed the
    // entire containing subtree for every target — O(targets × forest)
    // hashing — which made 1600-input mainnet block 92742 take ~7 minutes to
    // connect on every node (2026-08-21 incident; regression test
    // UtreexoConnectPerf.GenerateFastForManyTargets). Behavior parity with
    // the old loop: an unresolvable target is skipped (old find/prove
    // failure), and a duplicate target's later occurrence is skipped (its
    // leaf is already scheduled — the old second remove() failed the same
    // way). Proof re-verification is unnecessary here: we hold the full
    // forest, and positions come from its own leaf index (same trust
    // argument as ConnectBlockInternal's removeAtKnownPosition use).
    {
        std::vector<std::pair<uint64_t, UtreexoHash>> removals;
        removals.reserve(spend_proof.targets.size());
        std::unordered_set<uint64_t> scheduled_positions;
        scheduled_positions.reserve(spend_proof.targets.size());
        for (const auto& target : spend_proof.targets) {
            auto position_opt = snapshot.findLeafPosition(target);
            if (!position_opt.has_value()) {
                continue;
            }
            if (!scheduled_positions.insert(position_opt.value()).second) {
                continue;
            }
            removals.emplace_back(position_opt.value(), target);
        }
        if (!removals.empty() && !snapshot.removeAtKnownPositions(removals)) {
            // Structurally unreachable after the liveness checks above, but
            // fail closed the same way the add() failure path below does.
            std::cerr << "[TP-GEN] batched removal failed while building transition proof" << std::endl;
            return UtreexoTransitionProof();
        }
    }

    // Capture intermediate roots (after deletions, before additions) — height-indexed
    tp.roots_after_deletions = snapshot.getIndexedRoots();

    // PASS 2: ADD ALL new outputs
    tp.addition_hashes = computeAdditionHashes(block, block_height);
    for (const auto& leafHash : tp.addition_hashes) {
        if (snapshot.add(leafHash) == UINT64_MAX) {
            std::cerr << "[TP-GEN] add() failed while building transition proof (duplicate/capacity)" << std::endl;
            return UtreexoTransitionProof();
        }
    }

    // Post-state
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = tp.num_leaves_before + tp.addition_hashes.size();

    // DIAGNOSTIC: Dump generate() state (only for blocks with deletions)
    if (!spend_proof.targets.empty()) {
        std::cerr << "[TP-GEN] num_leaves_before=" << tp.num_leaves_before
                  << " deletions=" << spend_proof.targets.size()
                  << " additions=" << tp.addition_hashes.size()
                  << " num_leaves_after=" << tp.num_leaves_after << std::endl;

        // Roots after deletions
        size_t rad_count = 0;
        for (size_t h = 0; h < tp.roots_after_deletions.size(); h++) {
            if (tp.roots_after_deletions[h].has_value()) {
                const auto& r = tp.roots_after_deletions[h].value();
                std::cerr << "[TP-GEN] roots_after_del[" << h << "]: ";
                for (size_t b = 0; b < std::min(r.size(), size_t(8)); b++)
                    std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)r[b];
                std::cerr << std::dec << "..." << std::endl;
                rad_count++;
            }
        }
        std::cerr << "[TP-GEN] roots_after_del: " << rad_count << " non-null, vector size=" << tp.roots_after_deletions.size() << std::endl;

        // Snapshot roots after additions (= expected final state)
        auto snap_roots = snapshot.getIndexedRoots();
        for (size_t h = 0; h < snap_roots.size(); h++) {
            if (snap_roots[h].has_value()) {
                const auto& r = snap_roots[h].value();
                std::cerr << "[TP-GEN] snap_final_roots[" << h << "]: ";
                for (size_t b = 0; b < std::min(r.size(), size_t(8)); b++)
                    std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)r[b];
                std::cerr << std::dec << "..." << std::endl;
            }
        }
        std::cerr << "[TP-GEN] snap numLeaves=" << snapshot.getNumLeaves() << std::endl;

        // Commitment
        const auto& c = tp.commitment_after;
        std::cerr << "[TP-GEN] commitment_after: ";
        for (size_t b = 0; b < std::min(c.size(), size_t(16)); b++)
            std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)c[b];
        std::cerr << std::dec << std::endl;
    }

    return tp;
}

bool UtreexoTransitionProof::verify() const {
    // Step 0: Self-consistency check
    if (num_leaves_after != num_leaves_before + addition_hashes.size()) {
        std::cerr << "[TP-VER] FAIL step 0: num_leaves_after=" << num_leaves_after
                  << " != before(" << num_leaves_before << ") + adds(" << addition_hashes.size() << ")" << std::endl;
        return false;
    }

    // Step 1: Reconstruct stump_before from roots_before (already height-indexed)
    UtreexoStump stump_before = UtreexoStump::fromRoots(roots_before, num_leaves_before);

    bool has_deletions = !deletion_targets.empty();

    // Step 2: Batch proof verification (deletion targets exist in roots_before)
    // FATAL: If deletion proof fails, the transition proof is invalid.
    // The proof must demonstrate that all deleted UTXOs existed in the
    // pre-deletion forest state. Without this, roots_after_deletions is untrusted.
    if (!deletion_targets.empty()) {
        BlockUtreexoProof proof;
        proof.targets = deletion_targets;
        proof.positions = deletion_positions;
        proof.proof_hashes = deletion_proof_hashes;
        proof.numLeaves = num_leaves_before;

        if (!stump_before.verifyBlockProof(proof)) {
            std::cerr << "[TP-VER] FAIL step 2: batch deletion proof failed" << std::endl;
            return false;
        }
        std::cerr << "[TP-VER] Step 2 passed (deletion proof OK, " << deletion_targets.size() << " targets)" << std::endl;
    }

    // Step 3: Reconstruct stump_mid from roots_after_deletions (already height-indexed)
    // numLeaves is unchanged by deletions in Utreexo (positions marked deleted, not reclaimed)
    UtreexoStump stump_mid = UtreexoStump::fromRoots(roots_after_deletions, num_leaves_before);

    // DIAGNOSTIC: dump stump_mid roots before additions (only for blocks with deletions)
    if (has_deletions) {
        std::cerr << "[TP-VER] stump_mid roots_after_del (numLeaves=" << stump_mid.getNumLeaves() << "):" << std::endl;
        auto all_roots = stump_mid.getAllRoots();
        for (size_t h = 0; h < all_roots.size(); h++) {
            if (all_roots[h].has_value()) {
                const auto& r = all_roots[h].value();
                std::cerr << "[TP-VER]   root[" << h << "]: ";
                for (size_t b = 0; b < std::min(r.size(), size_t(8)); b++)
                    std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)r[b];
                std::cerr << std::dec << "..." << std::endl;
            }
        }
    }

    // Step 4: Apply additions
    for (const auto& leaf : addition_hashes) {
        stump_mid.addSingle(leaf);
    }

    // DIAGNOSTIC: dump stump_mid roots after additions (only for blocks with deletions)
    if (has_deletions) {
        std::cerr << "[TP-VER] stump_mid roots after additions (numLeaves=" << stump_mid.getNumLeaves() << "):" << std::endl;
        auto all_roots = stump_mid.getAllRoots();
        for (size_t h = 0; h < all_roots.size(); h++) {
            if (all_roots[h].has_value()) {
                const auto& r = all_roots[h].value();
                std::cerr << "[TP-VER]   root[" << h << "]: ";
                for (size_t b = 0; b < std::min(r.size(), size_t(8)); b++)
                    std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)r[b];
                std::cerr << std::dec << "..." << std::endl;
            }
        }

        // Print computed commitment
        auto computed = stump_mid.getCommitment();
        std::cerr << "[TP-VER] stump computed commitment: ";
        for (size_t b = 0; b < std::min(computed.size(), size_t(16)); b++)
            std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)computed[b];
        std::cerr << std::dec << std::endl;

        std::cerr << "[TP-VER] expected commitment:       ";
        for (size_t b = 0; b < std::min(commitment_after.size(), size_t(16)); b++)
            std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)commitment_after[b];
        std::cerr << std::dec << std::endl;
    }

    // Step 5: Check commitment matches expected
    if (!stump_mid.verifyCommitment(commitment_after)) {
        std::cerr << "[TP-VER] FAIL step 5: commitment mismatch" << std::endl;
        return false;
    }

    // Step 6: Check numLeaves
    if (stump_mid.getNumLeaves() != num_leaves_after) {
        std::cerr << "[TP-VER] FAIL step 6: numLeaves mismatch: stump="
                  << stump_mid.getNumLeaves() << " expected=" << num_leaves_after << std::endl;
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Serialization
// ─────────────────────────────────────────────────────────────────────────

static void writeLE64(std::vector<uint8_t>& out, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

static void writeLE32(std::vector<uint8_t>& out, uint32_t val) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

static uint64_t readLE64(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t val = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i) {
        val |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    return val;
}

static uint32_t readLE32(const std::vector<uint8_t>& data, size_t& offset) {
    uint32_t val = 0;
    for (int i = 0; i < 4 && offset < data.size(); ++i) {
        val |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    return val;
}

static void writeHash(std::vector<uint8_t>& out, const UtreexoHash& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

static UtreexoHash readHash(const std::vector<uint8_t>& data, size_t& offset) {
    UtreexoHash hash;
    if (offset + 32 <= data.size()) {
        hash.assign(data.begin() + offset, data.begin() + offset + 32);
        offset += 32;
    }
    return hash;
}

// Indexed roots: count(4) + for each: flag(1) + hash(32 if present)
static void writeIndexedRoots(std::vector<uint8_t>& out,
                               const std::vector<std::optional<UtreexoHash>>& roots) {
    writeLE32(out, static_cast<uint32_t>(roots.size()));
    for (const auto& r : roots) {
        if (r.has_value()) {
            out.push_back(1);
            writeHash(out, r.value());
        } else {
            out.push_back(0);
        }
    }
}

static std::vector<std::optional<UtreexoHash>> readIndexedRoots(
    const std::vector<uint8_t>& data, size_t& offset) {
    std::vector<std::optional<UtreexoHash>> roots;
    uint32_t count = readLE32(data, offset);
    for (uint32_t i = 0; i < count && offset < data.size(); ++i) {
        uint8_t flag = data[offset++];
        if (flag) {
            roots.push_back(readHash(data, offset));
        } else {
            roots.push_back(std::nullopt);
        }
    }
    return roots;
}

std::vector<uint8_t> UtreexoTransitionProof::serialize() const {
    std::vector<uint8_t> data;

    // Version
    data.push_back(1);

    // Leaf counts
    writeLE64(data, num_leaves_before);
    writeLE64(data, num_leaves_after);

    // roots_before (height-indexed)
    writeIndexedRoots(data, roots_before);

    // Deletion targets
    writeLE32(data, static_cast<uint32_t>(deletion_targets.size()));
    for (const auto& t : deletion_targets) writeHash(data, t);

    // Deletion positions
    for (uint64_t pos : deletion_positions) writeLE64(data, pos);

    // Deletion proof hashes
    writeLE32(data, static_cast<uint32_t>(deletion_proof_hashes.size()));
    for (const auto& h : deletion_proof_hashes) writeHash(data, h);

    // roots_after_deletions (height-indexed)
    writeIndexedRoots(data, roots_after_deletions);

    // addition_hashes
    writeLE32(data, static_cast<uint32_t>(addition_hashes.size()));
    for (const auto& a : addition_hashes) writeHash(data, a);

    // commitment_after
    writeHash(data, commitment_after);

    return data;
}

UtreexoTransitionProof UtreexoTransitionProof::deserialize(const std::vector<uint8_t>& data) {
    UtreexoTransitionProof tp;
    size_t offset = 0;

    if (data.empty()) return tp;

    // Version
    uint8_t version = data[offset++];
    if (version != 1) return tp;

    // Leaf counts
    tp.num_leaves_before = readLE64(data, offset);
    tp.num_leaves_after = readLE64(data, offset);

    // roots_before (height-indexed)
    tp.roots_before = readIndexedRoots(data, offset);

    // Deletion targets
    uint32_t numTargets = readLE32(data, offset);
    for (uint32_t i = 0; i < numTargets; ++i) {
        tp.deletion_targets.push_back(readHash(data, offset));
    }

    // Deletion positions
    for (uint32_t i = 0; i < numTargets; ++i) {
        tp.deletion_positions.push_back(readLE64(data, offset));
    }

    // Deletion proof hashes
    uint32_t numProofHashes = readLE32(data, offset);
    for (uint32_t i = 0; i < numProofHashes; ++i) {
        tp.deletion_proof_hashes.push_back(readHash(data, offset));
    }

    // roots_after_deletions (height-indexed)
    tp.roots_after_deletions = readIndexedRoots(data, offset);

    // addition_hashes
    uint32_t numAdditions = readLE32(data, offset);
    for (uint32_t i = 0; i < numAdditions; ++i) {
        tp.addition_hashes.push_back(readHash(data, offset));
    }

    // commitment_after
    tp.commitment_after = readHash(data, offset);

    return tp;
}

static size_t indexedRootsSize(const std::vector<std::optional<UtreexoHash>>& roots) {
    size_t s = 4; // count
    for (const auto& r : roots) {
        s += 1; // flag byte
        if (r.has_value()) s += 32; // hash
    }
    return s;
}

size_t UtreexoTransitionProof::serializedSize() const {
    return 1 + 8 + 8 +
           indexedRootsSize(roots_before) +
           4 + (deletion_targets.size() * 32) +
           (deletion_positions.size() * 8) +
           4 + (deletion_proof_hashes.size() * 32) +
           indexedRootsSize(roots_after_deletions) +
           4 + (addition_hashes.size() * 32) +
           32;
}

bool UtreexoTransitionProof::isEmpty() const {
    return roots_before.empty() && deletion_targets.empty() &&
           addition_hashes.empty() && commitment_after.empty();
}

} // namespace consensus
} // namespace dinero
