/**
 * Phase N.3: Header Serialization Utilities
 *
 * Dinero uses 128-byte BlockHeader v1 for both internal format and P2P wire format.
 *
 * BlockHeader v1 Layout (128 bytes):
 *   [0-3]:     version (4 bytes)
 *   [4-35]:    prev_block_hash (32 bytes)
 *   [36-67]:   merkle_root (32 bytes)
 *   [68-99]:   utreexo_root (32 bytes)
 *   [100-107]: timestamp (8 bytes)
 *   [108-111]: difficulty (4 bytes)
 *   [112-115]: nonce (4 bytes)
 *   [116-127]: reserved (12 bytes, MUST be zero)
 */

#include "daemon/header_serialization.h"
#include <cstring>

namespace dinero {

// hexToBytes and bytesToHex are now inline in header_serialization.h

/**
 * Serialize BlockHeader to 128-byte Dinero wire format (BlockHeader v1).
 *
 * Wire format: 128 bytes (CONSENSUS-FINAL)
 *
 * Structure (little-endian):
 *   [0-3]:     version (4 bytes)
 *   [4-35]:    prev_block_hash (32 bytes)
 *   [36-67]:   merkle_root (32 bytes)
 *   [68-99]:   utreexo_root (32 bytes)
 *   [100-107]: timestamp (8 bytes)
 *   [108-111]: difficulty (4 bytes)
 *   [112-115]: nonce (4 bytes)
 *   [116-127]: reserved (12 bytes, MUST be zero)
 */
std::vector<uint8_t> serializeHeaderForWire(const BlockHeader& header) {
    std::vector<uint8_t> data;
    data.reserve(128);

    // [0-3]: version (4 bytes, little-endian)
    uint32_t version = header.version;
    data.push_back(version & 0xFF);
    data.push_back((version >> 8) & 0xFF);
    data.push_back((version >> 16) & 0xFF);
    data.push_back((version >> 24) & 0xFF);

    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: Copy raw bytes directly (NO hex round-trip)
    // ═══════════════════════════════════════════════════════════════════════════
    // GetHex() returns big-endian display order (reversed from internal storage).
    // SerializeForHash() copies raw uint256.data bytes directly.
    // Wire format MUST match consensus format (SerializeForHash) for header sync
    // to work correctly. Using GetHex() → hexToBytes() would produce reversed
    // bytes, causing headers from P2P to never match full block headers.
    // ═══════════════════════════════════════════════════════════════════════════

    // [4-35]: prev_block_hash (32 bytes) - raw bytes, same as SerializeForHash
    data.insert(data.end(), header.prev_block_hash.begin(), header.prev_block_hash.end());

    // [36-67]: merkle_root (32 bytes) - raw bytes, same as SerializeForHash
    data.insert(data.end(), header.merkle_root.begin(), header.merkle_root.end());

    // [68-99]: utreexo_root (32 bytes) - raw bytes, same as SerializeForHash
    data.insert(data.end(), header.utreexo_root.begin(), header.utreexo_root.end());

    // [100-107]: timestamp (8 bytes, little-endian)
    uint64_t timestamp = header.timestamp;
    data.push_back(timestamp & 0xFF);
    data.push_back((timestamp >> 8) & 0xFF);
    data.push_back((timestamp >> 16) & 0xFF);
    data.push_back((timestamp >> 24) & 0xFF);
    data.push_back((timestamp >> 32) & 0xFF);
    data.push_back((timestamp >> 40) & 0xFF);
    data.push_back((timestamp >> 48) & 0xFF);
    data.push_back((timestamp >> 56) & 0xFF);

    // [108-111]: difficulty (4 bytes, little-endian)
    uint32_t difficulty = header.difficulty;
    data.push_back(difficulty & 0xFF);
    data.push_back((difficulty >> 8) & 0xFF);
    data.push_back((difficulty >> 16) & 0xFF);
    data.push_back((difficulty >> 24) & 0xFF);

    // [112-115]: nonce (4 bytes, little-endian)
    uint32_t nonce = header.nonce;
    data.push_back(nonce & 0xFF);
    data.push_back((nonce >> 8) & 0xFF);
    data.push_back((nonce >> 16) & 0xFF);
    data.push_back((nonce >> 24) & 0xFF);

    // [116-127]: reserved (12 bytes, MUST be zero)
    for (int i = 0; i < 12; i++) {
        data.push_back(0);
    }

    // Verify final size
    if (data.size() != 128) {
        throw std::runtime_error("Serialized header size incorrect: " + std::to_string(data.size()) + " (expected 128)");
    }

    return data;
}

/**
 * Deserialize 128-byte Dinero wire format (BlockHeader v1) to BlockHeader.
 */
BlockHeader deserializeHeaderFromWire(const std::vector<uint8_t>& data) {
    if (data.size() != 128) {
        throw std::runtime_error("Invalid header data size: " + std::to_string(data.size()) + " (expected 128)");
    }

    BlockHeader header;

    // [0-3]: version
    header.version = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: Copy raw bytes directly (NO hex round-trip)
    // ═══════════════════════════════════════════════════════════════════════════
    // bytesToHex() → FromHexUnsafe() would reverse bytes (FromHexUnsafe expects
    // big-endian hex and stores in little-endian). Wire format uses raw bytes
    // (same as SerializeForHash), so we must copy directly to preserve byte order.
    // ═══════════════════════════════════════════════════════════════════════════

    // [4-35]: prev_block_hash (32 bytes) - raw bytes, same as SerializeForHash
    std::memcpy(header.prev_block_hash.data, data.data() + 4, 32);

    // [36-67]: merkle_root (32 bytes) - raw bytes, same as SerializeForHash
    std::memcpy(header.merkle_root.data, data.data() + 36, 32);

    // [68-99]: utreexo_root (32 bytes) - raw bytes, same as SerializeForHash
    std::memcpy(header.utreexo_root.data, data.data() + 68, 32);

    // [100-107]: timestamp (8 bytes, little-endian)
    header.timestamp = static_cast<uint64_t>(data[100]) |
                       (static_cast<uint64_t>(data[101]) << 8) |
                       (static_cast<uint64_t>(data[102]) << 16) |
                       (static_cast<uint64_t>(data[103]) << 24) |
                       (static_cast<uint64_t>(data[104]) << 32) |
                       (static_cast<uint64_t>(data[105]) << 40) |
                       (static_cast<uint64_t>(data[106]) << 48) |
                       (static_cast<uint64_t>(data[107]) << 56);

    // [108-111]: difficulty
    header.difficulty = data[108] | (data[109] << 8) | (data[110] << 16) | (data[111] << 24);

    // [112-115]: nonce
    header.nonce = data[112] | (data[113] << 8) | (data[114] << 16) | (data[115] << 24);

    // [116-127]: reserved (12 bytes, ignored on read but should be zero)

    return header;
}

} // namespace dinero
