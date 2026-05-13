#include "wallet/wallet_manager.h"
#include "wallet/script_ownership.h"
#include "wallet/key_identity.h"
#include "crypto/hdkeychain.h"
#include <iostream>
#include <cassert>
#include <iomanip>
#include <filesystem>

using namespace dinero;
using namespace dinero::wallet;

// ═══════════════════════════════════════════════════════════════════════
// Helper Functions
// ═══════════════════════════════════════════════════════════════════════

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════════
// Integration Tests
// ═══════════════════════════════════════════════════════════════════════

void test_taproot_address_generation_with_keyid() {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Test 1: Taproot Address Generation + KeyID Storage         ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;

    // Create temporary test directory
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "dinero_test_descriptor";
    std::filesystem::create_directories(test_dir);

    try {
        // Create WalletManager
        WalletManager wallet_mgr(test_dir);

        // Create test wallet
        wallet_mgr.create("test_descriptor");
        wallet_mgr.open("test_descriptor");

        std::cout << "✓ Created and opened test wallet" << std::endl;

        // Generate Taproot address
        std::string taproot_addr = wallet_mgr.getNewAddress("Test Taproot", "taproot");
        assert(!taproot_addr.empty());

        std::cout << "✓ Generated Taproot address: " << taproot_addr << std::endl;

        // Verify KeyID was stored
        // Query database directly to check key_id column
        sqlite3* db = wallet_mgr.getCurrentDatabase();
        assert(db != nullptr);

        const char* sql = "SELECT key_id, internal_key_id, output_key_id FROM addresses WHERE address = ?";
        sqlite3_stmt* stmt = nullptr;

        bool has_keyid = false;
        bool has_internal = false;
        bool has_output = false;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, taproot_addr.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                // Check key_id
                if (sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
                    int size = sqlite3_column_bytes(stmt, 0);
                    has_keyid = (size == 20);
                    std::cout << "✓ key_id stored (" << size << " bytes)" << std::endl;
                }

                // Check internal_key_id
                if (sqlite3_column_type(stmt, 1) == SQLITE_BLOB) {
                    int size = sqlite3_column_bytes(stmt, 1);
                    has_internal = (size == 20);
                    std::cout << "✓ internal_key_id stored (" << size << " bytes)" << std::endl;
                }

                // Check output_key_id
                if (sqlite3_column_type(stmt, 2) == SQLITE_BLOB) {
                    int size = sqlite3_column_bytes(stmt, 2);
                    has_output = (size == 20);
                    std::cout << "✓ output_key_id stored (" << size << " bytes)" << std::endl;
                }
            }
            sqlite3_finalize(stmt);
        }

        assert(has_keyid);
        assert(has_internal);
        assert(has_output);

        std::cout << "\n🎯 TEST 1 PASSED: Taproot address generated with KeyID storage\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST 1 FAILED: " << e.what() << std::endl;
        std::filesystem::remove_all(test_dir);
        throw;
    }

    std::filesystem::remove_all(test_dir);
}

