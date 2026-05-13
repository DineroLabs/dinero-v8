#include "wallet/script_ownership.h"
#include "wallet/keystore.h"
#include "wallet/key_identity.h"
#include <iostream>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <map>

using namespace dinero::wallet;

// ═══════════════════════════════════════════════════════════════
// Mock WalletKeyStore for Testing
// ═══════════════════════════════════════════════════════════════

class MockKeyStore : public WalletKeyStore {
private:
    std::map<KeyID, WalletKey> keys_;              // key_id → WalletKey
    std::map<KeyID, KeyID> output_to_internal_;    // output_key_id → key_id (for Taproot)
    std::vector<uint8_t> master_seed_;
    bool have_seed_;

public:
    MockKeyStore() : have_seed_(false) {}

    void SetMasterSeed(const std::vector<uint8_t>& seed) {
        master_seed_ = seed;
        have_seed_ = true;
    }

    void AddMockKey(const WalletKey& key) {
        keys_[key.id] = key;

        // For Taproot, also register output_key_id mapping
        if (key.output_key_id.has_value()) {
            output_to_internal_[key.output_key_id.value()] = key.id;
        }
    }

    // ═══ WalletKeyStore Interface ═══

    bool HaveKey(const KeyID& key_id) const override {
        return keys_.find(key_id) != keys_.end();
    }

