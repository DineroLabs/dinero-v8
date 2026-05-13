#include "wallet/key_store_impl.h"
#include "crypto/hd_keychain.h"
#include <iostream>
#include <vector>

std::vector<uint8_t> getTestSeed() {
    std::vector<uint8_t> seed(64);
    for (size_t i = 0; i < 64; ++i) {
        seed[i] = static_cast<uint8_t>(i);
    }
    return seed;
}

int main() {
    std::cout << "Testing Extended Key Serialization Fix\n";
    std::cout << "=======================================\n\n";

    // Test with HDKeychain directly
    auto seed = getTestSeed();
    auto master = dinero::crypto::HDKeychain::fromSeed(seed);
    auto account = dinero::crypto::HDKeychain::getBIP84Account(master, 1, 0);
    std::string xpub = account.serialize(true);

    std::cout << "Extended Key (zpub):\n";
    std::cout << xpub << "\n\n";
    std::cout << "Length: " << xpub.length() << " characters\n";

    // Decode to check raw bytes
    // BIP32 extended keys are typically 111 chars in Base58
    // (78 bytes + 4 byte checksum = 82 bytes -> ~111 Base58 chars)

    if (xpub.length() >= 110 && xpub.length() <= 112) {
        std::cout << "✅ CORRECT: Extended key length is standard BIP32\n";
        std::cout << "   Expected: ~111 characters (78 bytes + checksum)\n";
        return 0;
    } else {
        std::cout << "❌ WRONG: Extended key length is non-standard\n";
        std::cout << "   Expected: ~111 characters\n";
        std::cout << "   Got: " << xpub.length() << " characters\n";
        return 1;
    }
}