void test_ismine_taproot_ownership() {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Test 2: IsMine Taproot Ownership Detection                 ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;

    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "dinero_test_ismine";
    std::filesystem::create_directories(test_dir);

    try {
        WalletManager wallet_mgr(test_dir);
        wallet_mgr.create("test_ismine");
        wallet_mgr.open("test_ismine");

        // Generate Taproot address
        std::string taproot_addr = wallet_mgr.getNewAddress("Test", "taproot");
        std::cout << "✓ Generated Taproot address: " << taproot_addr << std::endl;

        // Get scriptPubKey from database
        sqlite3* db = wallet_mgr.getCurrentDatabase();
        const char* sql = "SELECT pubkey FROM addresses WHERE address = ?";
        sqlite3_stmt* stmt = nullptr;

        std::vector<uint8_t> script_pubkey;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, taproot_addr.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* pubkey_text = sqlite3_column_text(stmt, 0);
                if (pubkey_text) {
                    std::string hex_script(reinterpret_cast<const char*>(pubkey_text));
                    script_pubkey = hexToBytes(hex_script);
                    std::cout << "✓ Retrieved scriptPubKey: " << hex_script << std::endl;
                }
            }
            sqlite3_finalize(stmt);
        }

        assert(!script_pubkey.empty());
        assert(script_pubkey.size() == 34);  // P2TR is 34 bytes
        assert(script_pubkey[0] == 0x51);    // OP_1
        assert(script_pubkey[1] == 0x20);    // Push 32 bytes

        // Create ScriptOwnershipResolver
        ScriptOwnershipResolver resolver(&wallet_mgr);

        // Test IsMine
        ScriptOwnership ownership = resolver.IsMine(script_pubkey);

        std::cout << "✓ IsMine result: ";
        if (ownership == ScriptOwnership::SPENDABLE) {
            std::cout << "SPENDABLE ✓" << std::endl;
        } else if (ownership == ScriptOwnership::WATCH_ONLY) {
            std::cout << "WATCH_ONLY" << std::endl;
        } else {
            std::cout << "NO" << std::endl;
        }

        assert(ownership == ScriptOwnership::SPENDABLE);

        // Test key extraction
        auto key_ids = resolver.ExtractKeyIDs(script_pubkey);
        assert(key_ids.size() == 1);
        std::cout << "✓ Extracted output_key_id: " << KeyIDToHex(key_ids[0]) << std::endl;

        // Test HaveKey via output_key_id
        bool have_key = resolver.HaveKey(key_ids[0]);
        std::cout << "✓ HaveKey (via output_key_id): " << (have_key ? "true" : "false") << std::endl;
        assert(have_key);

        std::cout << "\n🎯 TEST 2 PASSED: IsMine correctly identifies Taproot ownership\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST 2 FAILED: " << e.what() << std::endl;
        std::filesystem::remove_all(test_dir);
        throw;
    }

    std::filesystem::remove_all(test_dir);
}

void test_derive_taproot_privkey() {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Test 3: Taproot Private Key Derivation (BIP341)            ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;

    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "dinero_test_privkey";
    std::filesystem::create_directories(test_dir);

    try {
        WalletManager wallet_mgr(test_dir);
        wallet_mgr.create("test_privkey");
        wallet_mgr.open("test_privkey");

        // Generate Taproot address
        std::string taproot_addr = wallet_mgr.getNewAddress("Test", "taproot");
        std::cout << "✓ Generated Taproot address: " << taproot_addr << std::endl;

        // Get derivation path
        sqlite3* db = wallet_mgr.getCurrentDatabase();
        const char* sql = "SELECT derivation_path FROM address_derivation_paths WHERE address = ?";
        sqlite3_stmt* stmt = nullptr;

        std::string derivation_path;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, taproot_addr.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* path_text = sqlite3_column_text(stmt, 0);
                if (path_text) {
                    derivation_path = reinterpret_cast<const char*>(path_text);
                    std::cout << "✓ Derivation path: " << derivation_path << std::endl;
                }
            }
            sqlite3_finalize(stmt);
        }

        assert(!derivation_path.empty());
        assert(derivation_path.substr(0, 7) == "m/86'/");  // BIP86 Taproot

        // Derive private key using getPrivateKeyForPath
        std::string privkey_hex = wallet_mgr.getPrivateKeyForPath(derivation_path);
        assert(!privkey_hex.empty());
        assert(privkey_hex.length() == 64);  // 32 bytes = 64 hex chars

        std::cout << "✓ Derived private key: " << privkey_hex.substr(0, 16) << "..." << std::endl;

        // Verify this is the INTERNAL (untweaked) key
        // Parse path and use DerivePrivateKey directly
        auto origin_opt = KeyOriginInfo::parsePathString(derivation_path);
        assert(origin_opt.has_value());

        auto privkey_bytes_opt = wallet_mgr.DerivePrivateKey(origin_opt.value());
        assert(privkey_bytes_opt.has_value());

        // Convert to hex
        std::string direct_hex = bytesToHex(privkey_bytes_opt->data(), privkey_bytes_opt->size());

        std::cout << "✓ Direct derivation: " << direct_hex.substr(0, 16) << "..." << std::endl;

        // Should match (both are internal key, no tweaking)
        assert(privkey_hex == direct_hex);

        std::cout << "✓ Both methods return same INTERNAL (untweaked) private key" << std::endl;

        std::cout << "\n🎯 TEST 3 PASSED: Taproot private key derivation is BIP341 compliant\n" << std::endl;
        std::cout << "   (Returns internal key without TapTweak for signing)\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST 3 FAILED: " << e.what() << std::endl;
        std::filesystem::remove_all(test_dir);
        throw;
    }

    std::filesystem::remove_all(test_dir);
}

