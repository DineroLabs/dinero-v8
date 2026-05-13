/**
 * Phase 11e.1: Bitcoin Magic Translation Tests
 *
 * Tests for witness commitment magic translation switch (OFF by default).
 *
 * What this tests:
 * - Translation switch correctly interprets DINW as Bitcoin magic when enabled
 * - Translation respects height gating
 * - Translation only affects DINW commitments with correct version
 * - Translation is OFF by default on all networks
 *
 * What this does NOT test:
 * - Block validation (not changing blocks, only interpretation)
 * - Mining behavior (mining still creates DINW)
 * - Bitcoin SegWit activation (NOT what this is)
 */

#include <gtest/gtest.h>
#include "consensus/witness_commitment.h"
#include "consensus/chainparams.h"

using namespace dinero::consensus;

class WitnessMagicTranslationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // No setup needed - TranslateWitnessMagic is pure function
    }
};

/**
 * Test 1: Translation OFF → Returns Original Magic
 *
 * When translation is disabled (default), the function should always return
 * the original magic bytes unchanged, regardless of height or magic type.
 */
TEST_F(WitnessMagicTranslationTest, TranslationOff_ReturnsOriginalMagic) {
    // Translation OFF, height 100, DINW magic
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW (0x444E5257)
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        false,                             // enable_translation = false
        50                                 // translation_height = 50
    );

    // Should return original DINW magic (no translation)
    EXPECT_EQ(result, WitnessCommitment::MAGIC);
    EXPECT_NE(result, WitnessCommitment::BITCOIN_MAGIC);
}

/**
 * Test 2: Translation ON + Below Height → Returns Original Magic
 *
 * When translation is enabled but current height is below the activation height,
 * the function should return the original magic (translation not active yet).
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_BelowHeight_ReturnsOriginalMagic) {
    // Translation ON, but height 99 < translation_height 100
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        WitnessCommitment::VERSION,        // version = 0x01
        99,                                // height = 99 (below threshold)
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should return original DINW magic (before activation height)
    EXPECT_EQ(result, WitnessCommitment::MAGIC);
    EXPECT_NE(result, WitnessCommitment::BITCOIN_MAGIC);
}

/**
 * Test 3: Translation ON + At Height + DINW → Returns Bitcoin Magic
 *
 * This is the core translation behavior: when enabled and at/after activation height,
 * DINW commitments with version 0x01 are interpreted as Bitcoin magic.
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_AtHeight_DINW_ReturnsBitcoinMagic) {
    // Translation ON, height 100 >= translation_height 100, DINW magic
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW (0x444E5257)
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100 (at activation)
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should translate DINW → Bitcoin magic
    EXPECT_EQ(result, WitnessCommitment::BITCOIN_MAGIC);
    EXPECT_NE(result, WitnessCommitment::MAGIC);
}

/**
 * Test 4: Translation ON + Above Height + DINW → Returns Bitcoin Magic
 *
 * Verify translation continues to work after activation height.
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_AboveHeight_DINW_ReturnsBitcoinMagic) {
    // Translation ON, height 200 > translation_height 100, DINW magic
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        WitnessCommitment::VERSION,        // version = 0x01
        200,                               // height = 200 (well after activation)
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should translate DINW → Bitcoin magic
    EXPECT_EQ(result, WitnessCommitment::BITCOIN_MAGIC);
}

/**
 * Test 5: Translation ON + Non-DINW Magic → Returns Original
 *
 * Translation only applies to DINW commitments. Other magic values should
 * pass through unchanged (e.g., if Bitcoin magic is already present).
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_NonDINWMagic_ReturnsOriginal) {
    // Translation ON, height active, but magic is already Bitcoin magic
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::BITCOIN_MAGIC,  // magic = Bitcoin (0xaa21a9ed)
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should return original Bitcoin magic (no translation needed)
    EXPECT_EQ(result, WitnessCommitment::BITCOIN_MAGIC);

    // Test with random magic
    uint32_t random_result = TranslateWitnessMagic(
        0xDEADBEEF,                        // magic = random
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should return original random magic (not DINW, so no translation)
    EXPECT_EQ(random_result, 0xDEADBEEF);
}

/**
 * Test 6: Translation ON + Wrong Version → Returns Original
 *
 * Translation only applies to version 0x01 commitments. Other versions should
 * not be translated (future version compatibility).
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_WrongVersion_ReturnsOriginal) {
    // Translation ON, height active, DINW magic, but version is 0x02
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        0x02,                              // version = 0x02 (future version)
        100,                               // height = 100
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should return original DINW magic (wrong version, no translation)
    EXPECT_EQ(result, WitnessCommitment::MAGIC);
    EXPECT_NE(result, WitnessCommitment::BITCOIN_MAGIC);

    // Test with version 0x00 (hypothetical old version)
    uint32_t result_v0 = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        0x00,                              // version = 0x00
        100,                               // height = 100
        true,                              // enable_translation = true
        100                                // translation_height = 100
    );

    // Should return original DINW magic (wrong version)
    EXPECT_EQ(result_v0, WitnessCommitment::MAGIC);
}

/**
 * Test 7: Default Network Parameters → Translation OFF
 *
 * Verify that all networks (mainnet, testnet, regtest) have translation
 * disabled by default. This is a critical safety check.
 */
