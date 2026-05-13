#include "wallet/bip39.h"
#include "crypto/pbkdf2.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cassert>
#include <cstring>

using namespace dinero::bip39;

// Helper to convert hex string to bytes
std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t) strtol(byteString.c_str(), NULL, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

// Helper to convert bytes to hex string
std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return oss.str();
}

// Test vectors from BIP39 specification
struct TestVector {
    std::string entropy_hex;
    std::string mnemonic;
    std::string seed_hex;
    std::string passphrase;
};

// Official BIP39 test vectors
// Note: BIP39 spec says "The passphrase 'TREZOR' is used for all vectors"
const TestVector TEST_VECTORS[] = {
    {
        "00000000000000000000000000000000",
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
        "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04",
        "TREZOR"  // BIP39 spec uses "TREZOR" for all test vectors
    },
    {
        "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
        "legal winner thank year wave sausage worth useful legal winner thank yellow",
        "2e8905819b8723fe2c1d161860e5ee1830318dbf49a83bd451cfb8440c28bd6fa457fe1296106559a3c80937a1c1069be3a3a5bd381ee6260e8d9739fce1f607",
        "TREZOR"  // BIP39 spec uses "TREZOR" for all test vectors
    },
    {
        "80808080808080808080808080808080",
        "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
        "d71de856f81a8acc65e6fc851a38d4d7ec216fd0796d0a6827a3ad6ed5511a30fa280f12eb2e47ed2ac03b5c462a0358d18d69fe4f985ec81778c1b370b652a8",
        "TREZOR"  // BIP39 spec uses "TREZOR" for all test vectors
    },
    {
        "ffffffffffffffffffffffffffffffff",
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
        "ac27495480225222079d7be181583751e86f571027b0497b5b5d11218e0a8a13332572917f0f8e5a589620c6f15b11c61dee327651a14c34e18231052e48c069",
        "TREZOR"  // BIP39 spec uses "TREZOR" for all test vectors
    }
};

void test_entropy_to_mnemonic() {
    std::cout << "Testing Entropy -> Mnemonic..." << std::endl;
    
    for (const auto& tv : TEST_VECTORS) {
        auto entropy = hex_to_bytes(tv.entropy_hex);
        std::string mnemonic = EntropyToMnemonic(entropy.data(), entropy.size());
        
        std::cout << "  Expected: " << tv.mnemonic << std::endl;
        std::cout << "  Got:      " << mnemonic << std::endl;
        
        assert(mnemonic == tv.mnemonic);
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }
}

void test_mnemonic_validation() {
    std::cout << "Testing Mnemonic Validation..." << std::endl;
    
    // Valid mnemonics
    assert(ValidateMnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"));
    std::cout << "  ✅ Valid mnemonic accepted" << std::endl;
    
    // Invalid checksum
    assert(!ValidateMnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon"));
    std::cout << "  ✅ Invalid checksum rejected" << std::endl;
    
    // Invalid word
    assert(!ValidateMnemonic("notaword abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"));
    std::cout << "  ✅ Invalid word rejected" << std::endl;
    
    // Wrong word count
    assert(!ValidateMnemonic("abandon abandon"));
    std::cout << "  ✅ Wrong word count rejected" << std::endl << std::endl;
}

void test_mnemonic_to_seed() {
    std::cout << "Testing Mnemonic -> Seed (PBKDF2)..." << std::endl;
    
    for (const auto& tv : TEST_VECTORS) {
        std::vector<uint8_t> seed;
        bool success = MnemonicToSeed(tv.mnemonic, tv.passphrase, seed);
        
        assert(success);
        assert(seed.size() == 64);
        
        std::string seed_hex = bytes_to_hex(seed.data(), seed.size());
        
        std::cout << "  Mnemonic: " << tv.mnemonic.substr(0, 40) << "..." << std::endl;
        std::cout << "  Expected: " << tv.seed_hex << std::endl;
        std::cout << "  Got:      " << seed_hex << std::endl;
        
        assert(seed_hex == tv.seed_hex);
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }
}

void test_generate_mnemonic() {
    std::cout << "Testing Random Mnemonic Generation..." << std::endl;
    
    // Generate 12-word mnemonic
    std::string mnemonic = Generate(WordCount::Words12);
    assert(!mnemonic.empty());
    
    std::cout << "  Generated: " << mnemonic << std::endl;
    
    // Validate it
    assert(ValidateMnemonic(mnemonic));
    std::cout << "  ✅ Valid checksum" << std::endl;
    
    // Convert to seed
    std::vector<uint8_t> seed;
    assert(MnemonicToSeed(mnemonic, "", seed));
    assert(seed.size() == 64);
    std::cout << "  ✅ Converts to seed" << std::endl;
    
    // Generate another - should be different
    std::string mnemonic2 = Generate(WordCount::Words12);
    assert(mnemonic != mnemonic2);
    std::cout << "  ✅ Generates unique mnemonics" << std::endl << std::endl;
}

void test_passphrase() {
    std::cout << "Testing Passphrase Protection..." << std::endl;
    
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    
    std::vector<uint8_t> seed1, seed2;
    assert(MnemonicToSeed(mnemonic, "", seed1));
    assert(MnemonicToSeed(mnemonic, "TREZOR", seed2));
    
    // Different passphrases should produce different seeds
    assert(seed1 != seed2);
    std::cout << "  ✅ Different passphrases produce different seeds" << std::endl << std::endl;
}

void test_roundtrip() {
    std::cout << "Testing Entropy Roundtrip..." << std::endl;
    
    // Generate entropy
    uint8_t entropy[16];
    memset(entropy, 0xAA, 16);
    
    // Entropy -> Mnemonic
    std::string mnemonic = EntropyToMnemonic(entropy, 16);
    assert(!mnemonic.empty());
    std::cout << "  Mnemonic: " << mnemonic << std::endl;
    
    // Mnemonic -> Entropy
    std::vector<uint8_t> entropy_out;
    assert(MnemonicToEntropy(mnemonic, entropy_out));
    assert(entropy_out.size() == 16);
    
    // Compare
    assert(memcmp(entropy, entropy_out.data(), 16) == 0);
    std::cout << "  ✅ Entropy roundtrip successful" << std::endl << std::endl;
}

int main() {
    std::cout << "=== BIP39 Test Suite ===" << std::endl << std::endl;
    
    try {
        test_entropy_to_mnemonic();
        test_mnemonic_validation();
        test_mnemonic_to_seed();
        test_generate_mnemonic();
        test_passphrase();
        test_roundtrip();
        
        std::cout << "=== ✅ ALL TESTS PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}

