#include "wallet/key_store_impl.h"
#include "crypto/hd_keychain.h"
#include <iostream>
#include <iomanip>
#include <vector>

using namespace din;
using namespace dinero::crypto;

// Test seed (BIP39 64-byte seed from test mnemonic)
std::vector<uint8_t> getTestSeed() {
    // Using a well-known test seed for reproducibility
    std::vector<uint8_t> seed(64);
    for (size_t i = 0; i < 64; ++i) {
        seed[i] = static_cast<uint8_t>(i);
    }
    return seed;
}

void printHex(const std::string& label, const std::vector<uint8_t>& data) {
    std::cout << label << ": ";
    for (uint8_t byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(byte);
    }
    std::cout << std::dec << std::endl;
}

bool testBasicInitialization() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST: Basic Initialization" << std::endl;
    std::cout << "========================================" << std::endl;

    KeyStoreImpl keystore("test_wallet");
    auto seed = getTestSeed();

    if (!keystore.initializeFromSeed(seed, 1447)) {
        std::cout << "❌ Failed to initialize from seed" << std::endl;
        return false;
    }

    if (!keystore.isEncrypted()) {
        std::cout << "❌ Signing keystore should default to encrypted state" << std::endl;
        return false;
    }

    std::cout << "✅ KeyStore initialized successfully" << std::endl;
    std::cout << "   Wallet: test_wallet" << std::endl;
    std::cout << "   Coin type: 1447 (Dinero)" << std::endl;

    return true;
}

bool testBIP84AddressDerivation() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST: BIP84 Address Derivation (SegWit)" << std::endl;
    std::cout << "========================================" << std::endl;

    KeyStoreImpl keystore("test_wallet");
    auto seed = getTestSeed();
    keystore.initializeFromSeed(seed, 1447);

    // Derive first 3 receiving addresses
    for (uint32_t i = 0; i < 3; ++i) {
        std::string addr = keystore.deriveBIP84Address(0, false, i);
        if (addr.empty()) {
            std::cout << "❌ Failed to derive BIP84 address " << i << std::endl;
            return false;
        }
        std::cout << "   Address[0/0/" << i << "]: " << addr << std::endl;
    }

    // Derive first change address
    std::string change_addr = keystore.deriveBIP84Address(0, true, 0);
    if (change_addr.empty()) {
        std::cout << "❌ Failed to derive BIP84 change address" << std::endl;
        return false;
    }
    std::cout << "   Change[0/1/0]: " << change_addr << std::endl;

    std::cout << "✅ BIP84 address derivation successful" << std::endl;
    return true;
}

bool testBIP84Descriptor() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST: BIP84 Descriptor Generation" << std::endl;
    std::cout << "========================================" << std::endl;

    KeyStoreImpl keystore("test_wallet");
    auto seed = getTestSeed();
    keystore.initializeFromSeed(seed, 1447);

    // Get receiving descriptor
    std::string recv_desc = keystore.getBIP84Descriptor(0, false);
    if (recv_desc.empty()) {
        std::cout << "❌ Failed to generate receiving descriptor" << std::endl;
        return false;
    }
    std::cout << "   Receiving: " << recv_desc << std::endl;

    // Get change descriptor
    std::string change_desc = keystore.getBIP84Descriptor(0, true);
    if (change_desc.empty()) {
        std::cout << "❌ Failed to generate change descriptor" << std::endl;
        return false;
    }
    std::cout << "   Change: " << change_desc << std::endl;

    // Verify descriptor format
    if (recv_desc.find("wpkh([") != 0) {
        std::cout << "❌ Invalid descriptor format (should start with 'wpkh([')" << std::endl;
        return false;
    }

    std::cout << "✅ BIP84 descriptor generation successful" << std::endl;
    return true;
}

bool testSigningCapability() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST: Signing Capability" << std::endl;
    std::cout << "========================================" << std::endl;

    KeyStoreImpl keystore("test_wallet");
    auto seed = getTestSeed();
    keystore.initializeFromSeed(seed, 1447);

    // Create a test hash to sign
    std::vector<uint8_t> hash(32);
    for (size_t i = 0; i < 32; ++i) {
        hash[i] = static_cast<uint8_t>(i * 7 % 256);
    }

    // Derive key path: m/84h/1447h/0h/0/0
    std::string key_path = "m/84h/1447h/0h/0/0";

    // Check if can sign
    if (!keystore.canSign(key_path)) {
        std::cout << "❌ KeyStore reports cannot sign" << std::endl;
        return false;
    }

    // Sign the hash
    auto sig_opt = keystore.sign(hash, key_path);
    if (!sig_opt) {
        std::cout << "❌ Failed to sign hash" << std::endl;
        return false;
    }

    std::cout << "   Signed hash with key path: " << key_path << std::endl;
    std::cout << "   Signature length: " << sig_opt->size() << " bytes (DER format)" << std::endl;

    std::cout << "✅ Signing capability verified" << std::endl;
    return true;
}

bool testWatchOnlyMode() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST: Watch-Only Mode" << std::endl;
    std::cout << "========================================" << std::endl;

    KeyStoreImpl keystore("watch_only_wallet");

    // Initialize with xpub (watch-only)
    std::string test_xpub = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";
    std::string fingerprint = "f802fb0e";

    if (!keystore.initializeFromXPub(test_xpub, fingerprint)) {
        std::cout << "❌ Failed to initialize watch-only mode" << std::endl;
        return false;
    }

    // Verify cannot sign in watch-only mode
    if (keystore.canSign("m/84h/1447h/0h/0/0")) {
        std::cout << "❌ Watch-only keystore incorrectly reports can sign" << std::endl;
        return false;
    }

    std::vector<uint8_t> hash(32, 0);
    auto sig_opt = keystore.sign(hash, "m/84h/1447h/0h/0/0");
    if (sig_opt) {
        std::cout << "❌ Watch-only keystore incorrectly signed" << std::endl;
        return false;
    }

    std::cout << "✅ Watch-only mode functioning correctly" << std::endl;
    return true;
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║       KeyStoreImpl Test Suite (Phase 3A)              ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    bool all_passed = true;

    all_passed &= testBasicInitialization();
    all_passed &= testBIP84AddressDerivation();
    all_passed &= testBIP84Descriptor();
    all_passed &= testSigningCapability();
    all_passed &= testWatchOnlyMode();

    std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
    if (all_passed) {
        std::cout << "║             ✅ ALL TESTS PASSED ✅                     ║" << std::endl;
    } else {
        std::cout << "║             ❌ SOME TESTS FAILED ❌                    ║" << std::endl;
    }
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    return all_passed ? 0 : 1;
}
