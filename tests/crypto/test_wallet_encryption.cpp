// Comprehensive Wallet Encryption Tests
// Tests Argon2id KDF + AES-256-GCM + Auto-lock
// December 1, 2025

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include "wallet/wallet_manager.h"
#include "crypto/wallet_crypto.h"

namespace fs = std::filesystem;

// Helper: Clean test directory
void cleanupTestDir(const std::string& test_dir) {
    if (fs::exists(test_dir)) {
        fs::remove_all(test_dir);
    }
    fs::create_directories(test_dir);
}

// Test 1: Argon2id KDF produces consistent keys
void test_argon2id_consistency() {
    std::cout << "\n[Test 1] Argon2id KDF Consistency" << std::endl;
    
    std::string password = "TestPassword123!";
    std::vector<uint8_t> salt(32, 0xAB);  // Fixed salt for testing
    
    // Derive key twice with same password + salt
    std::array<uint8_t, 32> key1, key2;
    bool success1 = dinero::crypto::deriveKeyArgon2id(password, salt, 3, 65536, 1, key1);
    bool success2 = dinero::crypto::deriveKeyArgon2id(password, salt, 3, 65536, 1, key2);
    
    assert(success1 && "First key derivation failed");
    assert(success2 && "Second key derivation failed");
    assert(key1 == key2 && "Keys should match with same password+salt");
    
    std::cout << "✅ Argon2id produces consistent keys" << std::endl;
}

// Test 2: Different passwords produce different keys
void test_argon2id_uniqueness() {
    std::cout << "\n[Test 2] Argon2id Password Uniqueness" << std::endl;
    
    std::vector<uint8_t> salt(32, 0xCD);
    
    std::array<uint8_t, 32> key1, key2;
    dinero::crypto::deriveKeyArgon2id("password1", salt, 3, 65536, 1, key1);
    dinero::crypto::deriveKeyArgon2id("password2", salt, 3, 65536, 1, key2);
    
    assert(key1 != key2 && "Different passwords must produce different keys");
    
    std::cout << "✅ Different passwords produce unique keys" << std::endl;
}

// Test 3: AES-256-GCM encryption roundtrip
void test_aes_gcm_roundtrip() {
    std::cout << "\n[Test 3] AES-256-GCM Encryption Roundtrip" << std::endl;
    
    std::string plaintext = "This is secret wallet data 🔐";
    std::vector<uint8_t> plaintext_vec(plaintext.begin(), plaintext.end());
    
    // Derive encryption key
    std::vector<uint8_t> salt(32, 0xEF);
    std::array<uint8_t, 32> key;
    dinero::crypto::deriveKeyArgon2id("TestPass", salt, 3, 65536, 1, key);
    
    // Generate random nonce
    std::vector<uint8_t> nonce(12);
    for (size_t i = 0; i < 12; i++) nonce[i] = i + 100;
    
    // Encrypt
    std::vector<uint8_t> ciphertext = dinero::crypto::encryptAesGcm(plaintext_vec, key, nonce);
    
    // Decrypt
    std::vector<uint8_t> decrypted = dinero::crypto::decryptAesGcm(ciphertext, key, nonce);
    
    std::string decrypted_str(decrypted.begin(), decrypted.end());
    assert(decrypted_str == plaintext && "Decrypted data must match original");
    
    std::cout << "✅ AES-256-GCM encryption/decryption works" << std::endl;
}

// Test 4: AES-GCM authentication (tamper detection)
void test_aes_gcm_authentication() {
    std::cout << "\n[Test 4] AES-GCM Authentication (Tamper Detection)" << std::endl;
    
    std::string plaintext = "Protected data";
    std::vector<uint8_t> plaintext_vec(plaintext.begin(), plaintext.end());
    
    std::vector<uint8_t> salt(32, 0x11);
    std::array<uint8_t, 32> key;
    dinero::crypto::deriveKeyArgon2id("SecretKey", salt, 3, 65536, 1, key);
    
    std::vector<uint8_t> nonce(12, 0x22);
    std::vector<uint8_t> ciphertext = dinero::crypto::encryptAesGcm(plaintext_vec, key, nonce);
    
    // Tamper with ciphertext (flip a bit)
    ciphertext[5] ^= 0x01;
    
    // Attempt to decrypt tampered data
    bool caught_exception = false;
    try {
        dinero::crypto::decryptAesGcm(ciphertext, key, nonce);
    } catch (const std::runtime_error& e) {
        caught_exception = true;
        std::string error_msg = e.what();
        assert(error_msg.find("Authentication failed") != std::string::npos);
    }
    
    assert(caught_exception && "Tampered data should fail authentication");
    
    std::cout << "✅ AES-GCM detects tampered data" << std::endl;
}

