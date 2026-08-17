// SPDX-License-Identifier: MIT
// Dinero - Descriptor PSBT Integration Test (Phase 2 Step 3)

#include "wallet/descriptor_psbt.h"
#include "daemon/bech32_encoder.h"
#include "wallet/descriptor_checksum.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>

// Always-on check: assert() compiles out under NDEBUG and gates nothing in
// release/CI builds (scripts/ci/check_test_assertions.py, issue #497).
#define always_assert(cond)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", #cond,      \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

using namespace dinero;

// Helper: Create mock Taproot scriptPubKey (OP_1 <32-byte-pubkey>)
std::vector<uint8_t> createTaprootScriptPubKey() {
    std::vector<uint8_t> spk;
    spk.push_back(0x51); // OP_1 (witness v1)
    spk.push_back(0x20); // Push 32 bytes
    for (int i = 0; i < 32; ++i) {
        spk.push_back(static_cast<uint8_t>(i)); // Mock x-only pubkey
    }
    return spk;
}

// Helper: Create mock P2WPKH scriptPubKey (OP_0 <20-byte-hash>)
std::vector<uint8_t> createSegWitScriptPubKey() {
    std::vector<uint8_t> spk;
    spk.push_back(0x00); // OP_0 (witness v0)
    spk.push_back(0x14); // Push 20 bytes
    for (int i = 0; i < 20; ++i) {
        spk.push_back(static_cast<uint8_t>(i)); // Mock pubkey hash
    }
    return spk;
}

void test_bip86_descriptor_to_psbt() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: BIP86 Descriptor → PSBT Creation" << std::endl;
    std::cout << "========================================" << std::endl;

    // Test descriptor (BIP86 Taproot)
    std::string descriptor = "tr([f802fb0e/86h/1448h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)";

    // Add checksum
    descriptor = din::DescriptorChecksum::AddChecksum(descriptor);
    std::cout << "Descriptor: " << descriptor.substr(0, 60) << "..." << std::endl;

    // Create PSBT request
    DescriptorPsbtRequest request;
    request.descriptor = descriptor;

    // Add synthetic input (Taproot UTXO)
    PsbtInputInfo input;
    input.txid = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    input.vout = 0;
    input.amount = 100000; // 100k sats
    input.scriptPubKey = createTaprootScriptPubKey();
    input.address_index = 0; // m/.../0/0
    request.inputs.push_back(input);

    // Add mock output
    PsbtOutputInfo output;
    // Valid bech32m Taproot address (checksum must match the "din" HRP,
    // so encode with the same library the wallet decodes with).
    output.address = ::Bech32Encoder::encode_segwit_address(
        "din", 1, std::vector<uint8_t>(32, 0xaa));
    output.amount = 95000; // 95k sats (5k fee)
    request.outputs.push_back(output);

    // Create PSBT
    WalletPolicy wallet_policy = WalletPolicy::BIP86_TAPROOT;
    std::string wallet_fingerprint = "f802fb0e";

    DescriptorPsbtResult result = DescriptorPsbtFactory::createPsbtFromDescriptor(
        request,
        wallet_policy,
        wallet_fingerprint
    );

    // Verify result
    if (!result.success) {
        std::cout << "createPsbtFromDescriptor error: " << result.error << std::endl;
    }
    always_assert(result.success);
    always_assert(result.descriptor_type == "tr");
    always_assert(result.wallet_policy == WalletPolicy::BIP86_TAPROOT);
    always_assert(result.input_count == 1);
    always_assert(result.output_count == 1);
    always_assert(result.fee == 5000);
    always_assert(!result.psbt_base64.empty());

    std::cout << "✅ PSBT created successfully!" << std::endl;
    std::cout << "   Descriptor type: " << result.descriptor_type << std::endl;
    std::cout << "   Inputs: " << result.input_count << std::endl;
    std::cout << "   Outputs: " << result.output_count << std::endl;
    std::cout << "   Fee: " << result.fee << " sats" << std::endl;
    std::cout << "   PSBT length: " << result.psbt_base64.length() << " bytes (base64)" << std::endl;
    std::cout << std::endl;
}

