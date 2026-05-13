#include "primitives/block.h"
#include "consensus/limits.h"  // MAX_BLOCK_WEIGHT — explicit size gate
#include "common/sha256d.h"
#include "crypto/sha256.h"  // Phase 3: Use CSHA256 for block hashing
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dinero {

namespace {

inline uint8_t HexByte(char hi, char lo) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
        throw std::runtime_error("Invalid hex character");
    };
    return static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo));
}

inline std::string NormalizeHash(const std::string& hex, const char* field, bool allow_empty_zero = false) {
    if (hex.empty() && allow_empty_zero) {
        return std::string(64, '0');
    }
    if (hex.size() != 64) {
        throw std::runtime_error(std::string(field) + " must be 64 hex characters");
    }
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            throw std::runtime_error(std::string(field) + " contains non-hex characters");
        }
    }
    return hex;
}

inline void WriteLE32(uint8_t* data, size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>(value & 0xff);
    data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

inline void WriteHashLE(uint8_t* data, size_t offset, const std::string& hex) {
    // Write hash bytes directly from LE hex string (no reversal)
    // Input hex is in LE format (native internal format)
    for (size_t i = 0; i < 32; ++i) {
        const size_t pos = i * 2;
        data[offset + i] = HexByte(hex[pos], hex[pos + 1]);
    }
}

} // namespace

std::array<uint8_t, 128> BlockHeader::SerializeForHash() const {
    std::array<uint8_t, 128> out{};
    uint8_t* data = out.data();

    // BlockHeader v1 (128 bytes): Clean serialization with no legacy compatibility
    // Layout:
    //   0x00 (4 bytes):  version (LE)
    //   0x04 (32 bytes): prev_block_hash (LE)
    //   0x24 (32 bytes): merkle_root (LE)
    //   0x44 (32 bytes): utreexo_root (LE)
    //   0x64 (8 bytes):  timestamp (LE)
    //   0x6C (4 bytes):  difficulty (LE)
    //   0x70 (4 bytes):  nonce (LE)
    //   0x74 (12 bytes): reserved (MUST be zero)

    // version (4 bytes, offset 0x00)
    data[0x00] = static_cast<uint8_t>(version & 0xff);
    data[0x01] = static_cast<uint8_t>((version >> 8) & 0xff);
    data[0x02] = static_cast<uint8_t>((version >> 16) & 0xff);
    data[0x03] = static_cast<uint8_t>((version >> 24) & 0xff);

    // prev_block_hash (32 bytes, offset 0x04)
    std::memcpy(data + 0x04, prev_block_hash.data, 32);

    // merkle_root (32 bytes, offset 0x24)
    std::memcpy(data + 0x24, merkle_root.data, 32);

    // utreexo_root (32 bytes, offset 0x44)
    std::memcpy(data + 0x44, utreexo_root.data, 32);

    // timestamp (8 bytes, offset 0x64)
    data[0x64] = static_cast<uint8_t>(timestamp & 0xff);
    data[0x65] = static_cast<uint8_t>((timestamp >> 8) & 0xff);
    data[0x66] = static_cast<uint8_t>((timestamp >> 16) & 0xff);
    data[0x67] = static_cast<uint8_t>((timestamp >> 24) & 0xff);
    data[0x68] = static_cast<uint8_t>((timestamp >> 32) & 0xff);
    data[0x69] = static_cast<uint8_t>((timestamp >> 40) & 0xff);
    data[0x6A] = static_cast<uint8_t>((timestamp >> 48) & 0xff);
    data[0x6B] = static_cast<uint8_t>((timestamp >> 56) & 0xff);

    // difficulty (4 bytes, offset 0x6C)
    data[0x6C] = static_cast<uint8_t>(difficulty & 0xff);
    data[0x6D] = static_cast<uint8_t>((difficulty >> 8) & 0xff);
    data[0x6E] = static_cast<uint8_t>((difficulty >> 16) & 0xff);
    data[0x6F] = static_cast<uint8_t>((difficulty >> 24) & 0xff);

    // nonce (4 bytes, offset 0x70)
    data[0x70] = static_cast<uint8_t>(nonce & 0xff);
    data[0x71] = static_cast<uint8_t>((nonce >> 8) & 0xff);
    data[0x72] = static_cast<uint8_t>((nonce >> 16) & 0xff);
    data[0x73] = static_cast<uint8_t>((nonce >> 24) & 0xff);

    // reserved (12 bytes, offset 0x74)
    // Serialize actual bytes — the block hash MUST commit to the reserved field.
    // Consensus rule: reserved must be all zeros (enforced in block validation).
    // If reserved is non-zero, the hash WILL differ, which is correct behavior.
    std::memcpy(data + 0x74, reserved, 12);

    return out;
}