// Test 5: Wallet encryption lifecycle
void test_wallet_encryption_lifecycle() {
    std::cout << "\n[Test 5] Wallet Encryption Lifecycle" << std::endl;
    
    std::string test_dir = "./test_data/wallet_encryption";
    cleanupTestDir(test_dir);
    
    // Create wallet
    dinero::WalletManager wallet(test_dir);
    wallet.create("test_wallet");
    wallet.open("test_wallet");
    
    assert(!wallet.isWalletEncrypted() && "New wallet should not be encrypted");
    assert(!wallet.isWalletLocked() && "Unencrypted wallet should not be locked");
    
    // Encrypt wallet
    std::string passphrase = "MySecurePassword123!";
    wallet.encryptWallet(passphrase);
    
    assert(wallet.isWalletEncrypted() && "Wallet should be encrypted");
    assert(!wallet.isWalletLocked() && "Wallet should be unlocked after encryption");
    
    // Lock wallet
    wallet.lockWallet();
    assert(wallet.isWalletLocked() && "Wallet should be locked");
    
    // Attempt operation while locked (should fail)
    bool caught_exception = false;
    try {
        wallet.getNewAddress();
    } catch (const std::runtime_error& e) {
        caught_exception = true;
    }
    assert(caught_exception && "Operations should fail on locked wallet");
    
    // Unlock wallet
    wallet.unlockWallet(passphrase);
    assert(!wallet.isWalletLocked() && "Wallet should be unlocked");
    
    // Now operation should work
    std::string addr = wallet.getNewAddress();
    assert(!addr.empty() && "Should be able to generate address when unlocked");
    
    // Lock again
    wallet.lockWallet();
    assert(wallet.isWalletLocked() && "Wallet should be locked again");
    
    std::cout << "✅ Wallet encryption lifecycle works" << std::endl;
}

// Test 6: Wrong passphrase rejection
void test_wrong_passphrase() {
    std::cout << "\n[Test 6] Wrong Passphrase Rejection" << std::endl;
    
    std::string test_dir = "./test_data/wallet_wrong_pass";
    cleanupTestDir(test_dir);
    
    dinero::WalletManager wallet(test_dir);
    wallet.create("test_wallet");
    wallet.open("test_wallet");
    
    std::string correct_pass = "CorrectPassword123";
    std::string wrong_pass = "WrongPassword456";
    
    wallet.encryptWallet(correct_pass);
    wallet.lockWallet();
    
    // Try to unlock with wrong password
    bool caught_exception = false;
    try {
        wallet.unlockWallet(wrong_pass);
    } catch (const std::runtime_error& e) {
        caught_exception = true;
        std::string error_msg = e.what();
        assert(error_msg.find("Invalid passphrase") != std::string::npos);
    }
    
    assert(caught_exception && "Wrong passphrase should be rejected");
    assert(wallet.isWalletLocked() && "Wallet should remain locked after wrong password");
    
    // Correct password should work
    wallet.unlockWallet(correct_pass);
    assert(!wallet.isWalletLocked() && "Correct password should unlock wallet");
    
    std::cout << "✅ Wrong passphrase correctly rejected" << std::endl;
}

// Test 7: Passphrase change
void test_passphrase_change() {
    std::cout << "\n[Test 7] Passphrase Change" << std::endl;
    
    std::string test_dir = "./test_data/wallet_pass_change";
    cleanupTestDir(test_dir);
    
    dinero::WalletManager wallet(test_dir);
    wallet.create("test_wallet");
    wallet.open("test_wallet");
    
    std::string old_pass = "OldPassword123";
    std::string new_pass = "NewPassword456";
    
    wallet.encryptWallet(old_pass);
    wallet.changePassphrase(old_pass, new_pass);
    
    wallet.lockWallet();
    
    // Old password should fail
    bool old_failed = false;
    try {
        wallet.unlockWallet(old_pass);
    } catch (const std::runtime_error&) {
        old_failed = true;
    }
    assert(old_failed && "Old password should not work after change");
    
    // New password should work
    wallet.unlockWallet(new_pass);
    assert(!wallet.isWalletLocked() && "New password should unlock wallet");
    
    std::cout << "✅ Passphrase change works correctly" << std::endl;
}