void test_bip84_descriptor_to_psbt() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: BIP84 Descriptor → PSBT Creation" << std::endl;
    std::cout << "========================================" << std::endl;

    // Test descriptor (BIP84 SegWit)
    std::string descriptor = "wpkh([ae6a04bd/84h/1448h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)";

    // Add checksum
    descriptor = din::DescriptorChecksum::AddChecksum(descriptor);
    std::cout << "Descriptor: " << descriptor.substr(0, 60) << "..." << std::endl;

    // Create PSBT request
    DescriptorPsbtRequest request;
    request.descriptor = descriptor;

    // Add synthetic input (SegWit UTXO)
    PsbtInputInfo input;
    input.txid = "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
    input.vout = 1;
    input.amount = 50000; // 50k sats
    input.scriptPubKey = createSegWitScriptPubKey();
    input.address_index = 0; // m/.../0/0
    request.inputs.push_back(input);

    // Add mock output
    PsbtOutputInfo output;
    // Valid bech32 SegWit v0 address, same round-trip approach.
    output.address = ::Bech32Encoder::encode_segwit_address(
        "din", 0, std::vector<uint8_t>(20, 0xbb));
    output.amount = 48000; // 48k sats (2k fee)
    request.outputs.push_back(output);

    // Create PSBT
    WalletPolicy wallet_policy = WalletPolicy::BIP84_LEGACY;
    std::string wallet_fingerprint = "ae6a04bd";

    DescriptorPsbtResult result = DescriptorPsbtFactory::createPsbtFromDescriptor(
        request,
        wallet_policy,
        wallet_fingerprint
    );

    // Verify result
    if (!result.success) {
        std::cout << "createPsbtFromDescriptor error: " << result.error << std::endl;
    }
    always_assert(result.success);
    always_assert(result.descriptor_type == "wpkh");
    always_assert(result.wallet_policy == WalletPolicy::BIP84_LEGACY);
    always_assert(result.input_count == 1);
    always_assert(result.output_count == 1);
    always_assert(result.fee == 2000);
    always_assert(!result.psbt_base64.empty());

    std::cout << "✅ PSBT created successfully!" << std::endl;
    std::cout << "   Descriptor type: " << result.descriptor_type << std::endl;
    std::cout << "   Inputs: " << result.input_count << std::endl;
    std::cout << "   Outputs: " << result.output_count << std::endl;
    std::cout << "   Fee: " << result.fee << " sats" << std::endl;
    std::cout << "   PSBT length: " << result.psbt_base64.length() << " bytes (base64)" << std::endl;
    std::cout << std::endl;
}

void test_policy_mismatch_rejection() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: Policy Mismatch Rejection" << std::endl;
    std::cout << "========================================" << std::endl;

    // BIP86 descriptor but BIP84 wallet policy - should fail
    std::string descriptor = "tr([f802fb0e/86h/1448h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)";
    descriptor = din::DescriptorChecksum::AddChecksum(descriptor);

    DescriptorPsbtRequest request;
    request.descriptor = descriptor;

    PsbtInputInfo input;
    input.txid = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    input.vout = 0;
    input.amount = 100000;
    input.scriptPubKey = createTaprootScriptPubKey();
    request.inputs.push_back(input);

    PsbtOutputInfo output;
    // Valid Taproot address: the rejection below must come from the
    // POLICY mismatch, not from an undecodable address.
    output.address = ::Bech32Encoder::encode_segwit_address(
        "din", 1, std::vector<uint8_t>(32, 0xcc));
    output.amount = 95000;
    request.outputs.push_back(output);

    // Wrong policy!
    WalletPolicy wallet_policy = WalletPolicy::BIP84_LEGACY;
    std::string wallet_fingerprint = "f802fb0e";

    DescriptorPsbtResult result = DescriptorPsbtFactory::createPsbtFromDescriptor(
        request,
        wallet_policy,
        wallet_fingerprint
    );

    // Should fail
    always_assert(!result.success);
    always_assert(!result.error.empty());
    always_assert(result.error.find("does not match wallet policy") != std::string::npos);

    std::cout << "✅ Policy mismatch correctly rejected!" << std::endl;
    std::cout << "   Error: " << result.error << std::endl;
    std::cout << std::endl;
}

int main() {
    std::cout << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Descriptor PSBT Integration Tests (Phase 2 Step 3)   ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    try {
        test_bip86_descriptor_to_psbt();
        test_bip84_descriptor_to_psbt();
        test_policy_mismatch_rejection();

        std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║             ✅ ALL TESTS PASSED ✅                     ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;
        std::cout << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
