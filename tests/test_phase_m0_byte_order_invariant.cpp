/**
 * @file test_phase_m0_byte_order_invariant.cpp
 * @brief Phase M.0 Byte Order Invariant - Permanent Lock Test
 *
 * CRITICAL INVARIANT (NEVER MODIFY):
 * - DoubleSHA256Bytes() returns raw CSHA256::Finalize() output (no reversal)
 * - DoubleSHA256() reverses bytes then calls ToHex() (for display)
 * - uint256 stores bytes via memcpy from DoubleSHA256Bytes()
 * - GetHex() reverses uint256.data bytes for display
 * - GetTxid()/GetWtxid() use memcpy (preserve identity)
 *
 * This test ensures that:
 * 1. DoubleSHA256Bytes returns raw SHA256 output
 * 2. DoubleSHA256 reverses for display consistency
 * 3. memcpy preserves byte identity (no reversal)
 * 4. Merkle trees operate on raw bytes, not hex strings
 *
 * IF THIS TEST FAILS, YOU HAVE BROKEN CONSENSUS COMPATIBILITY.
 */

#include "wallet/transaction.h"
#include "primitives/uint256.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════
// Test Utilities - Type-Safe Assertions
// ═══════════════════════════════════════════════════════════════════════════

// Type-safe assertion for general types
template <typename T, typename U>
void assert_eq_impl(const T& expected, const U& got, const char* file, int line) {
    if (!(expected == got)) {
        std::cerr << "\n  ❌ ASSERT_EQ failed at " << file << ":" << line << "\n"
                  << "    Expected: " << expected << "\n"
                  << "    Got:      " << got << std::endl;
        std::exit(1);
    }
}

// Specialization for uint8_t (print as hex)
inline void assert_eq_impl(uint8_t expected, uint8_t got, const char* file, int line) {
    if (expected != got) {
        std::cerr << "\n  ❌ ASSERT_EQ failed at " << file << ":" << line << "\n"
                  << "    Expected: 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(expected) << "\n"
                  << "    Got:      0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(got) << std::dec << std::endl;
        std::exit(1);
    }
}

// Specialization for size_t
inline void assert_eq_impl(size_t expected, size_t got, const char* file, int line) {
    if (expected != got) {
        std::cerr << "\n  ❌ ASSERT_EQ failed at " << file << ":" << line << "\n"
                  << "    Expected: " << expected << "\n"
                  << "    Got:      " << got << std::endl;
        std::exit(1);
    }
}

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "Running: " << #name << "..." << std::flush; \
            test_##name(); \
            std::cout << " ✅" << std::endl; \
        } \
    } test_runner_##name; \
    void test_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n  ❌ ASSERT_TRUE failed at " << __FILE__ << ":" << __LINE__ << "\n" \
                      << "    Condition: " << #cond << std::endl; \
            std::exit(1); \
        } \
    } while(0)

#define ASSERT_EQ(expected, got) \
    assert_eq_impl((expected), (got), __FILE__, __LINE__)

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: uint256 Internal Format is Little-Endian
// ═══════════════════════════════════════════════════════════════════════════

