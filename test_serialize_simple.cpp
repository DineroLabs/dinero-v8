#include "crypto/bip32_slip132.hpp"
#include <iostream>
#include <cstring>

int main() {
    std::cout << "Testing BIP32 Serialization Fix" << std::endl;
    std::cout << "================================" << std::endl << std::endl;

    // Create test data
    uint8_t depth = 3;
    uint32_t parent_fpr = 0xD4691818;
    uint32_t child_num = 0x80000000;  // Hardened 0
    uint8_t chain_code[32];
    uint8_t pubkey[33];

    // Fill with test data
    std::memset(chain_code, 0xAA, 32);
    pubkey[0] = 0x02;  // Compressed pubkey prefix
    std::memset(pubkey + 1, 0xBB, 32);

    // Create NodeSer
    dinero::bip32::NodeSer node = dinero::bip32::serialize_xpub(
        depth, parent_fpr, child_num, chain_code, pubkey);

    // Check NodeSer size
    std::cout << "NodeSer size: " << sizeof(node.data) << " bytes" << std::endl;

    if (sizeof(node.data) == 74) {
        std::cout << "✅ CORRECT: NodeSer is 74 bytes (BIP32 payload)" << std::endl;
    } else {
        std::cout << "❌ WRONG: NodeSer is " << sizeof(node.data) << " bytes (expected 74)" << std::endl;
        return 1;
    }

    // Serialize to zpub (mainnet)
    std::string zpub = dinero::bip32::to_zpub_mainnet(node);

    std::cout << "\nExtended Key (zpub):" << std::endl;
    std::cout << zpub << std::endl << std::endl;
    std::cout << "Length: " << zpub.length() << " characters" << std::endl;

    // BIP32 extended keys are 111 characters in Base58
    // (78 bytes total: 4 version + 74 payload, then Base58 encoded ~111 chars)
    if (zpub.length() >= 110 && zpub.length() <= 112) {
        std::cout << "✅ CORRECT: Extended key length is standard BIP32" << std::endl;
        std::cout << "   Expected: ~111 characters (78 bytes total)" << std::endl;
        return 0;
    } else {
        std::cout << "❌ WRONG: Extended key length is non-standard" << std::endl;
        std::cout << "   Expected: ~111 characters" << std::endl;
        std::cout << "   Got: " << zpub.length() << " characters" << std::endl;
        return 1;
    }
}
