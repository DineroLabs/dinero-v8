/**
 * BIP32Deriver Equivalence Test
 *
 * CRITICAL: This test verifies that BIP32Deriver produces IDENTICAL keys
 * to the legacy inline lambdas used throughout hd_wallet.cpp.
 *
 * This is the "micro-test" safety check before refactoring lambda clusters.
 * If this test passes, BIP32Deriver is a drop-in replacement.
 *
 * Tests:
 * 1. BIP84 receive chain: m/84'/1447'/0'/0/index
 * 2. BIP84 change chain:  m/84'/1447'/0'/1/index
 * 3. BIP86 receive chain: m/86'/1447'/0'/0/index
 * 4. BIP86 change chain:  m/86'/1447'/0'/1/index
 * 5. Multiple indices (0-9) for each path
 */

#include "wallet/bip32_deriver.h"
#include "consensus/coin_type.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <gtest/gtest.h>
#include <cstring>
#include <array>
#include <iomanip>
#include <sstream>

namespace {

//=============================================================================
// Legacy Implementation (exact copy from hd_wallet.cpp for comparison)
//=============================================================================

static void HMAC512_Legacy(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out64[64]) {
    unsigned int len = 64;
    HMAC(EVP_sha512(), key, static_cast<int>(klen), msg, mlen, out64, &len);
}

static inline uint32_t U32BE_Legacy(uint32_t x) {
    return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) | ((x & 0xff00) << 8) | ((x & 0xff) << 24);
}

static inline uint32_t hardened_legacy(uint32_t i) {
    return 0x80000000u | i;
}

/**
 * Legacy derivation using the exact inline lambda pattern from hd_wallet.cpp.
 * Returns the final private key after derivation.
 */
std::array<uint8_t, 32> DeriveKeyLegacy(
    const uint8_t* seed,
    uint32_t purpose,      // 84 or 86
    uint32_t coin_type,    // 1447
    uint32_t account,      // 0
    uint32_t chain,        // 0=receive, 1=change, 2=mining
    uint32_t index
) {
    // BIP32 root from seed (exact copy from hd_wallet.cpp:450-455)
    uint8_t I[64];
    HMAC512_Legacy((const uint8_t*)"Bitcoin seed", 12, seed, 64, I);
    uint8_t k[32]; memcpy(k, I, 32);
    uint8_t c[32]; memcpy(c, I + 32, 32);

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Hardened derivation lambda (exact copy from hd_wallet.cpp:459-469)
    auto derive_hard = [&](uint32_t i) {
        uint8_t data[1 + 32 + 4]; data[0] = 0;
        memcpy(data + 1, k, 32);
        uint32_t be = U32BE_Legacy(hardened_legacy(i)); memcpy(data + 33, &be, 4);
        HMAC512_Legacy(c, 32, data, sizeof(data), I);
        uint8_t tweak[32]; memcpy(tweak, I, 32);
        if (!secp256k1_ec_seckey_verify(ctx, k)) throw std::runtime_error("bip32: bad key");
        if (!secp256k1_ec_seckey_tweak_add(ctx, k, tweak)) throw std::runtime_error("bip32: tweak add failed");
        memcpy(c, I + 32, 32);
    };

    // Derive m/purpose'/coin_type'/account'
    derive_hard(purpose);
    derive_hard(coin_type);
    derive_hard(account);

    // Normal derivation lambda (exact copy from hd_wallet.cpp:473-485)
    auto derive_norm = [&](uint32_t i) {
        secp256k1_pubkey P;
        if (!secp256k1_ec_pubkey_create(ctx, &P, k)) throw std::runtime_error("pubkey create failed");
        uint8_t ser[33]; size_t seclen = 33;
        secp256k1_ec_pubkey_serialize(ctx, ser, &seclen, &P, SECP256K1_EC_COMPRESSED);
        uint8_t data[33 + 4]; memcpy(data, ser, 33);
        uint32_t be = U32BE_Legacy(i); memcpy(data + 33, &be, 4);
        HMAC512_Legacy(c, 32, data, sizeof(data), I);
        uint8_t tweak[32]; memcpy(tweak, I, 32);
        if (!secp256k1_ec_seckey_tweak_add(ctx, k, tweak)) throw std::runtime_error("bip32: tweak add failed");
        memcpy(c, I + 32, 32);
    };

    // Derive /chain/index
    derive_norm(chain);
    derive_norm(index);

    // Copy result before cleanup
    std::array<uint8_t, 32> result;
    memcpy(result.data(), k, 32);

    // Cleanup (as in hd_wallet.cpp:497-502)
    OPENSSL_cleanse(k, sizeof(k));
    OPENSSL_cleanse(I, sizeof(I));
    OPENSSL_cleanse(c, sizeof(c));
    secp256k1_context_destroy(ctx);

    return result;
}

