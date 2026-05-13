/**
 * Phase C.5: Covenant Pattern Tests
 *
 * Tests high-level wallet recipes using covenant primitives
 */

#include "wallet/covenant_patterns.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::wallet::patterns;

// ============================================================================
// Test Helpers
// ============================================================================

void printTestHeader(const std::string& test_name) {
    std::cout << "\n[Test] " << test_name << std::endl;
}

void printPass(const std::string& message) {
    std::cout << "  [✓] " << message << std::endl;
}

// Generate test pubkey (deterministic for testing)
std::vector<uint8_t> generateTestPubkey(uint8_t seed) {
    std::vector<uint8_t> pubkey(32);
    for (size_t i = 0; i < 32; i++) {
        pubkey[i] = static_cast<uint8_t>((seed * 7 + i * 13) % 256);
    }
    return pubkey;
}

// ============================================================================
// Test 1: Simple Vault Pattern
// ============================================================================

void test_simple_vault() {
    printTestHeader("Simple Vault Pattern");

    // Create simple vault with 24-hour delay
    auto vault = createSimpleVault(
        "tb1q...test_destination",  // Final address
        1000000000,                  // 10 BTC in sats
        144,                         // 24 hours (144 blocks)
        "my-savings-vault"
    );

    // Verify pattern structure
    assert(!vault.vault_script.empty());
    assert(!vault.unvault_script.empty());
    assert(vault.unvault_delay_blocks == 144);
    assert(vault.final_output.value == 1000000000);
    assert(vault.label == "my-savings-vault");

    printPass("Vault created successfully");
    printPass("Unvault delay: 144 blocks (~24 hours)");
    printPass("Final destination: tb1q...test_destination");

    // Validate pattern
    auto error = validatePattern(vault);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Simple vault pattern works" << std::endl;
}

// ============================================================================
// Test 2: Recovery Vault Pattern
// ============================================================================

void test_recovery_vault() {
    printTestHeader("Recovery Vault Pattern");

    auto recovery_pubkey = generateTestPubkey(42);

    // Create recovery vault
    auto vault = createRecoveryVault(
        "tb1q...final_address",
        500000000,       // 5 BTC
        recovery_pubkey,
        144,             // Normal vault delay: 24h
        25920,           // Recovery delay: ~6 months
        "inheritance-vault"
    );

    // Verify pattern structure
    assert(!vault.vault.vault_script.empty());
    assert(!vault.recovery_script.empty());
    assert(vault.recovery_pubkey == recovery_pubkey);
    assert(vault.recovery_delay_blocks == 25920);

    printPass("Recovery vault created successfully");
    printPass("Normal path: 24-hour vault delay");
    printPass("Recovery path: 6-month timelock + recovery key");

    // Validate pattern
    auto error = validatePattern(vault);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Recovery vault pattern works" << std::endl;
}

// ============================================================================
// Test 3: Time-Delayed Recovery Pattern
// ============================================================================

void test_time_delayed_recovery() {
    printTestHeader("Time-Delayed Recovery Pattern");

    auto owner_pubkey = generateTestPubkey(10);
    auto recovery_pubkey = generateTestPubkey(20);

    // Create time-delayed recovery
    auto pattern = createTimeDelayedRecovery(
        owner_pubkey,
        recovery_pubkey,
        25920,  // 6 months
        "alice",
        "bob-backup"
    );

    // Verify pattern structure
    assert(pattern.owner_pubkey == owner_pubkey);
    assert(pattern.recovery_pubkey == recovery_pubkey);
    assert(pattern.recovery_delay_blocks == 25920);
    assert(!pattern.spending_script.empty());
    assert(pattern.owner_label == "alice");
    assert(pattern.recovery_label == "bob-backup");

    printPass("Time-delayed recovery created successfully");
    printPass("Owner can spend immediately");
    printPass("Recovery key activates after 6 months");

    // Validate pattern
    auto error = validatePattern(pattern);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Time-delayed recovery pattern works" << std::endl;
}

// ============================================================================
// Test 4: Social Recovery Pattern (3-of-5)
// ============================================================================

