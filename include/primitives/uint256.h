#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace dinero {

/**
 * @brief 256-bit hash type (canonical definition)
 *
 * Used throughout the codebase for block hashes, transaction hashes,
 * and cryptographic commitments.
 */
class uint256 {
public:
    uint8_t data[32];

    uint256() { memset(data, 0, 32); }

    bool operator==(const uint256& other) const;
    bool operator!=(const uint256& other) const { return !(*this == other); }
    bool operator<(const uint256& other) const;

    std::string ToString() const;
    std::string GetHex() const { return ToString(); }

    // Check if hash is null (all zeros)
    bool IsNull() const;

    // Set to null (all zeros)
    void SetNull();

    // STL-style iterators for byte access (enables range-based iteration)
    const uint8_t* begin() const { return data; }
    const uint8_t* end() const { return data + 32; }
    uint8_t* begin() { return data; }
    uint8_t* end() { return data + 32; }

    // Parse from hex string (returns false on error, does not throw)
    static bool FromHex(const std::string& hex, uint256& out);

    // Convenience wrapper that returns a null hash on parse error
    static uint256 FromHexUnsafe(const std::string& hex);
};

// Convenience function for creating uint256 from hex string literals (unsafe - returns null on error)
uint256 uint256S(const char* hex);

} // namespace dinero

// std::hash specialization for uint256 (enables use in unordered_map)
namespace std {
    template<>
    struct hash<dinero::uint256> {
        size_t operator()(const dinero::uint256& h) const noexcept {
            // Use first 8 bytes as hash (Bitcoin-style)
            size_t result;
            std::memcpy(&result, h.data, sizeof(size_t));
            return result;
        }
    };
}

// Phase M.2: Compiler enforcement of type boundaries (global invariants)

// Prevent accidental implicit conversions that could reintroduce Phase M.0 violations
static_assert(!std::is_convertible<dinero::uint256, std::string>::value,
              "uint256 must not implicitly convert to std::string (Phase M.2)");
static_assert(!std::is_constructible<dinero::uint256, std::string>::value,
              "uint256 must not be constructible from std::string (Phase M.2)");
static_assert(!std::is_constructible<dinero::uint256, const char*>::value,
              "uint256 must not be constructible from const char* (Phase M.2)");

// Ensure binary identity operations remain optimal
static_assert(std::is_trivially_copyable_v<dinero::uint256>,
              "uint256 must remain trivially copyable for memcmp optimization");
static_assert(sizeof(dinero::uint256) == 32,
              "uint256 must be exactly 32 bytes (consensus critical)");