/**
 * Derivation using BIP32Deriver class
 */
std::array<uint8_t, 32> DeriveKeyNew(
    const uint8_t* seed,
    uint32_t purpose,
    uint32_t coin_type,
    uint32_t account,
    uint32_t chain,
    uint32_t index
) {
    dinero::BIP32Deriver deriver(seed, 64);

    // Derive m/purpose'/coin_type'/account'
    deriver.deriveHardened(purpose);
    deriver.deriveHardened(coin_type);
    deriver.deriveHardened(account);

    // Derive /chain/index
    deriver.deriveNormal(chain);
    deriver.deriveNormal(index);

    return deriver.getPrivateKey();
}

// Test fixture with known seed
class BIP32DeriverEquivalenceTest : public ::testing::Test {
protected:
    // BIP39 test vector seed (from "abandon abandon ... about" mnemonic)
    static constexpr uint8_t TEST_SEED[64] = {
        0x5e, 0xb0, 0x0b, 0xbd, 0xdc, 0xf0, 0x69, 0x08,
        0x48, 0x89, 0xa8, 0xab, 0x91, 0x55, 0x56, 0x81,
        0x65, 0xf5, 0xc4, 0x53, 0xcc, 0xb8, 0x5e, 0x70,
        0x81, 0x1a, 0xae, 0xd6, 0xf6, 0xda, 0x5f, 0xc1,
        0x9a, 0x5a, 0xc4, 0x0b, 0x38, 0x9c, 0xd3, 0x70,
        0xd0, 0x86, 0x20, 0x6d, 0xec, 0x8a, 0xa6, 0xc4,
        0x3d, 0xae, 0xa6, 0x69, 0x0f, 0x20, 0xad, 0x3d,
        0x8d, 0x48, 0xb2, 0xd2, 0xce, 0x9e, 0x38, 0xe4
    };

    static constexpr uint32_t COIN_TYPE = 1447;

    std::string ToHex(const std::array<uint8_t, 32>& data) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (uint8_t byte : data) {
            ss << std::setw(2) << static_cast<int>(byte);
        }
        return ss.str();
    }
};

// Static member definition
constexpr uint8_t BIP32DeriverEquivalenceTest::TEST_SEED[];

//=============================================================================
// Tests
//=============================================================================

TEST_F(BIP32DeriverEquivalenceTest, BIP84_Receive_MultipleIndices) {
    // BIP84 receive chain: m/84'/1447'/0'/0/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 84, COIN_TYPE, 0, 0, index);
        auto modern = DeriveKeyNew(TEST_SEED, 84, COIN_TYPE, 0, 0, index);

        EXPECT_EQ(legacy, modern)
            << "BIP84 receive path m/84'/1447'/0'/0/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, BIP84_Change_MultipleIndices) {
    // BIP84 change chain: m/84'/1447'/0'/1/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 84, COIN_TYPE, 0, 1, index);
        auto modern = DeriveKeyNew(TEST_SEED, 84, COIN_TYPE, 0, 1, index);

        EXPECT_EQ(legacy, modern)
            << "BIP84 change path m/84'/1447'/0'/1/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, BIP84_Mining_MultipleIndices) {
    // BIP84 mining chain: m/84'/1447'/0'/2/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 84, COIN_TYPE, 0, 2, index);
        auto modern = DeriveKeyNew(TEST_SEED, 84, COIN_TYPE, 0, 2, index);

        EXPECT_EQ(legacy, modern)
            << "BIP84 mining path m/84'/1447'/0'/2/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, BIP86_Receive_MultipleIndices) {
    // BIP86 receive chain: m/86'/1447'/0'/0/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 86, COIN_TYPE, 0, 0, index);
        auto modern = DeriveKeyNew(TEST_SEED, 86, COIN_TYPE, 0, 0, index);

        EXPECT_EQ(legacy, modern)
            << "BIP86 receive path m/86'/1447'/0'/0/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, BIP86_Change_MultipleIndices) {
    // BIP86 change chain: m/86'/1447'/0'/1/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 86, COIN_TYPE, 0, 1, index);
        auto modern = DeriveKeyNew(TEST_SEED, 86, COIN_TYPE, 0, 1, index);

        EXPECT_EQ(legacy, modern)
            << "BIP86 change path m/86'/1447'/0'/1/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, BIP86_Mining_MultipleIndices) {
    // BIP86 mining chain: m/86'/1447'/0'/2/index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 86, COIN_TYPE, 0, 2, index);
        auto modern = DeriveKeyNew(TEST_SEED, 86, COIN_TYPE, 0, 2, index);

        EXPECT_EQ(legacy, modern)
            << "BIP86 mining path m/86'/1447'/0'/2/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, Lightning_Funding_MultipleIndices) {
    // Lightning funding chain: m/84'/1447'/0'/3/channel_index
    for (uint32_t index = 0; index < 10; ++index) {
        auto legacy = DeriveKeyLegacy(TEST_SEED, 84, COIN_TYPE, 0, 3, index);
        auto modern = DeriveKeyNew(TEST_SEED, 84, COIN_TYPE, 0, 3, index);

        EXPECT_EQ(legacy, modern)
            << "Lightning funding path m/84'/1447'/0'/3/" << index << " mismatch!\n"
            << "  Legacy: " << ToHex(legacy) << "\n"
            << "  Modern: " << ToHex(modern);
    }
}

