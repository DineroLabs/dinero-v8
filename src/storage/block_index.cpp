#include "storage/block_index.h"
#include <cstring>
#include <stdexcept>
#include <iomanip>
#include <sstream>

namespace dinero {

// ============================================================================
// Helper Functions
// ============================================================================

// Convert hex string to binary bytes (32 bytes)
static void hexToBytes(const std::string& hex, uint8_t* out) {
    if (hex.length() != 64) {
        // If invalid, write zeros
        std::memset(out, 0, 32);
        return;
    }

    for (size_t i = 0; i < 32; i++) {
        std::string byteStr = hex.substr(i * 2, 2);
        out[i] = static_cast<uint8_t>(std::strtol(byteStr.c_str(), nullptr, 16));
    }
}

// Convert binary bytes to hex string
static std::string bytesToHex(const uint8_t* bytes, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

// ============================================================================
// Serialization Format (Binary) - Dinero 128-byte Header Support (BlockHeader v1)
// ============================================================================
//
// BlockIndex is serialized to a compact binary format for storage in ChainDB.
// This is the persistent representation stored in RocksDB.
//
// Format (total: 200 bytes fixed size - includes Utreexo root):
//   [hash:32]
//   [hash_prev:32]
//   [height:4]
//   [file_pos.file_number:4]
//   [file_pos.offset:8]
//   [file_pos.size:4]
//   [version:4]
//   [merkle_root:32]
//   [time:4]
//   [bits:4]
//   [nonce:4]
//   [utreexo_root:32]  // NEW - Dinero native Utreexo commitment
//   [chain_work:32] (arith_uint256 as 32 bytes)
//   [tx_count:4]
//   [status:4]
//
// Note: pprev/pnext/pskip pointers are NOT serialized (runtime-only)
// Note: sequence_id is NOT serialized (runtime-only)
//
// ============================================================================

std::string BlockIndex::serialize() const {
    std::string data;
    data.resize(200); // Total size: 168 + 32 (utreexo_root) = 200 bytes

    uint8_t* ptr = reinterpret_cast<uint8_t*>(&data[0]);
    size_t offset = 0;

    // hash (32 bytes) - copy raw bytes from uint256
    std::memcpy(ptr + offset, hash.data, 32);
    offset += 32;

    // hash_prev (32 bytes) - copy raw bytes from uint256
    std::memcpy(ptr + offset, hash_prev.data, 32);
    offset += 32;

    // height (4 bytes, little-endian)
    int32_t h = height;
    std::memcpy(ptr + offset, &h, 4);
    offset += 4;

    // file_pos.file_number (4 bytes, little-endian)
    uint32_t fn = file_pos.file_number;
    std::memcpy(ptr + offset, &fn, 4);
    offset += 4;

    // file_pos.offset (8 bytes, little-endian)
    uint64_t fo = file_pos.offset;
    std::memcpy(ptr + offset, &fo, 8);
    offset += 8;

    // file_pos.size (4 bytes, little-endian)
    uint32_t fs = file_pos.size;
    std::memcpy(ptr + offset, &fs, 4);
    offset += 4;

    // version (4 bytes, little-endian)
    std::memcpy(ptr + offset, &version, 4);
    offset += 4;

    // merkle_root (32 bytes) - copy raw bytes from uint256
    std::memcpy(ptr + offset, merkle_root.data, 32);
    offset += 32;

    // time (4 bytes, little-endian)
    std::memcpy(ptr + offset, &time, 4);
    offset += 4;

    // bits (4 bytes, little-endian)
    std::memcpy(ptr + offset, &bits, 4);
    offset += 4;

    // nonce (4 bytes, little-endian)
    std::memcpy(ptr + offset, &nonce, 4);
    offset += 4;

    // utreexo_root (32 bytes) - Dinero native Utreexo commitment
    std::memcpy(ptr + offset, utreexo_root.data, 32);
    offset += 32;

    // chain_work (32 bytes) - serialize arith_uint256 as 4 uint64_t words (little-endian)
    for (int i = 0; i < 4; i++) {
        uint64_t word = chain_work.GetWord(i);
        std::memcpy(ptr + offset, &word, 8);
        offset += 8;
    }

    // tx_count (4 bytes, little-endian)
    std::memcpy(ptr + offset, &tx_count, 4);
    offset += 4;

    // status (4 bytes, little-endian)
    std::memcpy(ptr + offset, &status, 4);
    offset += 4;

    return data;
}

BlockIndex BlockIndex::deserialize(const std::string& data) {
    // Support both old (168 bytes) and new (200 bytes) formats for migration
    if (data.size() != 200 && data.size() != 168) {
        throw std::runtime_error("Invalid BlockIndex serialization size: expected 200 bytes (or 168 for legacy), got " +
                                 std::to_string(data.size()));
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;

    BlockIndex index;

    // hash (32 bytes) - copy raw bytes to uint256
    std::memcpy(index.hash.data, ptr + offset, 32);
    offset += 32;

    // hash_prev (32 bytes) - copy raw bytes to uint256
    std::memcpy(index.hash_prev.data, ptr + offset, 32);
    offset += 32;

    // height (4 bytes, little-endian)
    std::memcpy(&index.height, ptr + offset, 4);
    offset += 4;

    // file_pos.file_number (4 bytes, little-endian)
    uint32_t file_number;
    std::memcpy(&file_number, ptr + offset, 4);
    offset += 4;

    // file_pos.offset (8 bytes, little-endian)
    uint64_t file_offset;
    std::memcpy(&file_offset, ptr + offset, 8);
    offset += 8;

    // file_pos.size (4 bytes, little-endian)
    uint32_t file_size;
    std::memcpy(&file_size, ptr + offset, 4);
    offset += 4;

    index.file_pos = FilePosition(file_number, file_offset, file_size);

    // version (4 bytes, little-endian)
    std::memcpy(&index.version, ptr + offset, 4);
    offset += 4;

    // merkle_root (32 bytes) - copy raw bytes to uint256
    std::memcpy(index.merkle_root.data, ptr + offset, 32);
    offset += 32;

    // time (4 bytes, little-endian)
    std::memcpy(&index.time, ptr + offset, 4);
    offset += 4;

    // bits (4 bytes, little-endian)
    std::memcpy(&index.bits, ptr + offset, 4);
    offset += 4;

    // nonce (4 bytes, little-endian)
    std::memcpy(&index.nonce, ptr + offset, 4);
    offset += 4;

    // utreexo_root (32 bytes) - Dinero native Utreexo commitment (NEW)
    if (data.size() == 200) {
        std::memcpy(index.utreexo_root.data, ptr + offset, 32);
        offset += 32;
    }
    // Legacy 168-byte format: utreexo_root remains zero (default)

    // chain_work (32 bytes) - deserialize arith_uint256 from 4 uint64_t words (little-endian)
    for (int i = 0; i < 4; i++) {
        uint64_t word;
        std::memcpy(&word, ptr + offset, 8);
        index.chain_work.SetWord(i, word);
        offset += 8;
    }

    // tx_count (4 bytes, little-endian)
    std::memcpy(&index.tx_count, ptr + offset, 4);
    offset += 4;

    // status (4 bytes, little-endian)
    std::memcpy(&index.status, ptr + offset, 4);
    offset += 4;

    // Migrate legacy flag positions: old BLOCK_HAVE_DATA=8 → 128, BLOCK_HAVE_UNDO=16 → 256
    // Old format used bits 3-4, new format uses bits 7-8 (matching consensus/block_lifecycle.h)
    constexpr uint32_t OLD_BLOCK_HAVE_DATA = 8;
    constexpr uint32_t OLD_BLOCK_HAVE_UNDO = 16;
    if ((index.status & OLD_BLOCK_HAVE_DATA) && !(index.status & BlockIndex::BLOCK_HAVE_DATA)) {
        index.status = (index.status & ~OLD_BLOCK_HAVE_DATA) | BlockIndex::BLOCK_HAVE_DATA;
    }
    if ((index.status & OLD_BLOCK_HAVE_UNDO) && !(index.status & BlockIndex::BLOCK_HAVE_UNDO)) {
        index.status = (index.status & ~OLD_BLOCK_HAVE_UNDO) | BlockIndex::BLOCK_HAVE_UNDO;
    }

    // Runtime fields (pprev, pnext, pskip, sequence_id) remain at default values
    // These will be reconstructed when loading the BlockIndex into memory

    return index;
}

// ============================================================================
// F.7.2: Pruning Safety Check Implementation
// ============================================================================

bool BlockIndex::isPrunable(const BlockIndex* active_tip, const class BlockStorage* block_storage) const {
    // Invariant 1: Block must be older than MIN_UNDO_DEPTH from active tip
    // This ensures we keep enough undo data for deep reorgs
    if (!active_tip) {
        return false;  // Cannot determine depth without active tip
    }

    int depth_from_tip = active_tip->height - this->height;
    if (depth_from_tip < MIN_UNDO_DEPTH) {
        return false;  // Too close to tip, unsafe to prune
    }

    // Invariant 2: Block must NOT have any descendants on the active chain
    // Walk forward from this block to see if we reach the active tip
    // If we can reach active_tip through pnext links, then this block is an ancestor
    const BlockIndex* walker = this;
    while (walker && walker->pnext) {
        walker = walker->pnext;
        if (walker == active_tip) {
            return false;  // This block is on the active chain, cannot prune
        }
    }

    // Invariant 3: Block must have BLOCK_HAVE_UNDO flag set
    // This ensures metadata consistency
    if (!(this->status & BLOCK_HAVE_UNDO)) {
        return false;  // No undo flag, unsafe to prune
    }

    // Invariant 4: Undo data must actually be available on disk
    // This is the final safety check - verify rev*.dat file is readable
    if (!block_storage) {
        return false;  // Cannot check undo availability without BlockStorage
    }

    // Check if undo file position is valid (non-null)
    if (this->undo_pos.isNull()) {
        return false;  // No undo position recorded
    }

    // Verify undo data is actually readable from disk
    // hasBlock() checks if the file exists and position is valid
    auto undo_status = block_storage->hasBlock(this->undo_pos);
    if (undo_status != Status::Ok) {
        return false;  // Undo data not available on disk
    }

    // All invariants satisfied - block is safe to prune
    return true;
}

} // namespace dinero
