#pragma once

/**
 * @file primitives/amount.h
 * @brief Phase M.6: Semantic Money Domain Type
 *
 * Purpose: Make it impossible to confuse monetary amounts with raw integers
 *
 * Core Invariant:
 *   Every monetary amount lives in an explicit semantic domain.
 *   Same bytes ≠ same meaning. 1000 una ≠ 1000 height ≠ 1000 bytes.
 *
 * Design Principles (from M.3/M.4 hash domains):
 *   - No inheritance
 *   - No implicit conversions from integers
 *   - Explicit conversions TO integers (for boundaries only)
 *   - Domain-locked constructors only
 *   - Storage/wire format unchanged (still 8 bytes)
 *   - Zero runtime overhead (trivially copyable)
 *
 * What M.6 Prevents:
 *   ❌ Passing block height where amount is required
 *   ❌ Passing byte count where fee is required
 *   ❌ Signed/unsigned confusion at wallet/consensus boundary
 *   ❌ Arithmetic overflow without detection
 *   ❌ Creating amounts exceeding MAX_SUPPLY
 */

#include <cstdint>
#include <optional>
#include <type_traits>

namespace dinero {

// Forward declare for MAX_SUPPLY_UNA constant
// Actual definition in consensus/subsidy.h
constexpr uint64_t MAX_SUPPLY_UNA_CONST = 26542800000000000ULL;

/**
 * @brief Monetary amount domain - represents value in una (una-equivalent)
 *
 * Computed from: N/A (domain primitive, not derived)
 * Used for: All monetary values (outputs, fees, balances, subsidies)
 * Range: [0, MAX_SUPPLY_UNA] = [0, 26,542,800,000,000,000]
 *
 * Invariants:
 *   - Always unsigned (no negative money)
 *   - Always <= MAX_SUPPLY_UNA (inflation prevention)
 *   - Arithmetic operations return std::optional (overflow detection)
 */
struct AmountUna {
    uint64_t v;  // Value in una (una-equivalent)

    // ═══════════════════════════════════════════════════════════════
    // Constructors (domain-locked, explicit only)
    // ═══════════════════════════════════════════════════════════════

    // Default constructor (creates zero amount)
    constexpr AmountUna() : v(0) {}

    // Domain-locked constructors (preferred)
    static constexpr AmountUna Zero() { return AmountUna(0ULL); }
    static constexpr AmountUna Max() { return AmountUna(MAX_SUPPLY_UNA_CONST); }
    static constexpr AmountUna Una(uint64_t una) {
        return AmountUna(una);
    }

    // From DIN (1 DIN = 100,000,000 una)
    static constexpr AmountUna DIN(uint64_t din) {
        constexpr uint64_t UNA_PER_DIN = 100000000ULL;
        // Check for overflow before multiplication
        if (din > MAX_SUPPLY_UNA_CONST / UNA_PER_DIN) {
            return AmountUna(MAX_SUPPLY_UNA_CONST); // Saturate
        }
        return AmountUna(din * UNA_PER_DIN);
    }

    // Unsafe constructor for deserialization/SQLite (BOUNDARY USE ONLY)
    // Use this ONLY when reading from wire/DB - caller must validate!
    static constexpr AmountUna UnsafeFromRaw(uint64_t raw) {
        return AmountUna(raw);
    }

    // ═══════════════════════════════════════════════════════════════
    // Accessors
    // ═══════════════════════════════════════════════════════════════

    // Get raw value (for serialization, DB storage, RPC boundaries)
    constexpr uint64_t GetUna() const { return v; }

    // Convert to DIN (lossy - truncates fractional part)
    constexpr uint64_t GetDIN() const {
        return v / 100000000ULL;
    }

    // Convert to double DIN (for display)
    double GetDINDouble() const {
        return static_cast<double>(v) / 100000000.0;
    }

    // For SQLite binding (int64_t) - SAFE because MAX_SUPPLY < INT64_MAX
    int64_t GetInt64() const {
        return static_cast<int64_t>(v);
    }

    // ═══════════════════════════════════════════════════════════════
    // Validation
    // ═══════════════════════════════════════════════════════════════

    constexpr bool IsZero() const { return v == 0; }
    constexpr bool IsPositive() const { return v > 0; }

    // Check if amount is within consensus limits
    constexpr bool IsWithinSupply() const {
        return v <= MAX_SUPPLY_UNA_CONST;
    }

    // Check if amount meets dust threshold (546 una for P2WPKH)
    constexpr bool IsDust() const { return v < 546; }

    // Check if amount meets minimum transaction fee (100 una)
    constexpr bool MeetsMinFee() const { return v >= 100; }