TEST(uint256_internal_is_little_endian) {
    // Known test vector: SHA256("hello")
    // Display format (big-endian hex): 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824

    uint256 hash;
    // Manually set bytes in little-endian order (reversed from display)
    hash.data[0] = 0x24;   // LSB (last byte pair in display hex)
    hash.data[1] = 0x98;
    hash.data[2] = 0x8b;
    hash.data[3] = 0x93;
    hash.data[31] = 0x2c;  // MSB (first byte pair in display hex)

    // GetHex() should reverse bytes for display
    std::string hex = hash.GetHex();
    ASSERT_EQ(hex.substr(0, 2), "2c");  // First hex pair is MSB (data[31])
    ASSERT_EQ(hex.substr(62, 2), "24"); // Last hex pair is LSB (data[0])

    // Internal storage should be little-endian
    ASSERT_EQ(hash.data[0], 0x24);  // LSB
    ASSERT_EQ(hash.data[31], 0x2c); // MSB
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: DoubleSHA256Bytes Returns Raw Little-Endian Bytes
// ═══════════════════════════════════════════════════════════════════════════

TEST(double_sha256_bytes_returns_raw_sha256_output) {
    // Test vector: empty input
    std::vector<uint8_t> empty;
    auto hash = TransactionSerializer::DoubleSHA256Bytes(empty);
    std::string display = TransactionSerializer::DoubleSHA256(empty);

    ASSERT_EQ(hash.size(), 32);

    // Debug: print actual values
    std::cout << "\n  Raw bytes:   ";
    for (int i = 0; i < 32; i++) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    }
    std::cout << "\n  Display hex: " << display;
    std::cout << "\n  hash[0]=" << std::hex << (int)hash[0] << " hash[31]=" << (int)hash[31] << std::dec << "\n";

    // SHA256(SHA256("")) display hex = 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
    // DoubleSHA256Bytes() returns raw SHA256 output (no reversal)
    // DoubleSHA256() reverses bytes then converts to hex for display
    // So raw bytes should be the REVERSE of display hex
    //
    // Display: 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
    // Raw:     5694c45d3f98413ef45cf54545538103cc9f298e0575820ad3591376e2e0f65d (reversed)
    //
    // But observed: raw = 5df6... which matches display! This means DoubleSHA256()
    // is reversing correctly, so raw must be in the order that when reversed = display

    // Actual observed behavior:
    // Raw bytes are in big-endian (same order as display hex before reversal)
    // DoubleSHA256() reverses them to little-endian, then ToHex displays that
    // But ToHex displays from index 0, so reversed little-endian = display big-endian

    // The key invariant: hash[0] is the first byte output by CSHA256::Finalize()
    ASSERT_EQ(hash[0], 0x5d);  // First byte of SHA256 output
    ASSERT_EQ(hash[31], 0x56); // Last byte of SHA256 output
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: DoubleSHA256 (Display Function) Reverses Bytes
// ═══════════════════════════════════════════════════════════════════════════

TEST(double_sha256_display_reverses_bytes) {
    // Test vector: empty input
    std::vector<uint8_t> empty;
    std::string hex = TransactionSerializer::DoubleSHA256(empty);

    // DoubleSHA256() reverses raw bytes for display
    // Raw:     5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
    // Display: 56944c5d3f98413ef45cf54545538103cc9f298e0575820ad3591376e2e0f65d (reversed)
    ASSERT_EQ("56944c5d3f98413ef45cf54545538103cc9f298e0575820ad3591376e2e0f65d", hex);

    // First hex pair is last byte of raw (0x56)
    ASSERT_EQ("56", hex.substr(0, 2));
    // Last hex pair is first byte of raw (0x5d)
    ASSERT_EQ("5d", hex.substr(62, 2));
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: DoubleSHA256Bytes and DoubleSHA256 Consistency
// ═══════════════════════════════════════════════════════════════════════════

TEST(double_sha256_bytes_and_hex_consistency) {
    std::vector<uint8_t> test_data = {0x01, 0x02, 0x03, 0x04};

    // Get raw bytes
    auto bytes = TransactionSerializer::DoubleSHA256Bytes(test_data);

    // Get display hex
    std::string hex = TransactionSerializer::DoubleSHA256(test_data);

    // Manually reverse bytes and convert to hex
    std::vector<uint8_t> reversed_bytes = bytes;
    std::reverse(reversed_bytes.begin(), reversed_bytes.end());
    std::string manual_hex = TransactionSerializer::ToHex(reversed_bytes);

    // DoubleSHA256() should equal manually reversed + ToHex
    ASSERT_EQ(hex, manual_hex);

    // Verify first byte of raw is last byte pair of hex
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(bytes[0]);
    ASSERT_EQ(hex.substr(62, 2), oss.str());

    // Verify last byte of raw is first byte pair of hex
    oss.str("");
    oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(bytes[31]);
    ASSERT_EQ(hex.substr(0, 2), oss.str());
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: No Double Reversal in Hash Chain
// ═══════════════════════════════════════════════════════════════════════════

TEST(no_double_reversal_in_hash_chain) {
    // This test ensures we never have double-reversal bugs
    std::vector<uint8_t> data = {0xde, 0xad, 0xbe, 0xef};

    // Path 1: DoubleSHA256Bytes → memcpy to uint256 → GetHex
    auto bytes1 = TransactionSerializer::DoubleSHA256Bytes(data);
    uint256 hash1;
    std::memcpy(hash1.data, bytes1.data(), 32);
    std::string hex1 = hash1.GetHex();

    // Path 2: DoubleSHA256 (direct hex)
    std::string hex2 = TransactionSerializer::DoubleSHA256(data);

    // Both paths must produce identical hex output
    ASSERT_EQ(hex1, hex2);

    // Verify internal bytes are little-endian
    ASSERT_EQ(hash1.data[0], bytes1[0]);   // LSB matches
    ASSERT_EQ(hash1.data[31], bytes1[31]); // MSB matches

    // Verify hex is big-endian (reversed)
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(bytes1[31]);
    ASSERT_EQ(hex1.substr(0, 2), oss.str()); // First hex pair is last byte
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Memcpy Preserves Identity (No Reversal)
// ═══════════════════════════════════════════════════════════════════════════

TEST(memcpy_preserves_identity) {
    // Create test hash
    std::vector<uint8_t> test_data = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
    auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(test_data);

    // Copy to uint256 using memcpy (what GetTxid does)
    uint256 hash;
    std::memcpy(hash.data, hash_bytes.data(), 32);

    // Verify every byte matches (no reversal)
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(hash.data[i], hash_bytes[i]);
    }

    // Verify LSB and MSB are in correct positions
    ASSERT_EQ(hash.data[0], hash_bytes[0]);   // LSB
    ASSERT_EQ(hash.data[31], hash_bytes[31]); // MSB
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "Phase M.0 Byte Order Invariant Test Suite\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

    std::cout << "If ANY test fails, Phase M.0 compliance is BROKEN.\n";
    std::cout << "This indicates a CONSENSUS-CRITICAL bug.\n\n";

    // Tests run automatically via static constructors

    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "✅ ALL TESTS PASSED - Phase M.0 Invariant LOCKED\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

    std::cout << "INVARIANT CONFIRMED:\n";
    std::cout << "  • uint256 is little-endian (internal identity)\n";
    std::cout << "  • GetHex() reverses to big-endian (display)\n";
    std::cout << "  • DoubleSHA256Bytes() returns raw bytes (little-endian)\n";
    std::cout << "  • DoubleSHA256() reverses for display (big-endian)\n";
    std::cout << "  • GetTxid()/GetWtxid() preserve identity (memcpy)\n\n";

    return 0;
}