    std::optional<WalletKey> GetKey(const KeyID& key_id) const override {
        auto it = keys_.find(key_id);
        if (it != keys_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<WalletKey> GetKeyByOutputKeyID(const KeyID& output_key_id) const override {
        // Find internal key_id from output_key_id
        auto it = output_to_internal_.find(output_key_id);
        if (it != output_to_internal_.end()) {
            return GetKey(it->second);  // Look up by internal key_id
        }
        return std::nullopt;
    }

    std::vector<WalletKey> GetAllKeys() const override {
        std::vector<WalletKey> result;
        for (const auto& pair : keys_) {
            result.push_back(pair.second);
        }
        return result;
    }

    bool AddKey(const WalletKey& key) override {
        keys_[key.id] = key;
        if (key.output_key_id.has_value()) {
            output_to_internal_[key.output_key_id.value()] = key.id;
        }
        return true;
    }

    bool HaveMasterSeed() const override {
        return have_seed_;
    }

    std::optional<std::vector<uint8_t>> GetMasterSeed() const override {
        if (have_seed_) {
            return master_seed_;
        }
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> DerivePrivateKey(
        const KeyOriginInfo& origin) const override {
        // Not implemented for this test
        return std::nullopt;
    }
};

// ═══════════════════════════════════════════════════════════════
// Test Helpers
// ═══════════════════════════════════════════════════════════════

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════

void test_p2wpkh_extraction() {
    std::cout << "Testing P2WPKH key extraction..." << std::endl;

    // P2WPKH scriptPubKey: 0x0014 <20-byte-hash>
    // Example: 0014751e76e8199196d454941c45d1b3a323f1433bd6
    std::vector<uint8_t> script = hexToBytes("0014751e76e8199196d454941c45d1b3a323f1433bd6");

    MockKeyStore keystore;
    ScriptOwnershipResolver resolver(&keystore);

    // Extract KeyID
    auto key_ids = resolver.ExtractKeyIDs(script);
    assert(key_ids.size() == 1);

    std::string extracted_hex = KeyIDToHex(key_ids[0]);
    std::cout << "  Extracted KeyID: " << extracted_hex << std::endl;
    assert(extracted_hex == "751e76e8199196d454941c45d1b3a323f1433bd6");

    std::cout << "  ✅ P2WPKH extraction works" << std::endl;
}

void test_p2tr_extraction() {
    std::cout << "\nTesting P2TR key extraction..." << std::endl;

    // P2TR scriptPubKey: 0x5120 <32-byte-tweaked-pubkey>
    // Example tweaked key: a60869f0dbcf1dc659c9cecbaf8050135ea9e8cdc487053f1dc6880949dc684c
    std::string tweaked_key_hex = "a60869f0dbcf1dc659c9cecbaf8050135ea9e8cdc487053f1dc6880949dc684c";
    std::vector<uint8_t> script = hexToBytes("5120" + tweaked_key_hex);

    MockKeyStore keystore;
    ScriptOwnershipResolver resolver(&keystore);

    // Extract output_key_id
    auto key_ids = resolver.ExtractKeyIDs(script);
    assert(key_ids.size() == 1);

    std::string extracted_hex = KeyIDToHex(key_ids[0]);
    std::cout << "  Tweaked key: " << tweaked_key_hex << std::endl;
    std::cout << "  Extracted output_key_id: " << extracted_hex << std::endl;

    std::cout << "  ✅ P2TR extraction works" << std::endl;
}

void test_ismine_p2wpkh() {
    std::cout << "\nTesting IsMine for P2WPKH..." << std::endl;

    // P2WPKH scriptPubKey
    std::vector<uint8_t> script = hexToBytes("0014751e76e8199196d454941c45d1b3a323f1433bd6");

    MockKeyStore keystore;
    keystore.SetMasterSeed({0x01, 0x02, 0x03});  // Have master seed = spendable

    ScriptOwnershipResolver resolver(&keystore);

    // Initially: NO (key not in wallet)
    auto ownership = resolver.IsMine(script);
    assert(ownership == ScriptOwnership::NO);
    std::cout << "  Before adding key: NO ✓" << std::endl;

    // Add key to wallet (spendable)
    auto key_id_bytes = hexToBytes("751e76e8199196d454941c45d1b3a323f1433bd6");
    KeyID key_id;
    std::copy(key_id_bytes.begin(), key_id_bytes.end(), key_id.begin());

    WalletKey wallet_key;
    wallet_key.id = key_id;
    wallet_key.spendable = true;
    wallet_key.origin.fingerprint = 0x12345678;
    wallet_key.origin.path = {84 | 0x80000000, 1447 | 0x80000000, 0 | 0x80000000, 0, 0};
    keystore.AddMockKey(wallet_key);

    // Now: SPENDABLE
    ownership = resolver.IsMine(script);
    assert(ownership == ScriptOwnership::SPENDABLE);
    std::cout << "  After adding spendable key: SPENDABLE ✓" << std::endl;

    std::cout << "  ✅ P2WPKH IsMine works" << std::endl;
}

void test_ismine_p2tr() {
    std::cout << "\nTesting IsMine for P2TR (critical for Taproot fix)..." << std::endl;

    // P2TR scriptPubKey with tweaked key
    std::string tweaked_key_hex = "a60869f0dbcf1dc659c9cecbaf8050135ea9e8cdc487053f1dc6880949dc684c";
    std::vector<uint8_t> script = hexToBytes("5120" + tweaked_key_hex);

    // Compute output_key_id from tweaked key
    std::array<uint8_t, 32> tweaked_key;
    auto tweaked_bytes = hexToBytes(tweaked_key_hex);
    std::copy(tweaked_bytes.begin(), tweaked_bytes.end(), tweaked_key.begin());
    KeyID output_key_id = ComputeKeyIDFromXOnly(tweaked_key);

    std::cout << "  Tweaked key:     " << tweaked_key_hex << std::endl;
    std::cout << "  output_key_id:   " << KeyIDToHex(output_key_id) << std::endl;

    MockKeyStore keystore;
    keystore.SetMasterSeed({0x01, 0x02, 0x03});

    ScriptOwnershipResolver resolver(&keystore);

    // Initially: NO
    auto ownership = resolver.IsMine(script);
    assert(ownership == ScriptOwnership::NO);
    std::cout << "  Before adding key: NO ✓" << std::endl;

    // Add Taproot key to wallet
    // internal_key_id would be different from output_key_id
    // For this test, we'll use a mock internal key
    auto internal_key_bytes = hexToBytes("f678d9b79045452c8c64e9309d0f0046056e26c5");
    KeyID internal_key_id;
    std::copy(internal_key_bytes.begin(), internal_key_bytes.end(), internal_key_id.begin());

    WalletKey wallet_key;
    wallet_key.id = internal_key_id;
    wallet_key.internal_key_id = internal_key_id;
    wallet_key.output_key_id = output_key_id;  // CRITICAL: Store output_key_id
    wallet_key.spendable = true;
    wallet_key.origin.fingerprint = 0xf23a9c12;
    wallet_key.origin.path = {86 | 0x80000000, 1447 | 0x80000000, 0 | 0x80000000, 0, 0};
    keystore.AddMockKey(wallet_key);

    std::cout << "  internal_key_id: " << KeyIDToHex(internal_key_id) << std::endl;

    // Now: SPENDABLE (matched via output_key_id)
    ownership = resolver.IsMine(script);
    assert(ownership == ScriptOwnership::SPENDABLE);
    std::cout << "  After adding Taproot key: SPENDABLE ✓" << std::endl;
    std::cout << "  🎯 Taproot output_key_id matching works!" << std::endl;

    std::cout << "  ✅ P2TR IsMine works" << std::endl;
}

void test_watch_only() {
    std::cout << "\nTesting WATCH_ONLY ownership..." << std::endl;

    std::vector<uint8_t> script = hexToBytes("0014751e76e8199196d454941c45d1b3a323f1433bd6");

    MockKeyStore keystore;
    // NO master seed = watch-only wallet

    ScriptOwnershipResolver resolver(&keystore);

    // Add key but mark as NOT spendable
    auto key_id_bytes = hexToBytes("751e76e8199196d454941c45d1b3a323f1433bd6");
    KeyID key_id;
    std::copy(key_id_bytes.begin(), key_id_bytes.end(), key_id.begin());

    WalletKey wallet_key;
    wallet_key.id = key_id;
    wallet_key.spendable = false;  // Watch-only
    keystore.AddMockKey(wallet_key);

    // Should return WATCH_ONLY
    auto ownership = resolver.IsMine(script);
    assert(ownership == ScriptOwnership::WATCH_ONLY);
    std::cout << "  Watch-only key: WATCH_ONLY ✓" << std::endl;

    std::cout << "  ✅ WATCH_ONLY detection works" << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Script Ownership (IsMine) Unit Tests                ║" << std::endl;
    std::cout << "║  Week 1, Days 3-4 - Ownership Model                  ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    try {
        test_p2wpkh_extraction();
        test_p2tr_extraction();
        test_ismine_p2wpkh();
        test_ismine_p2tr();
        test_watch_only();

        std::cout << std::endl;
        std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                                  ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
        std::cout << std::endl;
        std::cout << "Week 1, Days 3-4 COMPLETE: IsMine ownership model implemented." << std::endl;
        std::cout << std::endl;
        std::cout << "🎯 KEY ACHIEVEMENT: Taproot output_key_id matching works!" << std::endl;
        std::cout << "   This is the foundation for fixing the Taproot spending bug." << std::endl;
        std::cout << std::endl;
        std::cout << "Next: Week 1 Day 5 - Implement WalletKeyStore in WalletManager" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