std::string BlockHeader::Serialize() const {
    auto bytes = SerializeForHash();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

uint256 BlockHeader::GetHash() const {
    // Phase 3: Use crypto::CSHA256 to match the genesis miner
    // (Dinero::Common::sha256 outputs big-endian bytes, CSHA256 outputs little-endian)
    auto bytes = SerializeForHash();

    // First SHA256
    uint8_t hash1[32];
    crypto::CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash1);

    // Second SHA256
    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // SHA-256 output is big-endian (MSB at byte 0).
    // uint256 uses LE storage (data[0]=LSB, data[31]=MSB).
    // Reverse bytes so operator< and GetHex() work correctly.
    uint256 result;
    for (int i = 0; i < 32; ++i)
        result.data[i] = hash2[31 - i];
    return result;
}

std::optional<BlockHeader> BlockHeader::Deserialize(const std::vector<uint8_t>& data) {
    return Deserialize(data.data(), data.size());
}

std::optional<BlockHeader> BlockHeader::Deserialize(const uint8_t* data, size_t len) {
    // BlockHeader v1 is exactly 128 bytes
    if (len < 128 || data == nullptr) {
        return std::nullopt;
    }

    BlockHeader header{};

    // version (4 bytes, offset 0x00, LE)
    header.version = static_cast<uint32_t>(data[0x00]) |
                     (static_cast<uint32_t>(data[0x01]) << 8) |
                     (static_cast<uint32_t>(data[0x02]) << 16) |
                     (static_cast<uint32_t>(data[0x03]) << 24);

    // prev_block_hash (32 bytes, offset 0x04)
    std::memcpy(header.prev_block_hash.data, data + 0x04, 32);

    // merkle_root (32 bytes, offset 0x24)
    std::memcpy(header.merkle_root.data, data + 0x24, 32);

    // utreexo_root (32 bytes, offset 0x44)
    std::memcpy(header.utreexo_root.data, data + 0x44, 32);

    // timestamp (8 bytes, offset 0x64, LE)
    header.timestamp = static_cast<uint64_t>(data[0x64]) |
                       (static_cast<uint64_t>(data[0x65]) << 8) |
                       (static_cast<uint64_t>(data[0x66]) << 16) |
                       (static_cast<uint64_t>(data[0x67]) << 24) |
                       (static_cast<uint64_t>(data[0x68]) << 32) |
                       (static_cast<uint64_t>(data[0x69]) << 40) |
                       (static_cast<uint64_t>(data[0x6A]) << 48) |
                       (static_cast<uint64_t>(data[0x6B]) << 56);

    // difficulty (4 bytes, offset 0x6C, LE)
    header.difficulty = static_cast<uint32_t>(data[0x6C]) |
                        (static_cast<uint32_t>(data[0x6D]) << 8) |
                        (static_cast<uint32_t>(data[0x6E]) << 16) |
                        (static_cast<uint32_t>(data[0x6F]) << 24);

    // nonce (4 bytes, offset 0x70, LE)
    header.nonce = static_cast<uint32_t>(data[0x70]) |
                   (static_cast<uint32_t>(data[0x71]) << 8) |
                   (static_cast<uint32_t>(data[0x72]) << 16) |
                   (static_cast<uint32_t>(data[0x73]) << 24);

    // reserved (12 bytes, offset 0x74)
    std::memcpy(header.reserved, data + 0x74, 12);

    return header;
}

std::string Block::Serialize() const {
    // Serialize block: header + varint tx count + transactions
    std::string result;

    // Serialize header
    result += header.Serialize();

    // Serialize transaction count using Bitcoin compact size (varint) encoding
    uint64_t tx_count = vtx.size();
    if (tx_count < 0xfd) {
        result += static_cast<char>(tx_count);
    } else if (tx_count <= 0xffff) {
        result += static_cast<char>(0xfd);
        result += static_cast<char>(tx_count & 0xff);
        result += static_cast<char>((tx_count >> 8) & 0xff);
    } else if (tx_count <= 0xffffffff) {
        result += static_cast<char>(0xfe);
        for (int i = 0; i < 4; i++) {
            result += static_cast<char>((tx_count >> (i * 8)) & 0xff);
        }
    } else {
        result += static_cast<char>(0xff);
        for (int i = 0; i < 8; i++) {
            result += static_cast<char>((tx_count >> (i * 8)) & 0xff);
        }
    }

    // Serialize transactions
    // ✅ CRITICAL FIX: Include witness data for SegWit/Taproot transactions
    // Witness data contains signatures needed for script validation.
    // Without witness, parsed transactions fail signature verification.
    // Phase 11d: Use explicit TxSerializationMode::WithWitness for clarity
    for (const auto& tx : vtx) {
        std::vector<uint8_t> tx_bytes = tx.Serialize(TxSerializationMode::WithWitness);
        result.append(reinterpret_cast<const char*>(tx_bytes.data()), tx_bytes.size());
    }

    // Serialize optional Utreexo data (Phase 1: Proof Data Structures)
    // Format: 1 byte flag + optional BlockUtreexoData
    // 0x00 = no Utreexo data (backward compatibility)
    // 0x01 = has Utreexo data
    if (utreexo.has_value()) {
        result += static_cast<char>(0x01);  // Flag: has Utreexo data
        std::vector<uint8_t> utreexo_bytes = utreexo->serialize();
        result.append(reinterpret_cast<const char*>(utreexo_bytes.data()), utreexo_bytes.size());
    } else {
        result += static_cast<char>(0x00);  // Flag: no Utreexo data
    }

    return result;
}