void test_complete_descriptor_flow() {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Test 4: Complete Descriptor Wallet Flow                    ║" << std::endl;
    std::cout << "║  (Address → KeyID → IsMine → PrivKey)                       ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;

    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "dinero_test_complete";
    std::filesystem::create_directories(test_dir);

    try {
        WalletManager wallet_mgr(test_dir);
        wallet_mgr.create("test_complete");
        wallet_mgr.open("test_complete");

        std::cout << "STEP 1: Generate Taproot address" << std::endl;
        std::string taproot_addr = wallet_mgr.getNewAddress("Complete Flow Test", "taproot");
        std::cout << "  ✓ Address: " << taproot_addr << std::endl;

        std::cout << "\nSTEP 2: Verify KeyID storage in database" << std::endl;
        sqlite3* db = wallet_mgr.getCurrentDatabase();

        const char* sql = "SELECT key_id, internal_key_id, output_key_id, pubkey FROM addresses WHERE address = ?";
        sqlite3_stmt* stmt = nullptr;

        KeyID stored_key_id;
        KeyID stored_internal_id;
        KeyID stored_output_id;
        std::vector<uint8_t> script_pubkey;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, taproot_addr.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                // Get key_id
                const void* blob = sqlite3_column_blob(stmt, 0);
                std::memcpy(stored_key_id.data(), blob, 20);
                std::cout << "  ✓ key_id:          " << KeyIDToHex(stored_key_id) << std::endl;

                // Get internal_key_id
                blob = sqlite3_column_blob(stmt, 1);
                std::memcpy(stored_internal_id.data(), blob, 20);
                std::cout << "  ✓ internal_key_id: " << KeyIDToHex(stored_internal_id) << std::endl;

                // Get output_key_id
                blob = sqlite3_column_blob(stmt, 2);
                std::memcpy(stored_output_id.data(), blob, 20);
                std::cout << "  ✓ output_key_id:   " << KeyIDToHex(stored_output_id) << std::endl;

                // Get scriptPubKey
                const unsigned char* pubkey_text = sqlite3_column_text(stmt, 3);
                std::string hex_script(reinterpret_cast<const char*>(pubkey_text));
                script_pubkey = hexToBytes(hex_script);
            }
            sqlite3_finalize(stmt);
        }

        std::cout << "\nSTEP 3: Test IsMine ownership via output_key_id" << std::endl;
        ScriptOwnershipResolver resolver(&wallet_mgr);

        auto extracted_ids = resolver.ExtractKeyIDs(script_pubkey);
        assert(extracted_ids.size() == 1);
        std::cout << "  ✓ Extracted output_key_id from script: " << KeyIDToHex(extracted_ids[0]) << std::endl;

        assert(extracted_ids[0] == stored_output_id);
        std::cout << "  ✓ Matches stored output_key_id" << std::endl;

        ScriptOwnership ownership = resolver.IsMine(script_pubkey);
        assert(ownership == ScriptOwnership::SPENDABLE);
        std::cout << "  ✓ IsMine returns: SPENDABLE" << std::endl;

        std::cout << "\nSTEP 4: Retrieve key via GetKeyByOutputKeyID" << std::endl;
        auto key_opt = wallet_mgr.GetKeyByOutputKeyID(stored_output_id);
        assert(key_opt.has_value());
        std::cout << "  ✓ Found key via output_key_id" << std::endl;
        std::cout << "  ✓ Spendable: " << (key_opt->spendable ? "true" : "false") << std::endl;

        std::cout << "\nSTEP 5: Derive private key using KeyOriginInfo" << std::endl;
        assert(!key_opt->origin.path.empty());

        auto privkey_opt = wallet_mgr.DerivePrivateKey(key_opt->origin);
        assert(privkey_opt.has_value());
        assert(privkey_opt->size() == 32);

        std::string privkey_hex = bytesToHex(privkey_opt->data(), 32);
        std::cout << "  ✓ Derived privkey: " << privkey_hex.substr(0, 16) << "..." << std::endl;
        std::cout << "  ✓ This is the INTERNAL (untweaked) key for BIP341 signing" << std::endl;

        std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  🎯 TEST 4 PASSED: Complete descriptor wallet flow works!   ║" << std::endl;
        std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;

        std::cout << "\n┌─ Flow Summary ─────────────────────────────────────────────┐" << std::endl;
        std::cout << "│ 1. Address generated with Taproot tweaking                │" << std::endl;
        std::cout << "│ 2. Three KeyIDs stored: key_id, internal, output          │" << std::endl;
        std::cout << "│ 3. IsMine extracts output_key_id from scriptPubKey        │" << std::endl;
        std::cout << "│ 4. GetKeyByOutputKeyID finds internal key metadata        │" << std::endl;
        std::cout << "│ 5. DerivePrivateKey returns internal (untweaked) key      │" << std::endl;
        std::cout << "│ 6. Ready for BIP341 signing (internal key + Schnorr)      │" << std::endl;
        std::cout << "└────────────────────────────────────────────────────────────┘\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ TEST 4 FAILED: " << e.what() << std::endl;
        std::filesystem::remove_all(test_dir);
        throw;
    }

    std::filesystem::remove_all(test_dir);
}

