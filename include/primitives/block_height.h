#pragma once

/**
 * @file primitives/block_height.h
 * @brief Phase M.6: Semantic Block Height Domain
 *
 * Purpose: Prevent height misuse and provide overflow-safe arithmetic
 *
 * Core Invariant:
 *   Every block height is an unsigned 32-bit value with semantic meaning.
 *   Heights have special values (Genesis=0, FirstPoW=1).
 *
 * Design Principles:
 *   - No implicit conversions to/from int or uint32_t
 *   - Domain-locked constructors for special heights
 *   - Overflow-safe arithmetic (no UB on addition)
 *   - Built-in maturity checking
 *   - Zero runtime overhead (trivially copyable)
 *
 * What M.6 Prevents:
 *   ❌ Height underflow (current - coinbase when current < coinbase)
 *   ❌ Height overflow (height + COINBASE_MATURITY > UINT32_MAX)
 *   ❌ Negative heights from signed/unsigned confusion
 *   ❌ Implicit conversions hiding bugs
 *   ❌ Maturity calculations with wrong operands
 */

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace dinero {

// Special height constants (compile-time)
struct HeightConstants {
    static constexpr uint32_t GENESIS_VALUE = 0;
    static constexpr uint32_t FIRST_POW_VALUE = 1;
    static constexpr uint32_t COINBASE_MATURITY = 100;

    // Maximum valid height (leaves room for sentinel values)
    static constexpr uint32_t MAX_HEIGHT = UINT32_MAX - 1000;
};

/**
 * @brief Block height domain - identifies a position in the blockchain
 *
 * Wraps uint32_t with semantic type safety and overflow protection.
 * Heights range from 0 (genesis) to ~4.2 billion (UINT32_MAX).
 */
class BlockHeight {
private:
    uint32_t v_;

public:
    // ═══════════════════════════════════════════════════════════════════════
    // Constructors (Domain-Locked)
    // ═══════════════════════════════════════════════════════════════════════

    // Default constructor (creates invalid/null height)
    BlockHeight() : v_(UINT32_MAX) {}

    // Named constructors for special heights
    static constexpr BlockHeight Genesis() { return BlockHeight(HeightConstants::GENESIS_VALUE); }
    static constexpr BlockHeight FirstPoW() { return BlockHeight(HeightConstants::FIRST_POW_VALUE); }

    // Safe construction from uint32_t with validation
    static std::optional<BlockHeight> FromUint32(uint32_t height) {
        if (height > HeightConstants::MAX_HEIGHT) {
            return std::nullopt;
        }
        return BlockHeight(height);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Boundary Conversions (Explicit Only)
    // ═══════════════════════════════════════════════════════════════════════

    // Convert to uint32_t (explicit)
    uint32_t AsUint32() const { return v_; }

    // Convert to int for legacy database APIs (explicit, lossy)
    // WARNING: Only use at ChainDB/SQLite boundaries
    // Returns -1 if height doesn't fit in int (defensive)
    int AsInt() const {
        if (v_ > static_cast<uint32_t>(INT32_MAX)) {
            return -1; // Sentinel for invalid conversion
        }
        return static_cast<int>(v_);
    }

    // Convert from int (database boundary ingress)
    static std::optional<BlockHeight> FromInt(int height) {
        if (height < 0) {
            return std::nullopt; // Reject negative heights
        }
        return BlockHeight(static_cast<uint32_t>(height));
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Validation Methods
    // ═══════════════════════════════════════════════════════════════════════

    bool IsNull() const { return v_ == UINT32_MAX; }
    bool IsValid() const { return v_ <= HeightConstants::MAX_HEIGHT; }
    bool IsGenesis() const { return v_ == HeightConstants::GENESIS_VALUE; }
    bool IsPoW() const { return v_ >= HeightConstants::FIRST_POW_VALUE && IsValid(); }

    // ═══════════════════════════════════════════════════════════════════════
    // Maturity-Aware Methods (Coinbase Logic)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if a coinbase at this height is mature at current_height
     * @param current_height Current blockchain tip height
     * @return true if this coinbase can be spent at current_height
     */
    bool IsMatureAt(BlockHeight current_height) const {
        if (current_height.v_ < v_) {
            return false; // Current height before coinbase
        }
        uint32_t blocks_on_top = current_height.v_ - v_;
        return blocks_on_top >= HeightConstants::COINBASE_MATURITY;
    }

    /**
     * Calculate when a coinbase at this height becomes spendable
     * @return Height at which coinbase becomes mature, or nullopt if overflow
     */
    std::optional<BlockHeight> GetSpendableHeight() const {
        // Check overflow (both UINT32_MAX and MAX_HEIGHT)
        if (v_ > UINT32_MAX - HeightConstants::COINBASE_MATURITY) {
            return std::nullopt;
        }
        uint32_t result = v_ + HeightConstants::COINBASE_MATURITY;
        if (result > HeightConstants::MAX_HEIGHT) {
            return std::nullopt;
        }
        return BlockHeight(result);
    }

    /**
     * Calculate blocks until this coinbase matures
     * @param current_height Current blockchain tip height
     * @return Blocks remaining (0 if already mature)
     */
    uint32_t BlocksUntilMature(BlockHeight current_height) const {
        if (IsMatureAt(current_height)) {
            return 0;
        }
        if (current_height.v_ < v_) {
            return HeightConstants::COINBASE_MATURITY; // Conservative
        }
        uint32_t blocks_on_top = current_height.v_ - v_;
        return HeightConstants::COINBASE_MATURITY - blocks_on_top;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Overflow-Safe Arithmetic
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Safe successor (height + 1)
     * @return Next height, or nullopt on overflow
     */
    std::optional<BlockHeight> Next() const {
        if (v_ == UINT32_MAX || v_ >= HeightConstants::MAX_HEIGHT) {
            return std::nullopt;
        }
        return BlockHeight(v_ + 1);
    }

    /**
     * Safe predecessor (height - 1)
     * @return Previous height, or nullopt if already at genesis
     */
    std::optional<BlockHeight> Prev() const {
        if (v_ == 0) {
            return std::nullopt;
        }
        return BlockHeight(v_ - 1);
    }

    /**
     * Safe addition
     * @param offset Amount to add
     * @return New height, or nullopt on overflow
     */
    std::optional<BlockHeight> Add(uint32_t offset) const {
        if (v_ > UINT32_MAX - offset) {
            return std::nullopt; // Would overflow UINT32_MAX
        }
        uint32_t result = v_ + offset;
        if (result > HeightConstants::MAX_HEIGHT) {
            return std::nullopt; // Would exceed MAX_HEIGHT
        }
        return BlockHeight(result);
    }

    /**
     * Safe subtraction (for distance calculations)
     * @param offset Amount to subtract
     * @return New height, or nullopt on underflow
     */
    std::optional<BlockHeight> Sub(uint32_t offset) const {
        if (v_ < offset) {
            return std::nullopt; // Would underflow
        }
        return BlockHeight(v_ - offset);
    }

    /**
     * Calculate distance between heights (always positive)
     * @param other Another height
     * @return Absolute distance, or nullopt if either height invalid
     */
    std::optional<uint32_t> DistanceTo(BlockHeight other) const {
        if (!IsValid() || !other.IsValid()) {
            return std::nullopt;
        }
        if (v_ >= other.v_) {
            return v_ - other.v_;
        } else {
            return other.v_ - v_;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Comparison Operators (Within Same Domain)
    // ═══════════════════════════════════════════════════════════════════════

    bool operator==(BlockHeight other) const { return v_ == other.v_; }
    bool operator!=(BlockHeight other) const { return v_ != other.v_; }
    bool operator<(BlockHeight other) const { return v_ < other.v_; }
    bool operator<=(BlockHeight other) const { return v_ <= other.v_; }
    bool operator>(BlockHeight other) const { return v_ > other.v_; }
    bool operator>=(BlockHeight other) const { return v_ >= other.v_; }

    // ═══════════════════════════════════════════════════════════════════════
    // Serialization Support
    // ═══════════════════════════════════════════════════════════════════════

    template <typename Stream>
    void Serialize(Stream& s) const {
        s << v_;
    }

    template <typename Stream>
    void Unserialize(Stream& s) {
        s >> v_;
    }

    // String conversion (for logging/debugging)
    std::string ToString() const {
        if (IsNull()) return "null";
        if (IsGenesis()) return "0 (genesis)";
        return std::to_string(v_);
    }

private:
    // Private constructor for domain-locked factory methods
    explicit constexpr BlockHeight(uint32_t h) : v_(h) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Compile-Time Enforcement (Phase M.6 Tripwires)
// ═══════════════════════════════════════════════════════════════════════════

// Prevent implicit conversions to/from primitive types
static_assert(!std::is_convertible<BlockHeight, uint32_t>::value,
    "BlockHeight must NOT be implicitly convertible to uint32_t");
static_assert(!std::is_convertible<uint32_t, BlockHeight>::value,
    "uint32_t must NOT be implicitly convertible to BlockHeight");
static_assert(!std::is_convertible<BlockHeight, int>::value,
    "BlockHeight must NOT be implicitly convertible to int");
static_assert(!std::is_convertible<int, BlockHeight>::value,
    "int must NOT be implicitly convertible to BlockHeight");

// Ensure trivially copyable (zero runtime overhead)
static_assert(std::is_trivially_copyable<BlockHeight>::value,
    "BlockHeight must be trivially copyable (performance guarantee)");

// Ensure correct size (4 bytes)
static_assert(sizeof(BlockHeight) == sizeof(uint32_t),
    "BlockHeight must be exactly 4 bytes (same as uint32_t)");

}  // namespace dinero

// ═══════════════════════════════════════════════════════════════════════════
// Hash Function Specialization (for std::unordered_map, std::unordered_set)
// ═══════════════════════════════════════════════════════════════════════════

namespace std {

template<>
struct hash<dinero::BlockHeight> {
    size_t operator()(dinero::BlockHeight h) const {
        return std::hash<uint32_t>{}(h.AsUint32());
    }
};

}  // namespace std
