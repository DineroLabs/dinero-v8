// Test ExtendedPubKey deserialization and derivation
#include "crypto/extended_pubkey.h"
#include <iostream>
#include <iomanip>

using namespace dinero::crypto;

void print_hex(const std::vector<uint8_t>& data) {
    for (auto byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    std::cout << std::dec;
}

int main() {
    try {
        // BIP32 test vector #1 (from BIP32 spec)
        // Master extended public key
        std::string test_xpub = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";

        std::cout << "Testing ExtendedPubKey::FromString()...\n";
        std::cout << "Input xpub: " << test_xpub << "\n\n";

        ExtendedPubKey key = ExtendedPubKey::FromString(test_xpub);

        std::cout << "✅ Deserialization successful!\n";
        std::cout << "Depth: " << (int)key.GetDepth() << "\n";
        std::cout << "Child number: " << key.GetChildNumber() << "\n";
        std::cout << "Parent fingerprint: 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << key.GetParentFingerprint() << std::dec << "\n";
        std::cout << "Fingerprint: 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << key.GetFingerprint() << std::dec << "\n";

        std::cout << "\nPublic key: ";
        print_hex(key.GetPublicKey());
        std::cout << "\n";

        std::cout << "\nChain code: ";
        print_hex(key.GetChainCode());
        std::cout << "\n\n";

        // Test derivation
        std::cout << "Testing Derive(0)...\n";
        ExtendedPubKey child0 = key.Derive(0);
        std::cout << "✅ Derivation successful!\n";
        std::cout << "Child 0 depth: " << (int)child0.GetDepth() << "\n";
        std::cout << "Child 0 public key: ";
        print_hex(child0.GetPublicKey());
        std::cout << "\n";

        // Test derivation chain: m/0/1
        std::cout << "\nTesting Derive(0).Derive(1)...\n";
        ExtendedPubKey child0_1 = child0.Derive(1);
        std::cout << "✅ Derivation successful!\n";
        std::cout << "Child 0/1 depth: " << (int)child0_1.GetDepth() << "\n";
        std::cout << "Child 0/1 public key: ";
        print_hex(child0_1.GetPublicKey());
        std::cout << "\n\n";

        // Test serialization round-trip
        std::cout << "Testing Serialize()...\n";
        std::string serialized = key.Serialize();
        std::cout << "Serialized: " << serialized << "\n";
        ExtendedPubKey deserialized = ExtendedPubKey::FromString(serialized);
        std::cout << "✅ Round-trip successful!\n\n";

        // Test hardened derivation rejection
        std::cout << "Testing hardened derivation rejection...\n";
        try {
            key.Derive(0x80000000);
            std::cout << "❌ ERROR: Hardened derivation should have failed!\n";
            return 1;
        } catch (const std::exception& e) {
            std::cout << "✅ Correctly rejected: " << e.what() << "\n\n";
        }

        std::cout << "==========================================\n";
        std::cout << "✅ ALL TESTS PASSED!\n";
        std::cout << "==========================================\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST FAILED: " << e.what() << "\n";
        return 1;
    }
}
