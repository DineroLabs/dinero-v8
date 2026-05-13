/**
 * Taproot Key Generation and Signing Test
 *
 * Tests BIP340 Schnorr signatures and BIP86 Taproot key generation
 */

#include <iostream>
#include <iomanip>
#include <cassert>
#include "wallet/taproot_keys.h"
#include "consensus/chainparams.h"
#include "address/addr_codec.h"

extern "C" {
#include <secp256k1.h>
}

void print_hex(const std::string& label, const uint8_t* data, size_t len) {
    std::cout << label << ": ";
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    std::cout << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Taproot Key Generation & Signing Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Initialize chain parameters for regtest
    dinero::SelectParams(dinero::Chain::REGTEST);
    const std::string& hrp = dinero::HrpForActiveNetworkRef();
    std::cout << "Network HRP: " << hrp << " (regtest)" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    // Test 1: Generate Taproot keypair
    std::cout << "[TEST 1] Generate Taproot keypair" << std::endl;
    try {
        std::array<uint8_t, 32> privkey;
        std::array<uint8_t, 32> xonly_pubkey;
        int parity;

        bool success = dinero::TaprootKeys::GenerateKeypair(privkey, xonly_pubkey, parity);

        if (success) {
            std::cout << "  ✓ Generated Taproot keypair" << std::endl;
            print_hex("  Private key", privkey.data(), 32);
            print_hex("  X-only pubkey", xonly_pubkey.data(), 32);
            std::cout << "  Parity: " << parity << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Failed to generate keypair" << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 2: Create Taproot address from x-only pubkey
    std::cout << "[TEST 2] Create Taproot address" << std::endl;
    try {
        std::array<uint8_t, 32> privkey;
        std::array<uint8_t, 32> xonly_pubkey;
        int parity;

        dinero::TaprootKeys::GenerateKeypair(privkey, xonly_pubkey, parity);
        std::string address = dinero::TaprootKeys::CreateTaprootAddress(xonly_pubkey, hrp);

        const std::string prefix = hrp + "1";
        const std::string v1_prefix = hrp + "1p";

        if (!address.empty() && address.rfind(prefix, 0) == 0) {
            std::cout << "  ✓ Created Taproot address" << std::endl;
            std::cout << "  Address: " << address << std::endl;

            // Verify it starts with correct prefix for witness v1
            if (address.rfind(v1_prefix, 0) == 0) {
                std::cout << "  ✓ Correct witness version 1 prefix (" << v1_prefix << ")" << std::endl;
                passed++;
            } else {
                std::cerr << "  ✗ FAIL: Incorrect witness version prefix" << std::endl;
                failed++;
            }
        } else {
            std::cerr << "  ✗ FAIL: Failed to create address or invalid format" << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 3: Sign and verify Schnorr signature
    std::cout << "[TEST 3] BIP340 Schnorr signature creation and verification" << std::endl;
    try {
        // Generate keypair
        std::array<uint8_t, 32> privkey;
        std::array<uint8_t, 32> xonly_pubkey;
        int parity;
        dinero::TaprootKeys::GenerateKeypair(privkey, xonly_pubkey, parity);

        // Create a test message (32 bytes)
        std::array<uint8_t, 32> msg;
        msg.fill(0x42);  // Arbitrary test message

        // Sign the message
        std::array<uint8_t, 64> sig;
        bool sign_success = dinero::TaprootKeys::SignSchnorr(sig, msg, privkey);

        if (!sign_success) {
            std::cerr << "  ✗ FAIL: Failed to create signature" << std::endl;
            failed++;
        } else {
            std::cout << "  ✓ Created Schnorr signature (64 bytes)" << std::endl;
            print_hex("  Signature", sig.data(), 64);

            // Verify the signature
            bool verify_success = dinero::TaprootKeys::VerifySchnorr(sig, msg, xonly_pubkey);

            if (verify_success) {
                std::cout << "  ✓ Signature verified successfully" << std::endl;
                passed++;
            } else {
                std::cerr << "  ✗ FAIL: Signature verification failed" << std::endl;
                failed++;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 4: Verify signature with wrong key fails
    std::cout << "[TEST 4] Signature verification with wrong key (should fail)" << std::endl;
    try {
        // Generate two keypairs
        std::array<uint8_t, 32> privkey1, privkey2;
        std::array<uint8_t, 32> xonly_pubkey1, xonly_pubkey2;
        int parity;

        dinero::TaprootKeys::GenerateKeypair(privkey1, xonly_pubkey1, parity);
        dinero::TaprootKeys::GenerateKeypair(privkey2, xonly_pubkey2, parity);

        // Sign with key1
        std::array<uint8_t, 32> msg;
        msg.fill(0x42);
        std::array<uint8_t, 64> sig;
        dinero::TaprootKeys::SignSchnorr(sig, msg, privkey1);

        // Try to verify with key2 (should fail)
        bool verify_result = dinero::TaprootKeys::VerifySchnorr(sig, msg, xonly_pubkey2);

        if (!verify_result) {
            std::cout << "  ✓ Correctly rejected signature with wrong key" << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Signature verified with wrong key (should have failed)" << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 5: Derive x-only pubkey from existing privkey
    std::cout << "[TEST 5] Derive x-only pubkey from private key" << std::endl;
    try {
        // Create a known private key (for testing)
        std::array<uint8_t, 32> privkey;
        for (int i = 0; i < 32; i++) {
            privkey[i] = static_cast<uint8_t>(i + 1);
        }

        std::array<uint8_t, 32> xonly_pubkey;
        int parity;

        bool success = dinero::TaprootKeys::DeriveXOnlyPubkey(privkey, xonly_pubkey, parity);

        if (success) {
            std::cout << "  ✓ Derived x-only pubkey from private key" << std::endl;
            print_hex("  X-only pubkey", xonly_pubkey.data(), 32);
            std::cout << "  Parity: " << parity << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Failed to derive pubkey" << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 6 (Taproot ring signing key) was retired: it covered
    // dinero::wallet::ComputeRingSigningKey / dinero::zk::Scalar32, which
    // were removed from the codebase when confidential transactions
    // migrated from a ring-signature scheme to consensus/shielded/
    // (Spend / Output via Spartan + Pedersen + Schnorr binding sigs).
    // No replacement public API exists, so there is nothing to assert.
    // Tests 1-5 above continue to cover BIP340 Schnorr + BIP86 Taproot.

    // Summary
    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << passed << std::endl;
    std::cout << "  Failed: " << failed << std::endl;
    std::cout << "========================================" << std::endl;

    if (failed == 0) {
        std::cout << std::endl;
        std::cout << "✓ All tests passed!" << std::endl;
        std::cout << "Taproot key generation and BIP340 Schnorr signing are working correctly." << std::endl;
        return 0;
    } else {
        std::cerr << std::endl;
        std::cerr << "✗ Some tests failed." << std::endl;
        return 1;
    }
}