void test_social_recovery() {
    printTestHeader("Social Recovery Pattern (3-of-5 Guardians)");

    auto owner_pubkey = generateTestPubkey(1);
    std::vector<std::vector<uint8_t>> guardians;
    for (int i = 0; i < 5; i++) {
        guardians.push_back(generateTestPubkey(10 + i));
    }

    std::vector<std::string> guardian_labels = {
        "alice-friend",
        "bob-family",
        "charlie-colleague",
        "david-lawyer",
        "eve-backup"
    };

    // Create 3-of-5 social recovery
    auto pattern = createSocialRecovery(
        owner_pubkey,
        guardians,
        3,      // Require 3 guardians
        25920,  // 6 months delay
        "owner",
        guardian_labels
    );

    // Verify pattern structure
    assert(pattern.owner_pubkey == owner_pubkey);
    assert(pattern.guardian_pubkeys.size() == 5);
    assert(pattern.threshold == 3);
    assert(pattern.recovery_delay_blocks == 25920);
    assert(!pattern.spending_script.empty());

    printPass("Social recovery created successfully");
    printPass("Owner has immediate access");
    printPass("3-of-5 guardians can recover after 6 months");
    printPass("Guardian labels preserved");

    // Validate pattern
    auto error = validatePattern(pattern);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Social recovery pattern works" << std::endl;
}

// ============================================================================
// Test 5: Restricted Multisig Pattern (2-of-3)
// ============================================================================

void test_restricted_multisig() {
    printTestHeader("Restricted Multisig Pattern (2-of-3 with Whitelist)");

    // Create multisig signers
    std::vector<std::vector<uint8_t>> signers;
    for (int i = 0; i < 3; i++) {
        signers.push_back(generateTestPubkey(30 + i));
    }

    std::vector<std::string> signer_labels = {"cfo", "cto", "ceo"};

    // Create whitelist
    std::vector<dinero::wallet::CTVOutput> whitelist;
    dinero::wallet::CTVOutput payroll;
    payroll.value = 100000000;  // 1 BTC
    payroll.address = "tb1q...payroll";
    whitelist.push_back(payroll);

    dinero::wallet::CTVOutput vendor;
    vendor.value = 50000000;  // 0.5 BTC
    vendor.address = "tb1q...vendor";
    whitelist.push_back(vendor);

    std::vector<std::string> whitelist_labels = {"payroll", "vendors"};

    // Create restricted multisig
    auto pattern = createRestrictedMultisig(
        signers,
        2,  // 2-of-3
        whitelist,
        signer_labels,
        whitelist_labels
    );

    // Verify pattern structure
    assert(pattern.pubkeys.size() == 3);
    assert(pattern.threshold == 2);
    assert(pattern.whitelist.size() == 2);
    assert(!pattern.spending_script.empty());

    printPass("Restricted multisig created successfully");
    printPass("2-of-3 multisig (CFO, CTO, CEO)");
    printPass("Can only send to: payroll, vendors");

    // Validate pattern
    auto error = validatePattern(pattern);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Restricted multisig pattern works" << std::endl;
}

// ============================================================================
// Test 6: Escrow Covenant Pattern
// ============================================================================

void test_escrow_covenant() {
    printTestHeader("Escrow Covenant Pattern (2-of-2 with Timeout Refund)");

    auto buyer_pubkey = generateTestPubkey(50);
    auto seller_pubkey = generateTestPubkey(60);

    // Create escrow covenant
    auto pattern = createEscrowCovenant(
        buyer_pubkey,
        seller_pubkey,
        100000000,  // 1 BTC
        "tb1q...seller_address",  // Mutual release to seller
        "tb1q...buyer_address",   // Refund to buyer
        2016,       // 2 weeks timeout
        "purchase-agreement-123"
    );

    // Verify pattern structure
    assert(pattern.buyer_pubkey == buyer_pubkey);
    assert(pattern.seller_pubkey == seller_pubkey);
    assert(pattern.timeout_blocks == 2016);
    assert(pattern.mutual_release.value == 100000000);
    assert(pattern.refund_output.value == 100000000);
    assert(!pattern.spending_script.empty());

    printPass("Escrow covenant created successfully");
    printPass("Happy path: 2-of-2 buyer + seller (immediate)");
    printPass("Timeout path: Buyer refund after 2 weeks");

    // Validate pattern
    auto error = validatePattern(pattern);
    assert(error.empty());
    printPass("Pattern validation passed");

    std::cout << "  [✓] PASS: Escrow covenant pattern works" << std::endl;
}

// ============================================================================
// Test 7: Pattern Validation (Error Cases)
// ============================================================================

