#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace dinero {
namespace solo {

// 256-bit hash (32 bytes)
using Hash256 = std::array<uint8_t, 32>;

// Block header constants (DineroCoin BlockHeader v1 - 128 bytes FROZEN)
constexpr size_t HEADER_SIZE = 128;
constexpr size_t HASH_SIZE = 32;

// Header field offsets
constexpr size_t OFFSET_VERSION = 0;
constexpr size_t OFFSET_PREV_HASH = 4;
constexpr size_t OFFSET_MERKLE_ROOT = 36;
constexpr size_t OFFSET_UTREEXO_ROOT = 68;
constexpr size_t OFFSET_TIMESTAMP = 100;
constexpr size_t OFFSET_DIFFICULTY = 108;
constexpr size_t OFFSET_NONCE = 112;
constexpr size_t OFFSET_RESERVED = 116;

// Header field sizes
constexpr size_t SIZE_VERSION = 4;
constexpr size_t SIZE_PREV_HASH = 32;
constexpr size_t SIZE_MERKLE_ROOT = 32;
constexpr size_t SIZE_UTREEXO_ROOT = 32;
constexpr size_t SIZE_TIMESTAMP = 8;
constexpr size_t SIZE_DIFFICULTY = 4;
constexpr size_t SIZE_NONCE = 4;
constexpr size_t SIZE_RESERVED = 12;

/**
 * Block header structure (128 bytes)
 * Must match DineroCoin consensus exactly.
 */
#pragma pack(push, 1)
struct BlockHeader {
    uint32_t version;            // 4 bytes
    uint8_t  prev_hash[32];      // 32 bytes
    uint8_t  merkle_root[32];    // 32 bytes
    uint8_t  utreexo_root[32];   // 32 bytes
    uint64_t timestamp;          // 8 bytes
    uint32_t difficulty;         // 4 bytes (compact target)
    uint32_t nonce;              // 4 bytes
    uint8_t  reserved[12];       // 12 bytes (MUST be zero)
};
#pragma pack(pop)

static_assert(sizeof(BlockHeader) == HEADER_SIZE, "BlockHeader must be 128 bytes");

/**
 * Convert compact difficulty (nBits) to 256-bit target
 */
Hash256 compactToTarget(uint32_t compact);

/**
 * Compare hash against target (hash <= target means valid)
 */
bool hashMeetsTarget(const Hash256& hash, const Hash256& target);

/**
 * Convert hex string to bytes
 */
std::vector<uint8_t> hexToBytes(const std::string& hex);

/**
 * Convert bytes to hex string
 */
std::string bytesToHex(const uint8_t* data, size_t len);
std::string bytesToHex(const std::vector<uint8_t>& data);

/**
 * Reverse byte order (for hash display)
 */
Hash256 reverseHash(const Hash256& hash);

} // namespace solo
} // namespace dinero