TEST_F(WitnessMagicTranslationTest, DefaultNetworkParameters_TranslationOff) {
    // Test mainnet
    dinero::SelectParams(dinero::Chain::MAINNET);
    const auto& mainnet = dinero::Params();
    EXPECT_FALSE(mainnet.enable_witness_magic_translation)
        << "Mainnet should have translation OFF by default";
    EXPECT_EQ(mainnet.witness_magic_translation_height, UINT32_MAX)
        << "Mainnet translation height should be UINT32_MAX (never)";

    // Test testnet
    dinero::SelectParams(dinero::Chain::TESTNET);
    const auto& testnet = dinero::Params();
    EXPECT_FALSE(testnet.enable_witness_magic_translation)
        << "Testnet should have translation OFF by default";
    EXPECT_EQ(testnet.witness_magic_translation_height, UINT32_MAX)
        << "Testnet translation height should be UINT32_MAX (never)";

    // Test regtest
    dinero::SelectParams(dinero::Chain::REGTEST);
    const auto& regtest = dinero::Params();
    EXPECT_FALSE(regtest.enable_witness_magic_translation)
        << "Regtest should have translation OFF by default";
    EXPECT_EQ(regtest.witness_magic_translation_height, UINT32_MAX)
        << "Regtest translation height should be UINT32_MAX (configurable)";
}

/**
 * Test 8: Edge Case - Height Exactly at Activation
 *
 * Verify that translation activates exactly at the specified height (not after).
 */
TEST_F(WitnessMagicTranslationTest, TranslationOn_ExactHeight_Translates) {
    uint32_t activation_height = 12345;

    // Height exactly at activation
    uint32_t result = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        WitnessCommitment::VERSION,        // version = 0x01
        activation_height,                 // height = activation_height (exact)
        true,                              // enable_translation = true
        activation_height                  // translation_height = same
    );

    // Should translate (>= comparison)
    EXPECT_EQ(result, WitnessCommitment::BITCOIN_MAGIC);

    // Height one block before activation
    uint32_t result_before = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        WitnessCommitment::VERSION,        // version = 0x01
        activation_height - 1,             // height = activation_height - 1
        true,                              // enable_translation = true
        activation_height                  // translation_height
    );

    // Should NOT translate (before activation)
    EXPECT_EQ(result_before, WitnessCommitment::MAGIC);
}

/**
 * Test 9: Multiple Translations → Idempotent
 *
 * Verify that translating already-translated magic doesn't cause issues.
 */
TEST_F(WitnessMagicTranslationTest, MultipleTranslations_Idempotent) {
    // First translation: DINW → Bitcoin
    uint32_t first = TranslateWitnessMagic(
        WitnessCommitment::MAGIC,          // magic = DINW
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        true,                              // enable_translation = true
        50                                 // translation_height = 50
    );
    EXPECT_EQ(first, WitnessCommitment::BITCOIN_MAGIC);

    // Second translation: Bitcoin → Bitcoin (should be unchanged)
    uint32_t second = TranslateWitnessMagic(
        first,                             // magic = Bitcoin (from first translation)
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        true,                              // enable_translation = true
        50                                 // translation_height = 50
    );
    EXPECT_EQ(second, WitnessCommitment::BITCOIN_MAGIC);

    // Third translation: same result
    uint32_t third = TranslateWitnessMagic(
        second,                            // magic = Bitcoin
        WitnessCommitment::VERSION,        // version = 0x01
        100,                               // height = 100
        true,                              // enable_translation = true
        50                                 // translation_height = 50
    );
    EXPECT_EQ(third, WitnessCommitment::BITCOIN_MAGIC);
}

/**
 * Test 10: Translation Logic Matches Specification
 *
 * Verify the exact translation rule from Phase 11e design:
 * "If enabled AND height >= threshold AND magic == DINW AND version == 0x01,
 *  then return Bitcoin magic; otherwise return original."
 */
TEST_F(WitnessMagicTranslationTest, TranslationLogic_MatchesSpecification) {
    // All conditions met → translate
    EXPECT_EQ(
        TranslateWitnessMagic(0x444E5257, 0x01, 100, true, 100),
        WitnessCommitment::BITCOIN_MAGIC
    );

    // Enabled=false → no translation
    EXPECT_EQ(
        TranslateWitnessMagic(0x444E5257, 0x01, 100, false, 100),
        0x444E5257
    );

    // Height too low → no translation
    EXPECT_EQ(
        TranslateWitnessMagic(0x444E5257, 0x01, 99, true, 100),
        0x444E5257
    );

    // Wrong magic → no translation
    EXPECT_EQ(
        TranslateWitnessMagic(0xDEADBEEF, 0x01, 100, true, 100),
        0xDEADBEEF
    );

    // Wrong version → no translation
    EXPECT_EQ(
        TranslateWitnessMagic(0x444E5257, 0x02, 100, true, 100),
        0x444E5257
    );
}
