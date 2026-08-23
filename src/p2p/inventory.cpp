/**
 * Phase G.1.4: Inventory Exchange - Implementation
 *
 * Serialization/deserialization for inventory messages following Bitcoin's wire protocol.
 */

#include "../../include/p2p/inventory.h"
#include "p2p/p2p_limits.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

namespace dinero {
namespace p2p {

//=============================================================================
// Hash256 Implementation
//=============================================================================

std::string Hash256::toHex() const {
    const char* hex_chars = "0123456789abcdef";
    std::string result;
    result.reserve(64);

    for (size_t i = 0; i < 32; i++) {
        result += hex_chars[(data[i] >> 4) & 0xF];
        result += hex_chars[data[i] & 0xF];
    }

    return result;
}

Hash256 Hash256::fromHex(const std::string& hex) {
    Hash256 result;

    if (hex.length() != 64) {
        return result;  // Return zero hash if invalid
    }

    for (size_t i = 0; i < 32; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];

        auto hex_digit = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };

        result.data[i] = (hex_digit(hi) << 4) | hex_digit(lo);
    }

    return result;
}

//=============================================================================
// Serialization Helpers
//=============================================================================

static void write_uint32(std::vector<uint8_t>& buf, uint32_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
    buf.push_back((value >> 16) & 0xFF);
    buf.push_back((value >> 24) & 0xFF);
}

static uint32_t read_uint32(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset > buf.size() || buf.size() - offset < sizeof(uint32_t)) {
        throw std::invalid_argument("inventory: truncated uint32");
    }
    uint32_t value = 0;
    value |= static_cast<uint32_t>(buf[offset++]);
    value |= static_cast<uint32_t>(buf[offset++]) << 8;
    value |= static_cast<uint32_t>(buf[offset++]) << 16;
    value |= static_cast<uint32_t>(buf[offset++]) << 24;
    return value;
}

static void write_varint(std::vector<uint8_t>& buf, uint64_t value) {
    if (value < 0xFD) {
        buf.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
        buf.push_back(0xFD);
        buf.push_back(value & 0xFF);
        buf.push_back((value >> 8) & 0xFF);
    } else if (value <= 0xFFFFFFFF) {
        buf.push_back(0xFE);
        write_uint32(buf, static_cast<uint32_t>(value));
    } else {
        buf.push_back(0xFF);
        for (int i = 0; i < 8; i++) {
            buf.push_back((value >> (i * 8)) & 0xFF);
        }
    }
}

static uint64_t read_varint(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset >= buf.size()) {
        throw std::invalid_argument("inventory: missing CompactSize");
    }
    uint8_t first = buf[offset++];

    if (first < 0xFD) {
        return first;
    } else if (first == 0xFD) {
        if (buf.size() - offset < 2) {
            throw std::invalid_argument("inventory: truncated CompactSize16");
        }
        uint64_t value = buf[offset++];
        value |= static_cast<uint64_t>(buf[offset++]) << 8;
        return value;
    } else if (first == 0xFE) {
        return read_uint32(buf, offset);
    } else { // 0xFF
        if (buf.size() - offset < 8) {
            throw std::invalid_argument("inventory: truncated CompactSize64");
        }
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= static_cast<uint64_t>(buf[offset++]) << (i * 8);
        }
        return value;
    }
}

static void write_hash(std::vector<uint8_t>& buf, const Hash256& hash) {
    // Write hash bytes (32 bytes, little-endian)
    for (size_t i = 0; i < 32; i++) {
        buf.push_back(hash.data[i]);
    }
}

static Hash256 read_hash(const std::vector<uint8_t>& buf, size_t& offset) {
    if (offset > buf.size() || buf.size() - offset < 32) {
        throw std::invalid_argument("inventory: truncated hash");
    }
    Hash256 hash;
    for (size_t i = 0; i < 32; i++) {
        hash.data[i] = buf[offset++];
    }
    return hash;
}

//=============================================================================
// InventoryVector Implementation
//=============================================================================

std::vector<uint8_t> InventoryVector::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(36);  // 4 bytes type + 32 bytes hash

    write_uint32(buf, type);
    write_hash(buf, hash);

    return buf;
}

InventoryVector InventoryVector::deserialize(const std::vector<uint8_t>& data, size_t& offset) {
    InventoryVector inv;

    inv.type = read_uint32(data, offset);
    inv.hash = read_hash(data, offset);

    return inv;
}

static std::vector<InventoryVector> deserialize_inventory(
    const std::vector<uint8_t>& data) {
    size_t offset = 0;
    const uint64_t count = read_varint(data, offset);
    constexpr size_t kInventoryVectorBytes = sizeof(uint32_t) + 32;

    if (count > P2P_MAX_INV_HASHES_PER_MSG) {
        throw std::invalid_argument("inventory: item count exceeds protocol limit");
    }
    if (count > (data.size() - offset) / kInventoryVectorBytes) {
        throw std::invalid_argument("inventory: item count exceeds payload bytes");
    }

    std::vector<InventoryVector> inventory;
    inventory.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        inventory.push_back(InventoryVector::deserialize(data, offset));
    }
    if (offset != data.size()) {
        throw std::invalid_argument("inventory: trailing payload bytes");
    }
    return inventory;
}

std::string InventoryVector::toString() const {
    std::stringstream ss;

    const char* type_str = "UNKNOWN";
    if (type == MSG_TX) type_str = "TX";
    else if (type == MSG_BLOCK) type_str = "BLOCK";

    ss << type_str << ":" << hash.toHex().substr(0, 16) << "...";
    return ss.str();
}

//=============================================================================
// InvMessage Implementation
//=============================================================================

std::vector<uint8_t> InvMessage::serialize() const {
    std::vector<uint8_t> buf;

    // Write count
    write_varint(buf, inventory.size());

    // Write each inventory vector
    for (const auto& inv : inventory) {
        auto inv_bytes = inv.serialize();
        buf.insert(buf.end(), inv_bytes.begin(), inv_bytes.end());
    }

    return buf;
}

InvMessage InvMessage::deserialize(const std::vector<uint8_t>& data) {
    InvMessage msg;
    msg.inventory = deserialize_inventory(data);
    return msg;
}

//=============================================================================
// GetDataMessage Implementation
//=============================================================================

std::vector<uint8_t> GetDataMessage::serialize() const {
    std::vector<uint8_t> buf;

    // Write count
    write_varint(buf, inventory.size());

    // Write each inventory vector
    for (const auto& inv : inventory) {
        auto inv_bytes = inv.serialize();
        buf.insert(buf.end(), inv_bytes.begin(), inv_bytes.end());
    }

    return buf;
}

GetDataMessage GetDataMessage::deserialize(const std::vector<uint8_t>& data) {
    GetDataMessage msg;
    msg.inventory = deserialize_inventory(data);
    return msg;
}

//=============================================================================
// NotFoundMessage Implementation
//=============================================================================

std::vector<uint8_t> NotFoundMessage::serialize() const {
    std::vector<uint8_t> buf;

    // Write count
    write_varint(buf, inventory.size());

    // Write each inventory vector
    for (const auto& inv : inventory) {
        auto inv_bytes = inv.serialize();
        buf.insert(buf.end(), inv_bytes.begin(), inv_bytes.end());
    }

    return buf;
}

NotFoundMessage NotFoundMessage::deserialize(const std::vector<uint8_t>& data) {
    NotFoundMessage msg;
    msg.inventory = deserialize_inventory(data);
    return msg;
}

} // namespace p2p
} // namespace dinero
