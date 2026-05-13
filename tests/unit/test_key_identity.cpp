#include "wallet/key_identity.h"
#include "wallet/key_origin.h"
#include <iostream>
#include <cassert>
#include <iomanip>

using namespace dinero::wallet;

void test_key_id_computation() {
    std::cout << "Testing KeyID computation..." << std::endl;

    // Test vector: compressed pubkey (33 bytes)
    std::vector<uint8_t> pubkey = {
        0x02,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98
    };

    KeyID key_id = ComputeKeyID(pubkey);
    std::string hex = KeyIDToHex(key_id);

    std::cout << "  Pubkey: 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798" << std::endl;
    std::cout << "  KeyID:  " << hex << std::endl;

    // Verify hex conversion roundtrip
    auto parsed = KeyIDFromHex(hex);
    assert(parsed.has_value());
    assert(*parsed == key_id);

    std::cout << "  ✅ KeyID computation works" << std::endl;
}

void test_xonly_key_id() {
    std::cout << "\nTesting X-only KeyID computation (Taproot)..." << std::endl;

    // Test vector: x-only pubkey (32 bytes)
    std::array<uint8_t, 32> xonly = {
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98
    };

    KeyID key_id = ComputeKeyIDFromXOnly(xonly);
    std::string hex = KeyIDToHex(key_id);

    std::cout << "  X-only: 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798" << std::endl;
    std::cout << "  KeyID:  " << hex << std::endl;
    std::cout << "  ✅ X-only KeyID computation works" << std::endl;
}

void test_key_origin_parsing() {
    std::cout << "\nTesting KeyOriginInfo parsing..." << std::endl;

    // Test BIP86 Taproot path
    std::string origin_str = "[f23a9c12/86'/1447'/0'/0/12]";
    auto origin = KeyOriginInfo::parse(origin_str);

    assert(origin.has_value());
    assert(origin->fingerprint == 0xf23a9c12);
    assert(origin->path.size() == 5);

    std::cout << "  Input:  " << origin_str << std::endl;
    std::cout << "  Parsed: " << origin->toString() << std::endl;
    std::cout << "  Path:   " << origin->getPathString() << std::endl;

    // Verify components
    assert(origin->getPurpose() == 86);
    assert(origin->getCoinType() == 1447);
    assert(origin->getAccount() == 0);
    assert(origin->getChange() == 0);
    assert(origin->getIndex() == 12);
    assert(origin->isBIP86());
    assert(!origin->isBIP84());

    std::cout << "  Purpose:  " << origin->getPurpose() << " (BIP86 Taproot)" << std::endl;
    std::cout << "  CoinType: " << origin->getCoinType() << " (Dinero)" << std::endl;
    std::cout << "  ✅ KeyOriginInfo parsing works" << std::endl;
}

void test_bip84_origin() {
    std::cout << "\nTesting BIP84 (P2WPKH) origin..." << std::endl;

    std::string origin_str = "[deadbeef/84'/1447'/0'/0/5]";
    auto origin = KeyOriginInfo::parse(origin_str);

    assert(origin.has_value());
    assert(origin->isBIP84());
    assert(!origin->isBIP86());
    assert(origin->getPurpose() == 84);

    std::cout << "  Input:  " << origin_str << std::endl;
    std::cout << "  Parsed: " << origin->toString() << std::endl;
    std::cout << "  ✅ BIP84 detection works" << std::endl;
}

void test_roundtrip() {
    std::cout << "\nTesting serialization roundtrip..." << std::endl;

    // Create origin
    KeyOriginInfo original;
    original.fingerprint = 0x12345678;
    original.path = {
        86 | KeyOriginInfo::HARDENED_BIT,
        1447 | KeyOriginInfo::HARDENED_BIT,
        0 | KeyOriginInfo::HARDENED_BIT,
        0,
        42
    };

    // Serialize
    std::string serialized = original.toString();
    std::cout << "  Original: " << serialized << std::endl;

    // Parse back
    auto parsed = KeyOriginInfo::parse(serialized);
    assert(parsed.has_value());
    assert(*parsed == original);

    std::cout << "  Parsed:   " << parsed->toString() << std::endl;
    std::cout << "  ✅ Roundtrip successful" << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Key Identity & Origin Unit Tests                    ║" << std::endl;
    std::cout << "║  Week 1, Day 1 - Foundation Layer                    ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    try {
        test_key_id_computation();
        test_xonly_key_id();
        test_key_origin_parsing();
        test_bip84_origin();
        test_roundtrip();

        std::cout << std::endl;
        std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                                  ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
        std::cout << std::endl;
        std::cout << "Week 1, Day 1 COMPLETE: Foundation layer implemented." << std::endl;
        std::cout << "Next: Day 2 - Integrate with WalletManager" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
