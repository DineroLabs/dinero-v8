#include "wallet/key_store_impl.h"
#include "crypto/hd_keychain.h"
#include <iostream>
#include <iomanip>

using namespace din;

// Simple test seed
std::vector<uint8_t> getTestSeed() {
    std::vector<uint8_t> seed(64);
    for (size_t i = 0; i < 64; ++i) {
        seed[i] = static_cast<uint8_t>(i);
    }
    return seed;
}

int main() {
    std::cout << "KeyStoreImpl Test (Phase 3A)" << std::endl;
    std::cout << "=============================" << std::endl << std::endl;

    // Test 1: Basic initialization
    std::cout << "Test 1: Initialization... ";
    KeyStoreImpl keystore("test_wallet");
    auto seed = getTestSeed();

    if (!keystore.initializeFromSeed(seed, 1447)) {
        std::cout << "FAILED" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;

    // Test 2: BIP84 address derivation
    std::cout << "Test 2: BIP84 Address Derivation... ";
    std::string addr1 = keystore.deriveBIP84Address(0, false, 0);
    std::string addr2 = keystore.deriveBIP84Address(0, false, 1);
    std::string change_addr = keystore.deriveBIP84Address(0, true, 0);

    if (addr1.empty() || addr2.empty() || change_addr.empty()) {
        std::cout << "FAILED" << std::endl;
        return 1;
    }
    if (addr1 == addr2) {
        std::cout << "FAILED (addresses not unique)" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;
    std::cout << "   Address[0/0/0]: " << addr1 << std::endl;
    std::cout << "   Address[0/0/1]: " << addr2 << std::endl;
    std::cout << "   Change[0/1/0]:  " << change_addr << std::endl;

    // Test 3: Descriptor generation
    std::cout << "Test 3: BIP84 Descriptor Generation... ";
    std::string desc = keystore.getBIP84Descriptor(0, false);
    if (desc.empty() || desc.find("wpkh([") != 0) {
        std::cout << "FAILED" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;
    std::cout << "   Descriptor: " << desc << std::endl;

    // Test 4: Signing capability
    std::cout << "Test 4: Signing... ";
    std::vector<uint8_t> hash(32, 0xAA);
    auto sig_opt = keystore.sign(hash, "m/84h/1447h/0h/0/0");
    if (!sig_opt || sig_opt->empty()) {
        std::cout << "FAILED" << std::endl;
        return 1;
    }
    std::cout << "PASSED (" << sig_opt->size() << " bytes DER)" << std::endl;

    // Test 5: Watch-only mode
    std::cout << "Test 5: Watch-Only Mode... ";
    KeyStoreImpl watch_only("watch");
    watch_only.initializeFromXPub("xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8", "f802fb0e");
    if (watch_only.canSign("m")) {
        std::cout << "FAILED (watch-only can sign)" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;

    std::cout << std::endl << "✅ ALL TESTS PASSED" << std::endl;
    return 0;
}