namespace {

// Read Bitcoin CompactSize (varint) from a raw byte span.
// Returns false on overflow/out-of-bounds.
bool ReadCompactSize(const uint8_t* data, size_t len, size_t& offset, uint64_t& out) {
    if (!data || offset >= len) {
        return false;
    }

    const uint8_t first = data[offset++];
    if (first < 0xfd) {
        out = first;
        return true;
    }

    if (first == 0xfd) {
        if (offset + 2 > len) return false;
        out = static_cast<uint64_t>(data[offset]) |
              (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    }

    if (first == 0xfe) {
        if (offset + 4 > len) return false;
        out = static_cast<uint64_t>(data[offset]) |
              (static_cast<uint64_t>(data[offset + 1]) << 8) |
              (static_cast<uint64_t>(data[offset + 2]) << 16) |
              (static_cast<uint64_t>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    }

    // 0xff
    if (offset + 8 > len) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(data[offset + i]) << (8 * i));
    }
    offset += 8;
    out = v;
    return true;
}

} // namespace

std::optional<Block> Block::Deserialize(const std::vector<uint8_t>& data) {
    return Deserialize(data.data(), data.size());
}

std::optional<Block> Block::Deserialize(const uint8_t* data, size_t len) {
    if (!data || len < 128) {
        return std::nullopt;
    }

    // Explicit size gate: reject before any parsing if payload exceeds
    // MAX_BLOCK_WEIGHT bytes. With SegWit, the maximum serialized block
    // size equals MAX_BLOCK_WEIGHT (4 MB) since witness data has 1:1
    // weight. This prevents memory exhaustion from oversized payloads.
    if (len > consensus::MAX_BLOCK_WEIGHT) {
        return std::nullopt;
    }

    auto header_opt = BlockHeader::Deserialize(data, len);
    if (!header_opt.has_value()) {
        return std::nullopt;
    }

    Block out;
    out.header = *header_opt;
    out.vtx.clear();
    out.utreexo.reset();

    size_t offset = 128;

    uint64_t tx_count = 0;
    if (!ReadCompactSize(data, len, offset, tx_count)) {
        return std::nullopt;
    }

    out.vtx.reserve(static_cast<size_t>(tx_count));
    for (uint64_t i = 0; i < tx_count; ++i) {
        if (offset >= len) {
            return std::nullopt;
        }

        // TransactionSerializer consumes a prefix from the provided buffer.
        // We pass the remaining bytes and advance by `consumed`.
        std::vector<uint8_t> remaining(data + offset, data + len);
        Transaction tx;
        size_t consumed = 0;
        if (!TransactionSerializer::Deserialize(tx, remaining, consumed)) {
            return std::nullopt;
        }
        if (consumed == 0 || offset + consumed > len) {
            return std::nullopt;
        }

        out.vtx.push_back(std::move(tx));
        offset += consumed;
    }

    // Optional Utreexo data flag (1 byte) + payload.
    // New blocks always include a flag; older blocks may omit it.
    if (offset >= len) {
        out.utreexo.reset();
        return out;
    }

    const uint8_t flag = data[offset++];
    if (flag == 0x00) {
        out.utreexo.reset();
        return out;
    }

    if (flag != 0x01) {
        return std::nullopt;
    }

    try {
        std::vector<uint8_t> utreexo_bytes(data + offset, data + len);
        out.utreexo = consensus::BlockUtreexoData::deserialize(utreexo_bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    return out;
}

uint256 Block::GetHash() const {
    // Block hash is the hash of the header only (Bitcoin standard)
    // Transactions are committed via merkle root in the header
    return header.GetHash();
}

} // namespace dinero 