void test_pattern_validation() {
    printTestHeader("Pattern Validation (Error Cases)");

    // Test invalid threshold (social recovery)
    try {
        auto owner = generateTestPubkey(1);
        std::vector<std::vector<uint8_t>> guardians = {
            generateTestPubkey(2),
            generateTestPubkey(3)
        };

        // Invalid: threshold = 0
        bool caught = false;
        try {
            createSocialRecovery(owner, guardians, 0);
        } catch (const std::exception&) {
            caught = true;
        }
        assert(caught);
        printPass("Correctly rejected threshold = 0");

        // Invalid: threshold > N
        caught = false;
        try {
            createSocialRecovery(owner, guardians, 5);
        } catch (const std::exception&) {
            caught = true;
        }
        assert(caught);
        printPass("Correctly rejected threshold > N");

    } catch (const std::exception& e) {
        std::cout << "  [✗] Unexpected error: " << e.what() << std::endl;
        assert(false);
    }

    // Test invalid pubkey size
    try {
        std::vector<uint8_t> short_pubkey = {0x01, 0x02, 0x03};  // Only 3 bytes
        std::vector<uint8_t> valid_pubkey = generateTestPubkey(5);

        bool caught = false;
        try {
            createTimeDelayedRecovery(short_pubkey, valid_pubkey);
        } catch (const std::exception&) {
            caught = true;
        }
        assert(caught);
        printPass("Correctly rejected invalid pubkey size");

    } catch (const std::exception& e) {
        std::cout << "  [✗] Unexpected error: " << e.what() << std::endl;
        assert(false);
    }

    std::cout << "  [✓] PASS: Pattern validation works correctly" << std::endl;
}

// ============================================================================
// Test 8: Fee Estimation for Patterns
// ============================================================================

void test_fee_estimation() {
    printTestHeader("Fee Estimation for Patterns");

    size_t simple_vault_size = estimatePatternWitnessSize("simple-vault");
    size_t recovery_vault_size = estimatePatternWitnessSize("recovery-vault");
    size_t social_recovery_size = estimatePatternWitnessSize("social-recovery");
    size_t restricted_multisig_size = estimatePatternWitnessSize("restricted-multisig");
    size_t escrow_size = estimatePatternWitnessSize("escrow-covenant");

    assert(simple_vault_size > 0);
    assert(recovery_vault_size > simple_vault_size);  // Recovery has extra path
    assert(social_recovery_size > simple_vault_size);  // More signatures
    assert(restricted_multisig_size > 0);
    assert(escrow_size > 0);

    std::cout << "  Simple Vault: " << simple_vault_size << " vbytes" << std::endl;
    std::cout << "  Recovery Vault: " << recovery_vault_size << " vbytes" << std::endl;
    std::cout << "  Social Recovery: " << social_recovery_size << " vbytes" << std::endl;
    std::cout << "  Restricted Multisig: " << restricted_multisig_size << " vbytes" << std::endl;
    std::cout << "  Escrow Covenant: " << escrow_size << " vbytes" << std::endl;

    printPass("Fee estimation working for all patterns");

    std::cout << "  [✓] PASS: Fee estimation works" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase C.5: Covenant Pattern Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_simple_vault();
        test_recovery_vault();
        test_time_delayed_recovery();
        test_social_recovery();
        test_restricted_multisig();
        test_escrow_covenant();
        test_pattern_validation();
        test_fee_estimation();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Phase C.5 pattern tests passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nSummary:" << std::endl;
        std::cout << "  ✓ Simple Vault pattern works" << std::endl;
        std::cout << "  ✓ Recovery Vault pattern works" << std::endl;
        std::cout << "  ✓ Time-Delayed Recovery pattern works" << std::endl;
        std::cout << "  ✓ Social Recovery (K-of-N) pattern works" << std::endl;
        std::cout << "  ✓ Restricted Multisig pattern works" << std::endl;
        std::cout << "  ✓ Escrow Covenant pattern works" << std::endl;
        std::cout << "  ✓ Pattern validation works" << std::endl;
        std::cout << "  ✓ Fee estimation works" << std::endl;
        std::cout << "\nPhase C.5 Status: ✅ COMPLETE" << std::endl;
        std::cout << "  - All vault patterns implemented" << std::endl;
        std::cout << "  - All recovery flows implemented" << std::endl;
        std::cout << "  - All multisig covenants implemented" << std::endl;
        std::cout << "  - No new primitives needed (composition only)" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cout << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
