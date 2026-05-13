/**
 * SHA256 Gadget Tests
 *
 * Verifies the R1CS SHA256 circuit against known NIST test vectors.
 * Each test builds the circuit, sets witness values, and checks:
 *   1. The output matches the expected hash
 *   2. All R1CS constraints are satisfied
 */

#include <gtest/gtest.h>
#include "zk/zkvm/sha256_gadget.h"
#include <openssl/sha.h>
#include <cstring>

using namespace dinero::zk::zkvm;

class SHA256GadgetTest : public ::testing::Test {
protected:
    // Convert a Word32 array (8 words) to 32-byte hash (big-endian)
    std::vector<uint8_t> hash_to_bytes(R1CS& cs, const std::array<Word32, 8>& h) {
        std::vector<uint8_t> out(32);
        for (unsigned w = 0; w < 8; ++w) {
            uint32_t val = 0;
            for (unsigned i = 0; i < 32; ++i) {
                if (!cs.get_value(h[w].bits[i]).is_zero())
                    val |= (1u << i);
            }
            // Big-endian byte order
            out[w*4 + 0] = (val >> 24) & 0xFF;
            out[w*4 + 1] = (val >> 16) & 0xFF;
            out[w*4 + 2] = (val >> 8) & 0xFF;
            out[w*4 + 3] = val & 0xFF;
        }
        return out;
    }

    // Allocate a message as Word32 bytes in the circuit
    std::vector<Word32> alloc_message(R1CS& cs, const uint8_t* data, size_t len) {
        std::vector<Word32> bytes;
        bytes.reserve(len);
        Variable zv = gadgets::constant(cs, Scalar::zero(), "mz");
        for (size_t i = 0; i < len; ++i) {
            Word32 w;
            for (unsigned b = 0; b < 32; ++b) {
                if (b < 8) {
                    uint32_t bit = (data[i] >> b) & 1;
                    w.bits[b] = cs.alloc(Scalar(static_cast<uint64_t>(bit)));
                    gadgets::enforce_boolean(cs, w.bits[b],
                        "m" + std::to_string(i) + "_" + std::to_string(b));
                } else {
                    w.bits[b] = zv;
                }
            }
            bytes.push_back(w);
        }
        return bytes;
    }

    // Compute reference SHA256 using OpenSSL
    std::vector<uint8_t> ref_sha256(const uint8_t* data, size_t len) {
        std::vector<uint8_t> hash(32);
        SHA256(data, len, hash.data());
        return hash;
    }

    std::vector<uint8_t> ref_double_sha256(const uint8_t* data, size_t len) {
        uint8_t inner[32];
        SHA256(data, len, inner);
        std::vector<uint8_t> hash(32);
        SHA256(inner, 32, hash.data());
        return hash;
    }
};

// Test: Word32 alloc and pack round-trip
TEST_F(SHA256GadgetTest, Word32_AllocPack) {
    R1CS cs;
    Word32 w = word32_alloc(cs, 0xDEADBEEF, "test");
    // Verify the bits
    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!cs.get_value(w.bits[i]).is_zero()) val |= (1u << i);
    }
    EXPECT_EQ(val, 0xDEADBEEFu);
    EXPECT_TRUE(cs.is_satisfied());
}

// Test: Word32 addition mod 2^32
TEST_F(SHA256GadgetTest, Word32_Add) {
    R1CS cs;
    Word32 a = word32_alloc(cs, 0xFFFFFFFF, "a");
    Word32 b = word32_alloc(cs, 1, "b");
    Word32 sum = word32_add(cs, a, b, "sum");

    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!cs.get_value(sum.bits[i]).is_zero()) val |= (1u << i);
    }
    EXPECT_EQ(val, 0u);  // 0xFFFFFFFF + 1 = 0 (mod 2^32)
    EXPECT_TRUE(cs.is_satisfied());
}

TEST_F(SHA256GadgetTest, Word32_Add_Normal) {
    R1CS cs;
    Word32 a = word32_alloc(cs, 100, "a");
    Word32 b = word32_alloc(cs, 200, "b");
    Word32 sum = word32_add(cs, a, b, "sum");

    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!cs.get_value(sum.bits[i]).is_zero()) val |= (1u << i);
    }
    EXPECT_EQ(val, 300u);
    EXPECT_TRUE(cs.is_satisfied());
}