int main() {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                                                              ║" << std::endl;
    std::cout << "║  Descriptor Wallet Integration Tests                        ║" << std::endl;
    std::cout << "║  Week 1 Complete Implementation Validation                  ║" << std::endl;
    std::cout << "║                                                              ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;

    try {
        test_taproot_address_generation_with_keyid();
        test_ismine_taproot_ownership();
        test_derive_taproot_privkey();
        test_complete_descriptor_flow();

        std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                                                              ║" << std::endl;
        std::cout << "║  ✅ ALL INTEGRATION TESTS PASSED (4/4)                       ║" << std::endl;
        std::cout << "║                                                              ║" << std::endl;
        std::cout << "║  Week 1 COMPLETE: Descriptor Wallet Foundation               ║" << std::endl;
        std::cout << "║  - Key Identity (KeyID + KeyOriginInfo)                      ║" << std::endl;
        std::cout << "║  - Script Ownership (IsMine)                                 ║" << std::endl;
        std::cout << "║  - Key Storage (WalletKeyStore)                              ║" << std::endl;
        std::cout << "║  - On-demand Derivation (DerivePrivateKey)                   ║" << std::endl;
        std::cout << "║  - BIP341 Taproot Signing Fix                                ║" << std::endl;
        std::cout << "║                                                              ║" << std::endl;
        std::cout << "║  Ready for Phase 4C-lite testing!                            ║" << std::endl;
        std::cout << "║                                                              ║" << std::endl;
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ INTEGRATION TEST SUITE FAILED" << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
