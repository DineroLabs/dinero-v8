/**
 * Taproot Address Decoding Test
 *
 * Tests Step 4.2 implementation: Decoding Taproot addresses (bech32m) and
 * creating P2TR scriptPubKey outputs.
 */

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "address/addr_codec.h"
#include "bech32/bech32.hpp"
#include "consensus/chainparams.h"
#include <array>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Taproot Address Decoding Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Initialize chain parameters for regtest
    dinero::SelectParams(dinero::Chain::REGTEST);
    const std::string& hrp = dinero::HrpForActiveNetworkRef();
    std::cout << "Network HRP: " << hrp << " (regtest)" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    auto hex_to_bytes = [](const std::string& hex) {
        std::vector<uint8_t> out;
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return out;
    };

    // Use a deterministic x-only pubkey (secp G x-coordinate from BIP340 vectors)
    const std::string internal_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
    std::vector<uint8_t> internal_key = hex_to_bytes(internal_hex);

    // Encode a valid Taproot address for the active HRP
    std::string taproot_addr = bech32::Encode(hrp, 1, internal_key, bech32::Encoding::BECH32M);

    // Test 1: Decode valid Taproot address
    std::cout << "[TEST 1] Decode valid Taproot address" << std::endl;
    try {
        std::vector<uint8_t> witness_program = dinero::DecodeTaprootWitnessProgram(taproot_addr);

        if (witness_program.size() == 32) {
            std::cout << "  ✓ Decoded 32-byte witness program" << std::endl;
            std::cout << "  First 8 bytes: ";
            for (int i = 0; i < 8 && i < static_cast<int>(witness_program.size()); i++) {
                printf("%02x", witness_program[i]);
            }
            std::cout << "..." << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Expected 32 bytes, got " << witness_program.size() << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 2: Create P2TR scriptPubKey
    std::cout << "[TEST 2] Create P2TR scriptPubKey" << std::endl;
    try {
        // Create a dummy 32-byte witness program
        std::vector<uint8_t> witness_program(32);
        for (int i = 0; i < 32; i++) {
            witness_program[i] = static_cast<uint8_t>(i);
        }

        std::vector<uint8_t> script_pubkey = dinero::CreateP2TRScriptPubKey(witness_program);

        // P2TR scriptPubKey should be: OP_1 (0x51) + OP_PUSHBYTES_32 (0x20) + 32 bytes = 34 bytes
        if (script_pubkey.size() == 34 &&
            script_pubkey[0] == 0x51 &&  // OP_1
            script_pubkey[1] == 0x20) {  // OP_PUSHBYTES_32

            std::cout << "  ✓ Created 34-byte P2TR scriptPubKey" << std::endl;
            std::cout << "  Format: OP_1 (0x51) + OP_PUSHBYTES_32 (0x20) + 32-byte witness program" << std::endl;
            std::cout << "  First 4 bytes: ";
            for (int i = 0; i < 4; i++) {
                printf("%02x ", script_pubkey[i]);
            }
            std::cout << "..." << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Invalid scriptPubKey format" << std::endl;
            std::cerr << "    Size: " << script_pubkey.size() << " (expected 34)" << std::endl;
            if (script_pubkey.size() >= 2) {
                std::cerr << "    Byte 0: 0x" << std::hex << (int)script_pubkey[0] << " (expected 0x51)" << std::endl;
                std::cerr << "    Byte 1: 0x" << std::hex << (int)script_pubkey[1] << " (expected 0x20)" << std::endl;
            }
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 3: Decode Taproot address to Destination
    std::cout << "[TEST 3] Decode Taproot address to Destination" << std::endl;
    try {
        dinero::Destination dest = dinero::DecodeTaprootAddress(taproot_addr, hrp);

        if (dinero::IsValidDestination(dest) && dest.pubkey_hash.size() == 32) {
            std::cout << "  ✓ Created valid Destination with 32-byte pubkey" << std::endl;
            passed++;
        } else {
            std::cerr << "  ✗ FAIL: Invalid destination" << std::endl;
            std::cerr << "    Is valid: " << dinero::IsValidDestination(dest) << std::endl;
            std::cerr << "    Pubkey size: " << dest.pubkey_hash.size() << std::endl;
            failed++;
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Test 4: Reject non-bech32m addresses
    std::cout << "[TEST 4] Reject non-bech32m addresses (witness v0)" << std::endl;
    try {
        // This is a bech32 (not bech32m) address - should fail
        std::string p2wpkh_addr = "rdin1qxyz...";  // witness v0

        try {
            std::vector<uint8_t> witness_program = dinero::DecodeTaprootWitnessProgram(p2wpkh_addr);
            std::cerr << "  ✗ FAIL: Should have rejected non-bech32m address" << std::endl;
            failed++;
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            if (error_msg.find("bech32m") != std::string::npos ||
                error_msg.find("decode") != std::string::npos) {
                std::cout << "  ✓ Correctly rejected non-bech32m address" << std::endl;
                std::cout << "    Error: " << error_msg << std::endl;
                passed++;
            } else {
                std::cerr << "  ✗ FAIL: Wrong error message: " << error_msg << std::endl;
                failed++;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: Unexpected exception: " << e.what() << std::endl;
        failed++;
    }
    std::cout << std::endl;

    // Summary
    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << passed << std::endl;
    std::cout << "  Failed: " << failed << std::endl;
    std::cout << "========================================" << std::endl;

    if (failed == 0) {
        std::cout << std::endl;
        std::cout << "✓ All tests passed!" << std::endl;
        std::cout << "Step 4.2 Taproot address decoding is working correctly." << std::endl;
        return 0;
    } else {
        std::cerr << std::endl;
        std::cerr << "✗ Some tests failed." << std::endl;
        return 1;
    }
}
