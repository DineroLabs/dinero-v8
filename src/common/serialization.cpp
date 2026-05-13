#include "common/serialization.h"
#include "common/uint256_le64.hpp"
#include "storage/tip_info.h"
#include <cstring>
#include <cstdint>

namespace dinero {

// Canonical HeaderInfo serialization (fixed-width little-endian)
bool Serialize(const HeaderInfo& header, std::string& out) {
    out.clear();
    out.reserve(32 + 4 + 8 + 4 + 32); // hash + height + work + timestamp + prev_hash

    // Hash (32 bytes raw binary string)
    if (header.hash.size() != 32) return false;
    out.append(header.hash);

    // Height (4 bytes, little-endian)
    uint32_t height_le = header.height;
    out.append(reinterpret_cast<const char*>(&height_le), 4);

    // Work (8 bytes, little-endian)
    uint64_t work_le = header.work;
    out.append(reinterpret_cast<const char*>(&work_le), 8);

    // Timestamp (4 bytes, little-endian)
    uint32_t timestamp_le = header.timestamp;
    out.append(reinterpret_cast<const char*>(&timestamp_le), 4);

    // Previous hash (32 bytes raw binary string)
    if (header.prev_hash.size() != 32) return false;
    out.append(header.prev_hash);

    return true;
}

bool Deserialize(std::string_view data, HeaderInfo& header) {
    if (data.size() != 80) return false; // 32 + 4 + 8 + 4 + 32
    
    size_t offset = 0;
    
    // Hash (32 bytes)
    header.hash = std::string(data.substr(offset, 32));
    offset += 32;
    
    // Height (4 bytes, little-endian)
    std::memcpy(&header.height, data.data() + offset, 4);
    offset += 4;
    
    // Work (8 bytes, little-endian)
    std::memcpy(&header.work, data.data() + offset, 8);
    offset += 8;
    
    // Timestamp (4 bytes, little-endian)
    std::memcpy(&header.timestamp, data.data() + offset, 4);
    offset += 4;
    
    // Previous hash (32 bytes)
    header.prev_hash = std::string(data.substr(offset, 32));
    
    return true;
}

// Canonical TipInfo serialization (fixed-width little-endian)
bool Serialize(const TipInfo& tip, std::string& out) {
    out.clear();
    out.reserve(32 + 4 + 8 + 4); // hash + height + work + timestamp

    // Hash (32 bytes raw binary)
    out.append(reinterpret_cast<const char*>(tip.hash.data), 32);

    // Height (4 bytes, little-endian)
    uint32_t height_le = tip.height;
    out.append(reinterpret_cast<const char*>(&height_le), 4);

    // Work (8 bytes, little-endian) - extract low 64 bits portably
    uint64_t work_le = Low64LE(tip.work);
    out.append(reinterpret_cast<const char*>(&work_le), 8);

    // Timestamp (4 bytes, little-endian)
    uint32_t timestamp_le = tip.timestamp;
    out.append(reinterpret_cast<const char*>(&timestamp_le), 4);

    return true;
}

bool Deserialize(std::string_view data, TipInfo& tip) {
    if (data.size() != 48) return false; // 32 + 4 + 8 + 4

    size_t offset = 0;

    // Hash (32 bytes raw binary)
    std::memcpy(tip.hash.data, data.data() + offset, 32);
    offset += 32;
    
    // Height (4 bytes, little-endian)
    std::memcpy(&tip.height, data.data() + offset, 4);
    offset += 4;
    
    // Work (8 bytes, little-endian) - reconstruct arith_uint256 from low 64 bits
    uint64_t work_value;
    std::memcpy(&work_value, data.data() + offset, 8);
    tip.work = arith_uint256(work_value);
    offset += 8;
    
    // Timestamp (4 bytes, little-endian)
    std::memcpy(&tip.timestamp, data.data() + offset, 4);
    
    return true;
}

} // namespace dinero
