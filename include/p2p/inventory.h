/**
 * Phase G.1.4: Inventory Exchange - Message Structures
 *
 * Implements Bitcoin-style inventory announcement and request protocol.
 *
 * Messages:
 * - inv: Announce availability of objects (blocks, transactions)
 * - getdata: Request specific objects by hash
 * - notfound: Signal that requested objects are not available
 *
 * Design Principles:
 * - Pure networking (no validation coupling)
 * - Stateless (tracking happens elsewhere)
 * - Minimal (only essential fields)
 * - Bitcoin-compatible (interoperable message format)
 */

#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <array>
#include <cstring>

namespace dinero {
namespace p2p {

//=============================================================================
// Hash256: Simple 256-bit hash type for inventory
//=============================================================================

struct Hash256 {
    std::array<uint8_t, 32> data;

    Hash256() { data.fill(0); }

    bool operator==(const Hash256& other) const {
        return data == other.data;
    }

    bool operator!=(const Hash256& other) const {
        return data != other.data;
    }

    bool operator<(const Hash256& other) const {
        for (size_t i = 0; i < 32; i++) {
            if (data[i] != other.data[i]) {
                return data[i] < other.data[i];
            }
        }
        return false;  // Equal
    }

    std::string toHex() const;
    static Hash256 fromHex(const std::string& hex);
};

//=============================================================================
// Inventory Types
//=============================================================================

// Object types for inventory vectors
constexpr uint32_t MSG_TX = 1;              // Transaction
constexpr uint32_t MSG_BLOCK = 2;           // Block
// MSG_FILTERED_BLOCK (3) - Deferred to G.3 (Bloom filters)
// MSG_CMPCT_BLOCK (4) - Deferred to G.2 (Compact blocks)

//=============================================================================
// InventoryVector: Single inventory item
//=============================================================================

struct InventoryVector {
    uint32_t type;      // MSG_TX, MSG_BLOCK, etc.
    Hash256 hash;       // Object hash (block hash or txid)

    InventoryVector() : type(0), hash() {}
    InventoryVector(uint32_t t, const Hash256& h) : type(t), hash(h) {}

    // Serialization
    std::vector<uint8_t> serialize() const;
    static InventoryVector deserialize(const std::vector<uint8_t>& data, size_t& offset);

    // Comparison (for deduplication)
    bool operator==(const InventoryVector& other) const {
        return type == other.type && hash == other.hash;
    }

    bool operator!=(const InventoryVector& other) const {
        return !(*this == other);
    }

    // Ordering (for std::map)
    bool operator<(const InventoryVector& other) const {
        if (type != other.type) return type < other.type;
        return hash < other.hash;
    }

    // String representation (for debugging)
    std::string toString() const;
};

//=============================================================================
// InvMessage: Inventory announcement
//=============================================================================

struct InvMessage {
    std::vector<InventoryVector> inventory;

    InvMessage() = default;
    explicit InvMessage(const std::vector<InventoryVector>& inv) : inventory(inv) {}

    // Serialization
    std::vector<uint8_t> serialize() const;
    static InvMessage deserialize(const std::vector<uint8_t>& data);

    // Helpers
    void add(uint32_t type, const Hash256& hash) {
        inventory.emplace_back(type, hash);
    }

    size_t size() const { return inventory.size(); }
    bool empty() const { return inventory.empty(); }
};

//=============================================================================
// GetDataMessage: Request for inventory objects
//=============================================================================

struct GetDataMessage {
    std::vector<InventoryVector> inventory;

    GetDataMessage() = default;
    explicit GetDataMessage(const std::vector<InventoryVector>& inv) : inventory(inv) {}

    // Serialization
    std::vector<uint8_t> serialize() const;
    static GetDataMessage deserialize(const std::vector<uint8_t>& data);

    // Helpers
    void add(uint32_t type, const Hash256& hash) {
        inventory.emplace_back(type, hash);
    }

    size_t size() const { return inventory.size(); }
    bool empty() const { return inventory.empty(); }
};

//=============================================================================
// NotFoundMessage: Signal unavailable objects
//=============================================================================

struct NotFoundMessage {
    std::vector<InventoryVector> inventory;

    NotFoundMessage() = default;
    explicit NotFoundMessage(const std::vector<InventoryVector>& inv) : inventory(inv) {}

    // Serialization
    std::vector<uint8_t> serialize() const;
    static NotFoundMessage deserialize(const std::vector<uint8_t>& data);

    // Helpers
    void add(uint32_t type, const Hash256& hash) {
        inventory.emplace_back(type, hash);
    }

    size_t size() const { return inventory.size(); }
    bool empty() const { return inventory.empty(); }
};

} // namespace p2p
} // namespace dinero