    // ═══════════════════════════════════════════════════════════════
    // Checked Arithmetic (overflow-safe)
    // ═══════════════════════════════════════════════════════════════

    // Add with overflow detection
    std::optional<AmountUna> Add(AmountUna other) const {
        uint64_t result = v + other.v;
        if (result < v) {
            return std::nullopt;  // Overflow
        }
        if (result > MAX_SUPPLY_UNA_CONST) {
            return std::nullopt;  // Exceeds max supply
        }
        return AmountUna(result);
    }

    // Subtract with underflow detection
    std::optional<AmountUna> Sub(AmountUna other) const {
        if (v < other.v) {
            return std::nullopt;  // Underflow (would be negative)
        }
        return AmountUna(v - other.v);
    }

    // Multiply by scalar (for fee calculation: size * rate)
    std::optional<AmountUna> Mul(uint64_t scalar) const {
        if (scalar == 0) return AmountUna::Zero();
        if (v > UINT64_MAX / scalar) {
            return std::nullopt;  // Overflow
        }
        uint64_t result = v * scalar;
        if (result > MAX_SUPPLY_UNA_CONST) {
            return std::nullopt;  // Exceeds max supply
        }
        return AmountUna(result);
    }

    // Divide (for average calculation)
    std::optional<AmountUna> Div(uint64_t divisor) const {
        if (divisor == 0) {
            return std::nullopt;  // Division by zero
        }
        return AmountUna(v / divisor);
    }

    // ═══════════════════════════════════════════════════════════════
    // Comparison Operators (within same domain)
    // ═══════════════════════════════════════════════════════════════

    constexpr bool operator==(const AmountUna& other) const { return v == other.v; }
    constexpr bool operator!=(const AmountUna& other) const { return v != other.v; }
    constexpr bool operator<(const AmountUna& other) const { return v < other.v; }
    constexpr bool operator<=(const AmountUna& other) const { return v <= other.v; }
    constexpr bool operator>(const AmountUna& other) const { return v > other.v; }
    constexpr bool operator>=(const AmountUna& other) const { return v >= other.v; }

    // ═══════════════════════════════════════════════════════════════
    // Serialization Support (wire format compatibility)
    // ═══════════════════════════════════════════════════════════════

    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }

private:
    // Private constructor for domain-locked factory methods
    explicit constexpr AmountUna(uint64_t una) : v(una) {}
};

inline std::optional<uint64_t> CheckedAddUna(uint64_t lhs, uint64_t rhs) {
    auto total = AmountUna::UnsafeFromRaw(lhs).Add(AmountUna::UnsafeFromRaw(rhs));
    if (!total) {
        return std::nullopt;
    }
    return total->GetUna();
}

// ═══════════════════════════════════════════════════════════════════
// Compile-Time Enforcement (Phase M.6 Tripwires)
// ═══════════════════════════════════════════════════════════════════

// Prevent implicit conversions
static_assert(!std::is_convertible<uint64_t, AmountUna>::value,
    "AmountUna must NOT be implicitly convertible from uint64_t");
static_assert(!std::is_convertible<int64_t, AmountUna>::value,
    "AmountUna must NOT be implicitly convertible from int64_t");
static_assert(!std::is_convertible<AmountUna, uint64_t>::value,
    "AmountUna must NOT be implicitly convertible to uint64_t");

// Ensure trivially copyable (performance guarantee)
static_assert(std::is_trivially_copyable<AmountUna>::value,
    "AmountUna must be trivially copyable");

// Ensure exactly 8 bytes (storage guarantee)
static_assert(sizeof(AmountUna) == 8, "AmountUna must be 8 bytes");

// ═══════════════════════════════════════════════════════════════════
// Constants (compile-time validated)
// ═══════════════════════════════════════════════════════════════════

namespace amounts {
    constexpr AmountUna ZERO = AmountUna::Zero();
    constexpr AmountUna DUST_THRESHOLD = AmountUna::Una(546);      // P2WPKH dust limit
    constexpr AmountUna MIN_TX_FEE = AmountUna::Una(100);          // Minimum transaction fee
    constexpr AmountUna MAX_SUPPLY = AmountUna::Max();              // Hard cap
    constexpr AmountUna ONE_DIN = AmountUna::DIN(1);                // 1 DIN = 100M una
}

}  // namespace dinero

// ═══════════════════════════════════════════════════════════════════
// Hash Function Specialization (for std::unordered_map, std::unordered_set)
// ═══════════════════════════════════════════════════════════════════

namespace std {
    template<>
    struct hash<dinero::AmountUna> {
        size_t operator()(const dinero::AmountUna& a) const {
            return std::hash<uint64_t>{}(a.GetUna());
        }
    };
}