TEST_F(BIP32DeriverEquivalenceTest, CompressedPubkey_MatchesLegacy) {
    // Verify getCompressedPubkey() produces correct pubkey
    dinero::BIP32Deriver deriver(TEST_SEED, 64);
    deriver.deriveHardened(86);
    deriver.deriveHardened(COIN_TYPE);
    deriver.deriveHardened(0);
    deriver.deriveNormal(0);
    deriver.deriveNormal(0);

    auto privkey = deriver.getPrivateKey();
    auto pubkey = deriver.getCompressedPubkey();

    // Verify pubkey is derived from privkey
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_pubkey P;
    ASSERT_TRUE(secp256k1_ec_pubkey_create(ctx, &P, privkey.data()));

    uint8_t expected_pub[33];
    size_t len = 33;
    secp256k1_ec_pubkey_serialize(ctx, expected_pub, &len, &P, SECP256K1_EC_COMPRESSED);

    EXPECT_EQ(memcmp(pubkey.data(), expected_pub, 33), 0)
        << "getCompressedPubkey() doesn't match direct computation";

    secp256k1_context_destroy(ctx);
}

TEST_F(BIP32DeriverEquivalenceTest, XOnlyPubkey_MatchesLegacy) {
    // Verify getXOnlyPubkey() produces correct x-only pubkey for Taproot
    dinero::BIP32Deriver deriver(TEST_SEED, 64);
    deriver.deriveHardened(86);
    deriver.deriveHardened(COIN_TYPE);
    deriver.deriveHardened(0);
    deriver.deriveNormal(0);
    deriver.deriveNormal(0);

    auto privkey = deriver.getPrivateKey();
    auto xonly = deriver.getXOnlyPubkey();

    // Verify x-only pubkey matches
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_pubkey P;
    ASSERT_TRUE(secp256k1_ec_pubkey_create(ctx, &P, privkey.data()));

    secp256k1_xonly_pubkey xonly_expected;
    ASSERT_TRUE(secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_expected, nullptr, &P));

    uint8_t expected_bytes[32];
    secp256k1_xonly_pubkey_serialize(ctx, expected_bytes, &xonly_expected);

    EXPECT_EQ(memcmp(xonly.data(), expected_bytes, 32), 0)
        << "getXOnlyPubkey() doesn't match direct computation";

    secp256k1_context_destroy(ctx);
}

TEST_F(BIP32DeriverEquivalenceTest, MoveSemantics_PreserveKeys) {
    // Test that move semantics work correctly
    dinero::BIP32Deriver deriver1(TEST_SEED, 64);
    deriver1.deriveHardened(86);
    deriver1.deriveHardened(COIN_TYPE);
    deriver1.deriveHardened(0);

    auto key_before_move = deriver1.getPrivateKey();

    // Move construct
    dinero::BIP32Deriver deriver2(std::move(deriver1));

    auto key_after_move = deriver2.getPrivateKey();
    EXPECT_EQ(key_before_move, key_after_move) << "Move construction should preserve key";

    // Move assign
    dinero::BIP32Deriver deriver3(TEST_SEED, 64);
    deriver3 = std::move(deriver2);

    auto key_after_assign = deriver3.getPrivateKey();
    EXPECT_EQ(key_before_move, key_after_assign) << "Move assignment should preserve key";
}

} // anonymous namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
