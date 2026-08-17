// SPDX-License-Identifier: MIT
// Dinero - Simple Descriptor PSBT Test (Phase 2 Step 3)
//
// This test verifies the CORE fixes:
// 1. Address → scriptPubKey conversion
// 2. Complete key derivation (including address_index)

#include "wallet/descriptor_psbt.h"
#include "wallet/descriptor_checksum.h"
#include "crypto/extended_pubkey.h"
#include "common/address_script_builder.h"
#include "daemon/bech32_encoder.h"
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

void test_address_to_scriptpubkey() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: Address → scriptPubKey Conversion" << std::endl;
    std::cout << "========================================" << std::endl;

    // Test Taproot address (din1p...). Encode with the repo's own encoder so
    // the bech32m checksum matches the "din" HRP — a hardcoded literal with a
    // hand-swapped HRP has an invalid checksum and can never decode.
    const std::vector<uint8_t> taproot_program(32, 0xaa);
    std::string taproot_addr = ::Bech32Encoder::encode_segwit_address("din", 1, taproot_program);
    std::vector<uint8_t> spk;
    std::string error;

    bool result = BuildScriptPubKeyFromAddress(taproot_addr, spk, error);

    if (result) {
        std::cout << "✅ Taproot address decoded successfully" << std::endl;
        std::cout << "   scriptPubKey size: " << spk.size() << " bytes" << std::endl;
        always_assert(spk.size() == 34); // OP_1 <32-byte-pubkey>
        always_assert(spk[0] == 0x51); // OP_1
        always_assert(spk[1] == 0x20); // Push 32 bytes
    } else {
        std::cout << "❌ Failed: " << error << std::endl;
        always_assert(false);
    }

    // Test SegWit v0 address (din1q...), same round-trip approach.
    const std::vector<uint8_t> segwit_program(20, 0xbb);
    std::string segwit_addr = ::Bech32Encoder::encode_segwit_address("din", 0, segwit_program);
    spk.clear();
    error.clear();

    result = BuildScriptPubKeyFromAddress(segwit_addr, spk, error);

    if (result) {
        std::cout << "✅ SegWit address decoded successfully" << std::endl;
        std::cout << "   scriptPubKey size: " << spk.size() << " bytes" << std::endl;
        always_assert(spk.size() == 22); // OP_0 <20-byte-hash>
        always_assert(spk[0] == 0x00); // OP_0
        always_assert(spk[1] == 0x14); // Push 20 bytes
    } else {
        std::cout << "❌ Failed: " << error << std::endl;
        always_assert(false);
    }

    std::cout << std::endl;
}

void test_key_derivation_path() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: Complete Key Derivation Path" << std::endl;
    std::cout << "========================================" << std::endl;

    // Test xpub (from BIP86 test vector)
    std::string xpub_str = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";

    try {
        crypto::ExtendedPubKey ext_key = crypto::ExtendedPubKey::FromString(xpub_str);
        std::cout << "✅ xpub parsed successfully" << std::endl;

        // Derive m/0/5 (receive path, address index 5)
        auto receive_key = ext_key.Derive(0); // m/.../0
        auto address_key = receive_key.Derive(5); // m/.../0/5

        std::vector<uint8_t> pubkey = address_key.GetPublicKey();
        std::cout << "✅ Derived to m/.../0/5" << std::endl;
        std::cout << "   Public key size: " << pubkey.size() << " bytes" << std::endl;
        always_assert(pubkey.size() == 33); // Compressed pubkey

        // Derive m/1/10 (change path, address index 10)
        auto change_key = ext_key.Derive(1); // m/.../1
        auto change_address_key = change_key.Derive(10); // m/.../1/10

        std::vector<uint8_t> change_pubkey = change_address_key.GetPublicKey();
        std::cout << "✅ Derived to m/.../1/10" << std::endl;
        std::cout << "   Public key size: " << change_pubkey.size() << " bytes" << std::endl;
        always_assert(change_pubkey.size() == 33);

        // Verify different keys
        always_assert(pubkey != change_pubkey);
        std::cout << "✅ Receive and change keys are different" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "❌ Failed: " << e.what() << std::endl;
        always_assert(false);
    }

    std::cout << std::endl;
}

void test_descriptor_parsing() {
    std::cout << "========================================" << std::endl;
    std::cout << "TEST: Descriptor Checksum & Parsing" << std::endl;
    std::cout << "========================================" << std::endl;

    // BIP86 descriptor without checksum
    std::string descriptor = "tr([f802fb0e/86h/1448h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)";

    // Add checksum
    std::string with_checksum = din::DescriptorChecksum::AddChecksum(descriptor);
    std::cout << "✅ Added checksum: " << with_checksum.substr(with_checksum.size() - 10) << std::endl;

    // Verify checksum
    bool valid = din::DescriptorChecksum::Verify(with_checksum);
    always_assert(valid);
    std::cout << "✅ Checksum verified" << std::endl;

    // Strip checksum
    std::string stripped = din::DescriptorChecksum::StripChecksum(with_checksum);
    always_assert(stripped == descriptor);
    std::cout << "✅ Stripped checksum correctly" << std::endl;

    std::cout << std::endl;
}

int main() {
    std::cout << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Simple Descriptor PSBT Tests (Phase 2 Step 3)      ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    try {
        test_address_to_scriptpubkey();
        test_key_derivation_path();
        test_descriptor_parsing();

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
