#include "wallet/bip84_descriptor.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/descriptor_checksum.h"
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║           Descriptor RPC Manual Testing - Direct API Calls            ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    // Test data (example fingerprint and xpub)
    std::string fingerprint = "390c8d84";  // From test_bip86_taproot wallet
    std::string account_xpub = "xpub6Dqr3pZZJNYNGTN8gMYMvLPJvw7s6Jd5VmJRx8FhKvL4QdFSjCmQy3KPjsHhQ2tR8kLmNpQrSt9vWxYzAbCdEfGhIjKlMnOpQrStUv";  // Example
    uint32_t coin_type = 1447;  // Dinero

    std::cout << "Test Parameters:" << std::endl;
    std::cout << "  Fingerprint: " << fingerprint << std::endl;
    std::cout << "  Coin Type:   " << coin_type << std::endl;
    std::cout << std::endl;

    // Test 1: BIP86 Taproot Descriptors
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 1: BIP86 Taproot Descriptor Generation" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    try {
        auto [bip86_receive, bip86_change] = din::BIP86DescriptorFactory::createDefaultDescriptors(
            fingerprint, account_xpub, coin_type
        );

        std::cout << "✅ BIP86 Receive Descriptor:" << std::endl;
        std::cout << "   " << bip86_receive << std::endl;
        std::cout << std::endl;

        std::cout << "✅ BIP86 Change Descriptor:" << std::endl;
        std::cout << "   " << bip86_change << std::endl;
        std::cout << std::endl;

        // Verify format
        if (bip86_receive.substr(0, 3) == "tr(") {
            std::cout << "✅ Format Check: Starts with 'tr(' (Taproot)" << std::endl;
        } else {
            std::cout << "❌ Format Check: Expected 'tr(' prefix" << std::endl;
        }

        if (bip86_receive.find("/86h/") != std::string::npos) {
            std::cout << "✅ Derivation Path: Contains '/86h/' (BIP86)" << std::endl;
        } else {
            std::cout << "❌ Derivation Path: Expected '/86h/'" << std::endl;
        }

        if (bip86_receive.find("/0/*") != std::string::npos) {
            std::cout << "✅ Receive Path: Contains '/0/*' (external chain)" << std::endl;
        } else {
            std::cout << "❌ Receive Path: Expected '/0/*'" << std::endl;
        }

        if (bip86_change.find("/1/*") != std::string::npos) {
            std::cout << "✅ Change Path: Contains '/1/*' (internal chain)" << std::endl;
        } else {
            std::cout << "❌ Change Path: Expected '/1/*'" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cout << "❌ Error: " << e.what() << std::endl;
    }

    std::cout << std::endl;

    // Test 2: BIP84 SegWit Descriptors
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 2: BIP84 SegWit Descriptor Generation" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    try {
        auto [bip84_receive, bip84_change] = din::BIP84DescriptorFactory::createDefaultDescriptors(
            fingerprint, account_xpub, coin_type
        );

        std::cout << "✅ BIP84 Receive Descriptor:" << std::endl;
        std::cout << "   " << bip84_receive << std::endl;
        std::cout << std::endl;

        std::cout << "✅ BIP84 Change Descriptor:" << std::endl;
        std::cout << "   " << bip84_change << std::endl;
        std::cout << std::endl;

        // Verify format
        if (bip84_receive.substr(0, 5) == "wpkh(") {
            std::cout << "✅ Format Check: Starts with 'wpkh(' (SegWit)" << std::endl;
        } else {
            std::cout << "❌ Format Check: Expected 'wpkh(' prefix" << std::endl;
        }

        if (bip84_receive.find("/84h/") != std::string::npos) {
            std::cout << "✅ Derivation Path: Contains '/84h/' (BIP84)" << std::endl;
        } else {
            std::cout << "❌ Derivation Path: Expected '/84h/'" << std::endl;
        }

        if (bip84_receive.find("/0/*") != std::string::npos) {
            std::cout << "✅ Receive Path: Contains '/0/*' (external chain)" << std::endl;
        } else {
            std::cout << "❌ Receive Path: Expected '/0/*'" << std::endl;
        }

        if (bip84_change.find("/1/*") != std::string::npos) {
            std::cout << "✅ Change Path: Contains '/1/*' (internal chain)" << std::endl;
        } else {
            std::cout << "❌ Change Path: Expected '/1/*'" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cout << "❌ Error: " << e.what() << std::endl;
    }

    std::cout << std::endl;

    // Test 3: Descriptor Parsing
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 3: Descriptor Parsing (getdescriptorinfo)" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    // Test BIP86 parsing
    std::string test_bip86_desc = "tr([390c8d84/86h/1447h/0h]" + account_xpub + "/0/*)";
    try {
        auto parsed_bip86 = din::BIP86DescriptorFactory::parseDescriptor(test_bip86_desc);

        std::cout << "BIP86 Descriptor: " << test_bip86_desc << std::endl;
        std::cout << "  Valid: " << (parsed_bip86.valid ? "✅ YES" : "❌ NO") << std::endl;
        std::cout << "  Type: tr (Taproot)" << std::endl;
        std::cout << "  Fingerprint: " << parsed_bip86.fingerprint << std::endl;
        std::cout << "  Is Change: " << (parsed_bip86.is_change ? "YES" : "NO") << std::endl;
        std::cout << std::endl;
    } catch (const std::exception& e) {
        std::cout << "❌ Parse Error: " << e.what() << std::endl;
    }

    // Test BIP84 parsing
    std::string test_bip84_desc = "wpkh([390c8d84/84h/1447h/0h]" + account_xpub + "/0/*)";
    try {
        auto parsed_bip84 = din::BIP84DescriptorFactory::parseDescriptor(test_bip84_desc);

        std::cout << "BIP84 Descriptor: " << test_bip84_desc << std::endl;
        std::cout << "  Valid: " << (parsed_bip84.valid ? "✅ YES" : "❌ NO") << std::endl;
        std::cout << "  Type: wpkh (SegWit)" << std::endl;
        std::cout << "  Fingerprint: " << parsed_bip84.fingerprint << std::endl;
        std::cout << "  Is Change: " << (parsed_bip84.is_change ? "YES" : "NO") << std::endl;
        std::cout << std::endl;
    } catch (const std::exception& e) {
        std::cout << "❌ Parse Error: " << e.what() << std::endl;
    }

    std::cout << "╔════════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                 Descriptor API Testing Complete                       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════════╝" << std::endl;

    return 0;
}
