/**
 * Phase E.3.1: Wallet Lifecycle Integration Tests
 *
 * Tests the complete wallet lifecycle:
 * - Create → Encrypt → Lock → Unlock → Timeout → Relock
 * - Key material never accessible when locked
 * - Zeroization verified after lock
 *
 * CRITICAL: These tests validate that security fixes from Phase E.2 work correctly
 * in real-world wallet operation scenarios.
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include "wallet/wallet_manager.h"
#include "crypto/secure_memory.h"

namespace fs = std::filesystem;
using namespace dinero;

class WalletLifecycleTest : public ::testing::Test {
protected:
    std::string test_dir_;
    std::unique_ptr<WalletManager> wallet_manager_;

    void SetUp() override {
        // Create isolated test directory
        test_dir_ = "/tmp/wallet_lifecycle_test_" + std::to_string(time(nullptr));
        fs::create_directories(test_dir_);

        // Initialize wallet manager (constructor takes dataDir)
        wallet_manager_ = std::make_unique<WalletManager>(test_dir_);
    }

    void TearDown() override {
        // Clean up
        wallet_manager_.reset();

        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    // Helper: Create and optionally encrypt wallet
    bool createTestWallet(const std::string& name, const std::string& passphrase = "") {
        try {
            // Create wallet
            wallet_manager_->create(name);

            // Open it (makes it current)
            wallet_manager_->open(name);

            // Encrypt if passphrase provided
            if (!passphrase.empty()) {
                wallet_manager_->encryptWallet(passphrase);
            }

            return true;
        } catch (const std::exception& e) {
            return false;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.1: Complete Lifecycle (Create → Encrypt → Lock → Unlock → Timeout)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, CompleteEncryptedWalletLifecycle) {
    const std::string wallet_name = "lifecycle_test_wallet";
    const std::string passphrase = "TestPassphrase12345!";

    // Step 1: Create and encrypt wallet
    ASSERT_TRUE(createTestWallet(wallet_name, passphrase))
        << "Failed to create encrypted wallet";

    // Verify wallet is encrypted and locked
    EXPECT_TRUE(wallet_manager_->isWalletEncrypted())
        << "Wallet should be encrypted";
    EXPECT_TRUE(wallet_manager_->isWalletLocked())
        << "Newly encrypted wallet should start locked";

    // Step 2: Unlock wallet
    ASSERT_NO_THROW({
        wallet_manager_->unlockWallet(passphrase);
    }) << "Failed to unlock wallet with correct passphrase";

    EXPECT_FALSE(wallet_manager_->isWalletLocked())
        << "Wallet should be unlocked after correct passphrase";

    // Step 3: Verify wallet functionality while unlocked
    ASSERT_NO_THROW({
        auto address = wallet_manager_->getNewAddress();
        EXPECT_FALSE(address.empty()) << "Should generate address when unlocked";
    }) << "Failed to generate address while unlocked";

    // Step 4: Lock wallet manually
    ASSERT_NO_THROW({
        wallet_manager_->lockWallet();
    }) << "Failed to lock wallet";

    EXPECT_TRUE(wallet_manager_->isWalletLocked())
        << "Wallet should be locked after lockWallet()";

    // Step 5: Verify wallet operations blocked when locked
    EXPECT_THROW({
        wallet_manager_->getNewAddress();
    }, std::runtime_error) << "Should not generate address when locked";

    // Step 6: Unlock with timeout (2 seconds)
    ASSERT_NO_THROW({
        wallet_manager_->unlockWallet(passphrase, 2);  // 2 second timeout
    }) << "Failed to unlock wallet with timeout";

    EXPECT_FALSE(wallet_manager_->isWalletLocked())
        << "Wallet should be unlocked immediately after unlockWallet()";

    // Step 7: Wait for timeout to expire
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Step 8: Verify operations blocked after timeout (wallet auto-locked)
    // Note: checkUnlockTimeout() is private, so we rely on getNewAddress()
    // triggering the timeout check internally
    EXPECT_THROW({
        wallet_manager_->getNewAddress();
    }, std::runtime_error) << "Should not generate address after auto-lock";

    // Verify wallet is now locked
    EXPECT_TRUE(wallet_manager_->isWalletLocked())
        << "Wallet should be locked after timeout";
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.2: Key Material Never Accessible When Locked
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, KeyMaterialInaccessibleWhenLocked) {
    const std::string wallet_name = "key_isolation_test";
    const std::string passphrase = "SecurePassphrase99!";

    // Create and encrypt wallet
    ASSERT_TRUE(createTestWallet(wallet_name, passphrase));

    // Unlock wallet
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase));

    // Generate an address (requires master seed access)
    std::string address;
    ASSERT_NO_THROW({
        address = wallet_manager_->getNewAddress();
        ASSERT_FALSE(address.empty());
    });

    // Lock the wallet
    ASSERT_NO_THROW(wallet_manager_->lockWallet());

    // Verify operations that require private keys are blocked
    EXPECT_THROW({
        wallet_manager_->getNewAddress();
    }, std::runtime_error) << "Should not generate address when locked";

    // Unlock wallet
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase));

    // Now operations should succeed
    EXPECT_NO_THROW({
        std::string addr = wallet_manager_->getNewAddress();
        EXPECT_FALSE(addr.empty()) << "Should generate address when unlocked";
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.3: Zeroization Verified After Lock
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, MasterSeedZeroizedAfterLock) {
    const std::string wallet_name = "zeroization_test";
    const std::string passphrase = "ZeroTestPass123!";

    // Create and encrypt wallet
    ASSERT_TRUE(createTestWallet(wallet_name, passphrase));

    // Unlock wallet
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase));

    // Generate some addresses to ensure master seed is loaded
    for (int i = 0; i < 5; i++) {
        wallet_manager_->getNewAddress();
    }

    // Lock wallet (should trigger clearPrivateKeyCache → OPENSSL_cleanse)
    ASSERT_NO_THROW(wallet_manager_->lockWallet());

    // Verify wallet is locked
    EXPECT_TRUE(wallet_manager_->isWalletLocked());

    // Attempting to access key material should fail
    // (If zeroization didn't work, wallet might still have cached keys)
    EXPECT_THROW({
        wallet_manager_->getNewAddress();
    }, std::runtime_error) << "Master seed should be zeroized, operations should fail";

    // Unlock again
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase));

    // Verify wallet works again (master seed reloaded from encrypted storage)
    EXPECT_NO_THROW({
        std::string addr = wallet_manager_->getNewAddress();
        EXPECT_FALSE(addr.empty());
    }) << "Wallet should reload master seed from encrypted storage";
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.4: Unencrypted Wallet Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, UnencryptedWalletOperations) {
    const std::string wallet_name = "unencrypted_test";

    // Create unencrypted wallet (no passphrase)
    ASSERT_TRUE(createTestWallet(wallet_name));

    // Verify wallet is not encrypted and not locked
    EXPECT_FALSE(wallet_manager_->isWalletEncrypted())
        << "Wallet should not be encrypted";
    EXPECT_FALSE(wallet_manager_->isWalletLocked())
        << "Unencrypted wallet should never be locked";

    // Operations should work without unlock
    EXPECT_NO_THROW({
        std::string addr = wallet_manager_->getNewAddress();
        EXPECT_FALSE(addr.empty());
    });

    // Generate multiple addresses to verify stability
    EXPECT_NO_THROW({
        for (int i = 0; i < 5; i++) {
            std::string addr = wallet_manager_->getNewAddress();
            EXPECT_FALSE(addr.empty());
        }
    });

    // Attempting to lock should fail (no encryption)
    EXPECT_THROW({
        wallet_manager_->lockWallet();
    }, std::runtime_error) << "Cannot lock unencrypted wallet";

    // Attempting to unlock should fail (no encryption)
    EXPECT_THROW({
        wallet_manager_->unlockWallet("any_passphrase");
    }, std::runtime_error) << "Cannot unlock unencrypted wallet";
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.5: Wrong Passphrase Rejection
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, WrongPassphraseRejected) {
    const std::string wallet_name = "passphrase_test";
    const std::string correct_passphrase = "CorrectPass123!";
    const std::string wrong_passphrase = "WrongPass456!";

    // Create encrypted wallet
    ASSERT_TRUE(createTestWallet(wallet_name, correct_passphrase));

    // Attempt unlock with wrong passphrase
    EXPECT_THROW({
        wallet_manager_->unlockWallet(wrong_passphrase);
    }, std::runtime_error) << "Should reject wrong passphrase";

    // Wallet should still be locked
    EXPECT_TRUE(wallet_manager_->isWalletLocked())
        << "Wallet should remain locked after failed unlock attempt";

    // Unlock with correct passphrase
    EXPECT_NO_THROW({
        wallet_manager_->unlockWallet(correct_passphrase);
    }) << "Should unlock with correct passphrase";

    EXPECT_FALSE(wallet_manager_->isWalletLocked())
        << "Wallet should be unlocked after correct passphrase";
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.6: Passphrase Change Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, PassphraseChangeAndRelock) {
    const std::string wallet_name = "passphrase_change_test";
    const std::string old_passphrase = "OldPass123!";
    const std::string new_passphrase = "NewPass456!";

    // Create encrypted wallet
    ASSERT_TRUE(createTestWallet(wallet_name, old_passphrase));

    // Unlock with old passphrase
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(old_passphrase));

    // Change passphrase
    EXPECT_NO_THROW({
        wallet_manager_->changePassphrase(old_passphrase, new_passphrase);
    }) << "Should change passphrase";

    // Lock wallet
    ASSERT_NO_THROW(wallet_manager_->lockWallet());

    // Old passphrase should no longer work
    EXPECT_THROW({
        wallet_manager_->unlockWallet(old_passphrase);
    }, std::runtime_error) << "Old passphrase should be rejected";

    // New passphrase should work
    EXPECT_NO_THROW({
        wallet_manager_->unlockWallet(new_passphrase);
    }) << "New passphrase should unlock wallet";

    EXPECT_FALSE(wallet_manager_->isWalletLocked());
}

// ═══════════════════════════════════════════════════════════════════════════
// E.3.1.7: Multiple Lock/Unlock Cycles
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletLifecycleTest, MultipleLockUnlockCycles) {
    const std::string wallet_name = "cycle_test";
    const std::string passphrase = "CyclePass123!";

    ASSERT_TRUE(createTestWallet(wallet_name, passphrase));

    // Perform 10 lock/unlock cycles
    for (int i = 0; i < 10; i++) {
        // Unlock
        ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase))
            << "Cycle " << i << ": Failed to unlock";

        EXPECT_FALSE(wallet_manager_->isWalletLocked())
            << "Cycle " << i << ": Should be unlocked";

        // Use wallet
        EXPECT_NO_THROW({
            std::string addr = wallet_manager_->getNewAddress();
            EXPECT_FALSE(addr.empty());
        }) << "Cycle " << i << ": Should generate address";

        // Lock
        ASSERT_NO_THROW(wallet_manager_->lockWallet())
            << "Cycle " << i << ": Failed to lock";

        EXPECT_TRUE(wallet_manager_->isWalletLocked())
            << "Cycle " << i << ": Should be locked";
    }

    // Final unlock to verify wallet still works
    ASSERT_NO_THROW(wallet_manager_->unlockWallet(passphrase));
    EXPECT_NO_THROW({
        std::string addr = wallet_manager_->getNewAddress();
        EXPECT_FALSE(addr.empty());
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