// Regression: local carry-out is not the same as the next bit of the final sum.
// 3 + 1 = 4, but bit 0 still produces a carry.
TEST_F(SHA256GadgetTest, Word32_Add_CarryChainRegression) {
    R1CS cs;
    Word32 a = word32_alloc(cs, 3, "a");
    Word32 b = word32_alloc(cs, 1, "b");
    Word32 sum = word32_add(cs, a, b, "sum");

    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!cs.get_value(sum.bits[i]).is_zero()) val |= (1u << i);
    }
    EXPECT_EQ(val, 4u);
    EXPECT_TRUE(cs.is_satisfied());
}

// Test: Word32 XOR
TEST_F(SHA256GadgetTest, Word32_Xor) {
    R1CS cs;
    Word32 a = word32_alloc(cs, 0xFF00FF00, "a");
    Word32 b = word32_alloc(cs, 0x0F0F0F0F, "b");
    Word32 x = word32_xor(cs, a, b, "xor");

    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!cs.get_value(x.bits[i]).is_zero()) val |= (1u << i);
    }
    EXPECT_EQ(val, 0xF00FF00Fu);
    EXPECT_TRUE(cs.is_satisfied());
}

// Test: SHA256("") — empty string
TEST_F(SHA256GadgetTest, SHA256_Empty) {
    R1CS cs;
    std::vector<Word32> empty_msg;
    auto hash = sha256_full(cs, empty_msg);

    auto circuit_hash = hash_to_bytes(cs, hash);
    auto expected = ref_sha256(nullptr, 0);

    EXPECT_EQ(circuit_hash, expected);
    EXPECT_TRUE(cs.is_satisfied());

    // Report constraint count
    std::cout << "  SHA256(\"\") constraints: " << cs.num_constraints() << std::endl;
}

// Test: SHA256("abc") — NIST test vector
TEST_F(SHA256GadgetTest, SHA256_abc) {
    R1CS cs;
    const uint8_t msg[] = {'a', 'b', 'c'};
    auto message = alloc_message(cs, msg, 3);
    auto hash = sha256_full(cs, message);

    auto circuit_hash = hash_to_bytes(cs, hash);
    auto expected = ref_sha256(msg, 3);

    EXPECT_EQ(circuit_hash, expected);
    EXPECT_TRUE(cs.is_satisfied());

    std::cout << "  SHA256(\"abc\") constraints: " << cs.num_constraints() << std::endl;
}

// Test: SHA256 of 55 bytes (fits in one block with padding)
TEST_F(SHA256GadgetTest, SHA256_55bytes) {
    R1CS cs;
    uint8_t msg[55];
    memset(msg, 0x41, 55);  // 55 'A's
    auto message = alloc_message(cs, msg, 55);
    auto hash = sha256_full(cs, message);

    auto circuit_hash = hash_to_bytes(cs, hash);
    auto expected = ref_sha256(msg, 55);

    EXPECT_EQ(circuit_hash, expected);
    EXPECT_TRUE(cs.is_satisfied());

    std::cout << "  SHA256(55 bytes) constraints: " << cs.num_constraints() << std::endl;
}

// Test: SHA256 of 56 bytes (boundary: needs two blocks)
TEST_F(SHA256GadgetTest, SHA256_56bytes) {
    R1CS cs;
    uint8_t msg[56];
    memset(msg, 0x42, 56);
    auto message = alloc_message(cs, msg, 56);
    auto hash = sha256_full(cs, message);

    auto circuit_hash = hash_to_bytes(cs, hash);
    auto expected = ref_sha256(msg, 56);

    EXPECT_EQ(circuit_hash, expected);
    EXPECT_TRUE(cs.is_satisfied());

    std::cout << "  SHA256(56 bytes) constraints: " << cs.num_constraints()
              << " (two blocks)" << std::endl;
}

// Test: Double SHA256("abc")
TEST_F(SHA256GadgetTest, DoubleSHA256_abc) {
    R1CS cs;
    const uint8_t msg[] = {'a', 'b', 'c'};
    auto message = alloc_message(cs, msg, 3);
    auto hash = double_sha256(cs, message);

    auto circuit_hash = hash_to_bytes(cs, hash);
    auto expected = ref_double_sha256(msg, 3);

    EXPECT_EQ(circuit_hash, expected);
    EXPECT_TRUE(cs.is_satisfied());

    std::cout << "  DoubleSHA256(\"abc\") constraints: " << cs.num_constraints() << std::endl;
}