// Test 8: Auto-lock timeout
void test_auto_lock_timeout() {
    std::cout << "\n[Test 8] Auto-Lock Timeout" << std::endl;
    
    std::string test_dir = "./test_data/wallet_auto_lock";
    cleanupTestDir(test_dir);
    
    dinero::WalletManager wallet(test_dir);
    wallet.create("test_wallet");
    wallet.open("test_wallet");
    
    std::string passphrase = "TestPassword";
    wallet.encryptWallet(passphrase);
    wallet.lockWallet();
    
    // Unlock with 2-second timeout
    wallet.unlockWallet(passphrase, 2);
    assert(!wallet.isWalletLocked() && "Wallet should be unlocked");
    
    // Wait 1 second (should still be unlocked)
    std::this_thread::sleep_for(std::chrono::seconds(1));
    assert(!wallet.isWalletLocked() && "Wallet should still be unlocked after 1 second");
    
    // Wait another 2 seconds (total 3 seconds, should auto-lock)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    assert(wallet.isWalletLocked() && "Wallet should auto-lock after timeout");
    
    std::cout << "✅ Auto-lock timeout works" << std::endl;
}

// Test 9: Persistent encryption across restarts
void test_encryption_persistence() {
    std::cout << "\n[Test 9] Encryption Persistence Across Restarts" << std::endl;
    
    std::string test_dir = "./test_data/wallet_persistence";
    cleanupTestDir(test_dir);
    
    std::string passphrase = "PersistentPassword";
    
    // Create and encrypt wallet
    {
        dinero::WalletManager wallet(test_dir);
        wallet.create("test_wallet");
        wallet.open("test_wallet");
        wallet.encryptWallet(passphrase);
        // Wallet manager destroyed (simulates daemon restart)
    }
    
    // Open wallet again (fresh instance)
    {
        dinero::WalletManager wallet(test_dir);
        wallet.open("test_wallet");
        
        assert(wallet.isWalletEncrypted() && "Wallet should still be encrypted after restart");
        assert(wallet.isWalletLocked() && "Wallet should be locked after restart");
        
        // Unlock with original passphrase
        wallet.unlockWallet(passphrase);
        assert(!wallet.isWalletLocked() && "Should unlock with original passphrase");
    }
    
    std::cout << "✅ Encryption persists across restarts" << std::endl;
}

// Test 10: Empty passphrase rejection
void test_empty_passphrase() {
    std::cout << "\n[Test 10] Empty Passphrase Rejection" << std::endl;
    
    std::string test_dir = "./test_data/wallet_empty_pass";
    cleanupTestDir(test_dir);
    
    dinero::WalletManager wallet(test_dir);
    wallet.create("test_wallet");
    wallet.open("test_wallet");
    
    bool caught_exception = false;
    try {
        wallet.encryptWallet("");
    } catch (const std::runtime_error& e) {
        caught_exception = true;
        std::string error_msg = e.what();
        assert(error_msg.find("cannot be empty") != std::string::npos);
    }
    
    assert(caught_exception && "Empty passphrase should be rejected");
    assert(!wallet.isWalletEncrypted() && "Wallet should not be encrypted");
    
    std::cout << "✅ Empty passphrase correctly rejected" << std::endl;
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
    std::cout << "  Dinero Wallet Encryption Test Suite" << std::endl;
    std::cout << "  Argon2id + AES-256-GCM + Auto-Lock" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
    
    try {
        // Crypto primitive tests
        test_argon2id_consistency();
        test_argon2id_uniqueness();
        test_aes_gcm_roundtrip();
        test_aes_gcm_authentication();
        
        // Wallet integration tests
        test_wallet_encryption_lifecycle();
        test_wrong_passphrase();
        test_passphrase_change();
        test_auto_lock_timeout();
        test_encryption_persistence();
        test_empty_passphrase();
        
        std::cout << "\n═══════════════════════════════════════════════════════" << std::endl;
        std::cout << "  ✅ ALL TESTS PASSED (10/10)" << std::endl;
        std::cout << "═══════════════════════════════════════════════════════" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}

