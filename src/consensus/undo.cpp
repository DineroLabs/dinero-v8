// Undo data serialization/deserialization for block disconnection
#include "consensus/undo.h"
#include <cstring>
#include <stdexcept>

namespace dinero {

namespace {

constexpr uint32_t kUndoConfidentialFormatFlag = 0x80000000u;

}  // namespace

// Binary serialization format:
// - num_spent (4 bytes)
// - for each SpentCoin:
//   - prev_txid length (4 bytes) + prev_txid data
//   - prev_vout (4 bytes)
//   - value (8 bytes)
//   - scriptPubKey length (4 bytes) + scriptPubKey data
//   - is_coinbase (1 byte)
//   - height (4 bytes)
//   - [v2] is_confidential (1 byte)
//   - [v2] commitment length (4 bytes) + commitment bytes
// - num_created (4 bytes)
// - for each CreatedOut:
//   - txid length (4 bytes) + txid data
//   - vout (4 bytes)
// - [v3 optional trailer] has_pre_block_shielded_frontier (1 byte)
// - [v3 optional trailer] frontier length (4 bytes) + frontier bytes
//
// Backward compatibility:
// - Older undo blobs end immediately after created outputs.
// - Newer readers treat trailing bytes as the optional shielded frontier
//   snapshot needed by flatfile-backed shielded disconnect paths.

std::vector<uint8_t> UndoRecord::Serialize() const {
    std::vector<uint8_t> result;

    // Helper lambda to write uint32_t
    auto write_u32 = [&](uint32_t val) {
        result.push_back((val >> 0) & 0xFF);
        result.push_back((val >> 8) & 0xFF);
        result.push_back((val >> 16) & 0xFF);
        result.push_back((val >> 24) & 0xFF);
    };

    // Helper lambda to write uint64_t
    auto write_u64 = [&](uint64_t val) {
        result.push_back((val >> 0) & 0xFF);
        result.push_back((val >> 8) & 0xFF);
        result.push_back((val >> 16) & 0xFF);
        result.push_back((val >> 24) & 0xFF);
        result.push_back((val >> 32) & 0xFF);
        result.push_back((val >> 40) & 0xFF);
        result.push_back((val >> 48) & 0xFF);
        result.push_back((val >> 56) & 0xFF);
    };

    // Helper lambda to write uint256 (32 bytes)
    auto write_uint256 = [&](const uint256& hash) {
        result.insert(result.end(), hash.data, hash.data + 32);
    };

    // Helper lambda to write bytes vector
    auto write_bytes = [&](const std::vector<uint8_t>& bytes) {
        write_u32(static_cast<uint32_t>(bytes.size()));
        result.insert(result.end(), bytes.begin(), bytes.end());
    };

    // Write spent coins
    write_u32(static_cast<uint32_t>(spent.size()) | kUndoConfidentialFormatFlag);
    for (const auto& coin : spent) {
        write_uint256(coin.prev_txid);  // Phase M.0: 32 bytes, not string
        write_u32(coin.prev_vout);
        write_u64(coin.value);
        write_bytes(coin.scriptPubKey);  // Binary, not hex
        result.push_back(coin.is_coinbase ? 1 : 0);
        write_u32(coin.height);
        result.push_back(coin.is_confidential ? 1 : 0);
        write_bytes(coin.commitment);
    }

    // Write created outputs
    write_u32(static_cast<uint32_t>(created.size()));
    for (const auto& out : created) {
        write_uint256(out.txid);  // Phase M.0: 32 bytes, not string
        write_u32(out.vout);
    }

    result.push_back(pre_block_shielded_frontier.has_value() ? 1 : 0);
    if (pre_block_shielded_frontier.has_value()) {
        write_bytes(*pre_block_shielded_frontier);
    }

    // [v4 optional trailer] shielded epoch reset snapshot (reset block only).
    // Appended after the frontier trailer so older readers that stop after the
    // frontier are unaffected, and older blobs deserialize with this nullopt.
    result.push_back(pre_reset_shielded_epoch.has_value() ? 1 : 0);
    if (pre_reset_shielded_epoch.has_value()) {
        write_bytes(pre_reset_shielded_epoch->tree_frontier);
        write_bytes(pre_reset_shielded_epoch->anchor_history);
        write_bytes(pre_reset_shielded_epoch->nullifiers);
    }

    return result;
}

UndoRecord UndoRecord::Deserialize(const std::vector<uint8_t>& data) {
    UndoRecord result;
    size_t offset = 0;

    // Helper lambda to read uint32_t
    auto read_u32 = [&]() -> uint32_t {
        if (offset + 4 > data.size()) {
            throw std::runtime_error("Undo data truncated (reading u32)");
        }
        uint32_t val = (static_cast<uint32_t>(data[offset + 0]) << 0) |
                       (static_cast<uint32_t>(data[offset + 1]) << 8) |
                       (static_cast<uint32_t>(data[offset + 2]) << 16) |
                       (static_cast<uint32_t>(data[offset + 3]) << 24);
        offset += 4;
        return val;
    };

    // Helper lambda to read uint64_t
    auto read_u64 = [&]() -> uint64_t {
        if (offset + 8 > data.size()) {
            throw std::runtime_error("Undo data truncated (reading u64)");
        }
        uint64_t val = (static_cast<uint64_t>(data[offset + 0]) << 0) |
                       (static_cast<uint64_t>(data[offset + 1]) << 8) |
                       (static_cast<uint64_t>(data[offset + 2]) << 16) |
                       (static_cast<uint64_t>(data[offset + 3]) << 24) |
                       (static_cast<uint64_t>(data[offset + 4]) << 32) |
                       (static_cast<uint64_t>(data[offset + 5]) << 40) |
                       (static_cast<uint64_t>(data[offset + 6]) << 48) |
                       (static_cast<uint64_t>(data[offset + 7]) << 56);
        offset += 8;
        return val;
    };

    // Helper lambda to read uint256 (32 bytes)
    auto read_uint256 = [&]() -> uint256 {
        if (offset + 32 > data.size()) {
            throw std::runtime_error("Undo data truncated (reading uint256)");
        }
        uint256 hash;
        std::memcpy(hash.data, data.data() + offset, 32);
        offset += 32;
        return hash;
    };

    // Helper lambda to read bytes vector
    auto read_bytes = [&]() -> std::vector<uint8_t> {
        uint32_t len = read_u32();
        if (offset + len > data.size()) {
            throw std::runtime_error("Undo data truncated (reading bytes)");
        }
        std::vector<uint8_t> bytes(data.begin() + offset, data.begin() + offset + len);
        offset += len;
        return bytes;
    };

    // Read spent coins
    uint32_t num_spent_raw = read_u32();
    bool has_confidential_metadata = (num_spent_raw & kUndoConfidentialFormatFlag) != 0;
    uint32_t num_spent = num_spent_raw & ~kUndoConfidentialFormatFlag;
    result.spent.reserve(num_spent);
    for (uint32_t i = 0; i < num_spent; i++) {
        SpentCoin coin;
        coin.prev_txid = read_uint256();  // Phase M.0: 32 bytes, not string
        coin.prev_vout = read_u32();
        coin.value = read_u64();
        coin.scriptPubKey = read_bytes();  // Binary, not hex

        if (offset >= data.size()) {
            throw std::runtime_error("Undo data truncated (reading is_coinbase)");
        }
        coin.is_coinbase = (data[offset++] != 0);

        coin.height = read_u32();
        if (has_confidential_metadata) {
            if (offset >= data.size()) {
                throw std::runtime_error("Undo data truncated (reading is_confidential)");
            }
            coin.is_confidential = (data[offset++] != 0);
            coin.commitment = read_bytes();
        }

        result.spent.push_back(coin);
    }

    // Read created outputs
    uint32_t num_created = read_u32();
    result.created.reserve(num_created);
    for (uint32_t i = 0; i < num_created; i++) {
        CreatedOut out;
        out.txid = read_uint256();  // Phase M.0: 32 bytes, not string
        out.vout = read_u32();
        result.created.push_back(out);
    }

    if (offset < data.size()) {
        const bool has_pre_block_shielded_frontier = (data[offset++] != 0);
        if (has_pre_block_shielded_frontier) {
            result.pre_block_shielded_frontier = read_bytes();
        }
    }

    if (offset < data.size()) {
        const bool has_reset_epoch = (data[offset++] != 0);
        if (has_reset_epoch) {
            consensus::shielded::ShieldedEpochSnapshot snap;
            snap.tree_frontier  = read_bytes();
            snap.anchor_history = read_bytes();
            snap.nullifiers     = read_bytes();
            result.pre_reset_shielded_epoch = std::move(snap);
        }
    }

    return result;
}

} // namespace dinero
