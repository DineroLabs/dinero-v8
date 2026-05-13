#include <gtest/gtest.h>
#include "wallet/reference/wallet.h"
#include "wallet/reference/crypto.h"
#include "wallet/reference/database.h"
#include "wallet/reference/utxo_manager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>

using namespace dinero::wallet::reference;
namespace fs = std::filesystem;

// Test fixture for wallet tests
class ReferenceWalletTest : public ::testing::Test {
protected:
    std::string test_dir = "./test_wallets";

    void SetUp() override {
        // Create test directory
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        // Clean up test wallets
        fs::remove_all(test_dir);
    }

    std::string GetTestPath(const std::string& wallet_name) {
        // Return just the test_dir, wallet will append wallet_name.db
        return test_dir;
    }
};

// =============================================================================
// BIP39 Test Vectors (Official)
// =============================================================================
// Source: https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki

TEST_F(ReferenceWalletTest, BIP39_TestVector1_EnglishMnemonic) {
    // Official BIP39 Test Vector #1 (English)
    // Source: https://github.com/trezor/python-mnemonic/blob/master/vectors.json
    // Entropy: 00000000000000000000000000000000
    // Mnemonic: abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about
    // Passphrase: TREZOR (ALL official BIP39 test vectors use "TREZOR" as passphrase)
    // Seed: c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04

    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    // Validate mnemonic
    EXPECT_TRUE(crypto::BIP39::ValidateMnemonic(mnemonic));

    // Convert to seed with passphrase "TREZOR" (official BIP39 test vectors use this)
    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, "TREZOR");

    // Expected full 64-byte seed from BIP39 spec
    std::vector<uint8_t> expected_seed = {
        0xc5, 0x52, 0x57, 0xc3, 0x60, 0xc0, 0x7c, 0x72,
        0x02, 0x9a, 0xeb, 0xc1, 0xb5, 0x3c, 0x05, 0xed,
        0x03, 0x62, 0xad, 0xa3, 0x8e, 0xad, 0x3e, 0x3e,
        0x9e, 0xfa, 0x37, 0x08, 0xe5, 0x34, 0x95, 0x53,
        0x1f, 0x09, 0xa6, 0x98, 0x75, 0x99, 0xd1, 0x82,
        0x64, 0xc1, 0xe1, 0xc9, 0x2f, 0x2c, 0xf1, 0x41,
        0x63, 0x0c, 0x7a, 0x3c, 0x4a, 0xb7, 0xc8, 0x1b,
        0x2f, 0x00, 0x16, 0x98, 0xe7, 0x46, 0x3b, 0x04
    };

    // Verify all 64 bytes match
    ASSERT_EQ(seed.size(), 64);
    for (size_t i = 0; i < 64; i++) {
        EXPECT_EQ(seed[i], expected_seed[i]) << "Mismatch at byte " << i;
    }
}

TEST_F(ReferenceWalletTest, BIP39_EmptyPassphrase) {
    // Test with EMPTY passphrase (different from "TREZOR")
    // This verifies that empty passphrase produces a different seed
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, "");

    // Expected seed with EMPTY passphrase (verified with Python hashlib.pbkdf2_hmac)
    std::vector<uint8_t> expected_seed = {
        0x5e, 0xb0, 0x0b, 0xbd, 0xdc, 0xf0, 0x69, 0x08,
        0x48, 0x89, 0xa8, 0xab, 0x91, 0x55, 0x56, 0x81,
        0x65, 0xf5, 0xc4, 0x53, 0xcc, 0xb8, 0x5e, 0x70,
        0x81, 0x1a, 0xae, 0xd6, 0xf6, 0xda, 0x5f, 0xc1,
        0x9a, 0x5a, 0xc4, 0x0b, 0x38, 0x9c, 0xd3, 0x70,
        0xd0, 0x86, 0x20, 0x6d, 0xec, 0x8a, 0xa6, 0xc4,
        0x3d, 0xae, 0xa6, 0x69, 0x0f, 0x20, 0xad, 0x3d,
        0x8d, 0x48, 0xb2, 0xd2, 0xce, 0x9e, 0x38, 0xe4
    };

    ASSERT_EQ(seed.size(), 64);
    for (size_t i = 0; i < 64; i++) {
        EXPECT_EQ(seed[i], expected_seed[i]) << "Mismatch at byte " << i;
    }
}

TEST_F(ReferenceWalletTest, BIP39_InvalidMnemonic) {
    // Invalid checksum
    std::string invalid = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon";
    EXPECT_FALSE(crypto::BIP39::ValidateMnemonic(invalid));

    // Invalid word
    std::string invalid2 = "abandon abandon abandon invalid abandon abandon abandon abandon abandon abandon abandon about";
    EXPECT_FALSE(crypto::BIP39::ValidateMnemonic(invalid2));
}

TEST_F(ReferenceWalletTest, BIP39_GenerateMnemonic) {
    // Generate 12-word mnemonic
    auto mnemonic12 = crypto::BIP39::GenerateMnemonic(12);
    EXPECT_FALSE(mnemonic12.empty());
    EXPECT_TRUE(crypto::BIP39::ValidateMnemonic(mnemonic12));

    // Count words
    size_t word_count = 1;
    for (char c : mnemonic12) {
        if (c == ' ') word_count++;
    }
    EXPECT_EQ(word_count, 12);

    // Generate 24-word mnemonic
    auto mnemonic24 = crypto::BIP39::GenerateMnemonic(24);
    EXPECT_FALSE(mnemonic24.empty());
    EXPECT_TRUE(crypto::BIP39::ValidateMnemonic(mnemonic24));

    // Different calls should produce different mnemonics
    auto mnemonic24_2 = crypto::BIP39::GenerateMnemonic(24);
    EXPECT_NE(mnemonic24, mnemonic24_2);
}

// =============================================================================
// BIP32 HD Key Derivation Tests
// =============================================================================

TEST_F(ReferenceWalletTest, BIP32_MasterKeyDerivation) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, "");

    // Derive master key
    auto master = crypto::BIP32::MasterKeyFromSeed(seed);

    // Master key should have 32-byte private key and 33-byte compressed pubkey
    EXPECT_EQ(master.private_key.size(), 32);
    EXPECT_EQ(master.public_key.size(), 33);
    EXPECT_EQ(master.chain_code.size(), 32);

    // Public key should start with 0x02 or 0x03 (compressed format)
    EXPECT_TRUE(master.public_key[0] == 0x02 || master.public_key[0] == 0x03);
}

TEST_F(ReferenceWalletTest, BIP32_ChildDerivation) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, "");
    auto master = crypto::BIP32::MasterKeyFromSeed(seed);

    // Derive child key at index 0 (non-hardened)
    auto child0 = crypto::BIP32::DeriveChild(master, 0);
    EXPECT_EQ(child0.private_key.size(), 32);
    EXPECT_EQ(child0.public_key.size(), 33);

    // Derive child key at index 0' (hardened)
    auto child0h = crypto::BIP32::DeriveChild(master, 0x80000000);
    EXPECT_EQ(child0h.private_key.size(), 32);
    EXPECT_EQ(child0h.public_key.size(), 33);

    // Non-hardened and hardened should be different
    EXPECT_NE(child0.private_key, child0h.private_key);
}

TEST_F(ReferenceWalletTest, BIP32_BIP84Path) {
    // Test BIP84 derivation path: m/84'/1447'/0'/0/0
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    auto seed = crypto::BIP39::MnemonicToSeed(mnemonic, "");
    auto master = crypto::BIP32::MasterKeyFromSeed(seed);

    // Derive: m/84'/1447'/0'/0/0
    auto derived = crypto::BIP32::DerivePath(master, "m/84'/1447'/0'/0/0");

    EXPECT_EQ(derived.private_key.size(), 32);
    EXPECT_EQ(derived.public_key.size(), 33);

    // Should produce a valid address
    auto address = crypto::Address::PublicKeyToAddress(derived.public_key);
    EXPECT_FALSE(address.empty());
    EXPECT_TRUE(address.substr(0, 4) == "din1");
}

// =============================================================================
// Determinism Tests - CRITICAL for Reference Wallet
// =============================================================================

TEST_F(ReferenceWalletTest, Determinism_SameSeed_SameAddress) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    auto wallet1 = ReferenceWallet::CreateFromMnemonic("wallet1", mnemonic, "", GetTestPath("wallet1"));
    auto wallet2 = ReferenceWallet::CreateFromMnemonic("wallet2", mnemonic, "", GetTestPath("wallet2"));

    // Same mnemonic must produce same address
    EXPECT_EQ(wallet1->GetAddress(), wallet2->GetAddress());
}

TEST_F(ReferenceWalletTest, Determinism_DifferentSeed_DifferentAddress) {
    std::string mnemonic1 = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    std::string mnemonic2 = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong";

    auto wallet1 = ReferenceWallet::CreateFromMnemonic("wallet1", mnemonic1, "", GetTestPath("wallet1"));
    auto wallet2 = ReferenceWallet::CreateFromMnemonic("wallet2", mnemonic2, "", GetTestPath("wallet2"));

    // Different mnemonics must produce different addresses
    EXPECT_NE(wallet1->GetAddress(), wallet2->GetAddress());
}

TEST_F(ReferenceWalletTest, Determinism_SamePassphrase_SameAddress) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    std::string passphrase = "my secret passphrase";

    auto wallet1 = ReferenceWallet::CreateFromMnemonic("wallet1", mnemonic, passphrase, GetTestPath("wallet1"));
    auto wallet2 = ReferenceWallet::CreateFromMnemonic("wallet2", mnemonic, passphrase, GetTestPath("wallet2"));

    EXPECT_EQ(wallet1->GetAddress(), wallet2->GetAddress());
}

TEST_F(ReferenceWalletTest, Determinism_DifferentPassphrase_DifferentAddress) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    auto wallet1 = ReferenceWallet::CreateFromMnemonic("wallet1", mnemonic, "", GetTestPath("wallet1"));
    auto wallet2 = ReferenceWallet::CreateFromMnemonic("wallet2", mnemonic, "passphrase", GetTestPath("wallet2"));

    // Different passphrase must produce different address
    EXPECT_NE(wallet1->GetAddress(), wallet2->GetAddress());
}

// =============================================================================
// UTXO Selection Tests - Deterministic Ordering
// =============================================================================

TEST_F(ReferenceWalletTest, UTXO_DeterministicSorting) {
    // Create database
    std::string db_path = test_dir + "/utxo_sorting_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    UTXOManager manager(&db);

    // Add UTXOs in non-sorted order
    UTXO utxo1{"aaaa0000000000000000000000000000000000000000000000000000000000aa", 1, 100000, "script1", 100, false};
    UTXO utxo2{"bbbb0000000000000000000000000000000000000000000000000000000000bb", 0, 200000, "script2", 100, false};
    UTXO utxo3{"aaaa0000000000000000000000000000000000000000000000000000000000aa", 0, 150000, "script3", 100, false};

    manager.AddUTXO(utxo2);
    manager.AddUTXO(utxo1);
    manager.AddUTXO(utxo3);

    // Get sorted UTXOs
    auto sorted = manager.GetUnspentUTXOs(0, 100);

    ASSERT_EQ(sorted.size(), 3);

    // Verify sorting: (txid, vout) lexicographic order
    // Expected: aaaa:0, aaaa:1, bbbb:0
    EXPECT_EQ(sorted[0].txid, "aaaa0000000000000000000000000000000000000000000000000000000000aa");
    EXPECT_EQ(sorted[0].vout, 0);

    EXPECT_EQ(sorted[1].txid, "aaaa0000000000000000000000000000000000000000000000000000000000aa");
    EXPECT_EQ(sorted[1].vout, 1);

    EXPECT_EQ(sorted[2].txid, "bbbb0000000000000000000000000000000000000000000000000000000000bb");
    EXPECT_EQ(sorted[2].vout, 0);
}

TEST_F(ReferenceWalletTest, UTXO_SelectionGreedyLowestFirst) {
    std::string db_path = test_dir + "/utxo_select_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    UTXOManager manager(&db);

    // Add UTXOs: need to select enough to cover 250,000 + 1,000 fee
    UTXO utxo1{"aaaa", 0, 100000, "script1", 100, false};  // Will be selected (lowest txid)
    UTXO utxo2{"bbbb", 0, 100000, "script2", 100, false};  // Will be selected
    UTXO utxo3{"cccc", 0, 100000, "script3", 100, false};  // Will be selected
    UTXO utxo4{"dddd", 0, 100000, "script4", 100, false};  // Won't be selected

    manager.AddUTXO(utxo4);
    manager.AddUTXO(utxo2);
    manager.AddUTXO(utxo3);
    manager.AddUTXO(utxo1);

    // Select UTXOs for 250,000 sats + 1,000 fee = 251,000 total
    auto selected = manager.SelectUTXOs(250000, 1000, 0, 100);

    ASSERT_EQ(selected.size(), 3);
    EXPECT_EQ(selected[0].txid, "aaaa");  // Lowest first
    EXPECT_EQ(selected[1].txid, "bbbb");
    EXPECT_EQ(selected[2].txid, "cccc");
}

TEST_F(ReferenceWalletTest, UTXO_InsufficientFunds) {
    std::string db_path = test_dir + "/utxo_insufficient_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    UTXOManager manager(&db);

    UTXO utxo1{"aaaa", 0, 100000, "script1", 100, false};
    manager.AddUTXO(utxo1);

    // Try to spend more than available
    EXPECT_THROW(
        manager.SelectUTXOs(200000, 1000, 0, 100),
        std::runtime_error
    );
}

// =============================================================================
// Balance Calculation Tests
// =============================================================================

TEST_F(ReferenceWalletTest, Balance_ConfirmedUnconfirmedImmature) {
    std::string db_path = test_dir + "/balance_test_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    UTXOManager manager(&db);

    // Add confirmed UTXO (height 100, current 200, = 101 confirmations)
    UTXO confirmed{"aaaa", 0, 100000, "script1", 100, false};
    manager.AddUTXO(confirmed);

    // Add unconfirmed UTXO (height 0)
    UTXO unconfirmed{"bbbb", 0, 50000, "script2", 0, false};
    manager.AddUTXO(unconfirmed);

    // Add immature coinbase (height 150, current 200, = 51 confirmations < 100)
    UTXO immature{"cccc", 0, 200000, "script3", 150, true};
    manager.AddUTXO(immature);

    auto balance = manager.CalculateBalance(1, 200);

    EXPECT_EQ(balance.confirmed, 100000);
    EXPECT_EQ(balance.unconfirmed, 50000);
    EXPECT_EQ(balance.immature, 200000);
    EXPECT_EQ(balance.total, 350000);
}

TEST_F(ReferenceWalletTest, Balance_CoinbaseMaturity) {
    std::string db_path = test_dir + "/coinbase_maturity_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    UTXOManager manager(&db);

    // Coinbase at height 100, current height 199 = 100 confirmations (not mature yet)
    UTXO immature{"aaaa", 0, 10000000000ULL, "script1", 100, true};
    manager.AddUTXO(immature);

    auto balance1 = manager.CalculateBalance(1, 199);
    EXPECT_EQ(balance1.immature, 10000000000ULL);
    EXPECT_EQ(balance1.confirmed, 0);

    // At height 200 = 101 confirmations (mature!)
    auto balance2 = manager.CalculateBalance(1, 200);
    EXPECT_EQ(balance2.immature, 0);
    EXPECT_EQ(balance2.confirmed, 10000000000ULL);
}

// =============================================================================
// Address Validation Tests
// =============================================================================

TEST_F(ReferenceWalletTest, Address_ValidBech32) {
    std::string valid = crypto::Address::Encode(std::vector<uint8_t>(20, 0xab), "din", 0);
    ASSERT_FALSE(valid.empty());
    EXPECT_TRUE(crypto::Address::Validate(valid, "din"));
}

TEST_F(ReferenceWalletTest, Address_InvalidHRP) {
    // Bitcoin address (wrong HRP)
    std::string btc_addr = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    EXPECT_FALSE(crypto::Address::Validate(btc_addr, "din"));
}

TEST_F(ReferenceWalletTest, Address_InvalidChecksum) {
    // Modified checksum
    std::string invalid = "din1q8wf2mvj6sjtkvtam4ws2kxssnedqr5n9invalid";
    EXPECT_FALSE(crypto::Address::Validate(invalid, "din"));
}

TEST_F(ReferenceWalletTest, Address_EncodeDecodeRoundtrip) {
    // Create a 20-byte hash
    std::vector<uint8_t> hash(20, 0xab);

    // Encode to bech32
    std::string address = crypto::Address::Encode(hash, "din", 0);
    EXPECT_FALSE(address.empty());
    EXPECT_TRUE(address.substr(0, 4) == "din1");

    // Decode back
    auto decoded = crypto::Address::Decode(address);
    EXPECT_EQ(decoded, hash);
}

TEST_F(ReferenceWalletTest, Address_PublicKeyToAddress) {
    // Generate a valid secp256k1 public key (compressed format)
    auto privkey = crypto::ECC::GeneratePrivateKey();
    auto pubkey = crypto::ECC::DerivePublicKey(privkey, true);

    ASSERT_EQ(pubkey.size(), 33);
    EXPECT_TRUE(pubkey[0] == 0x02 || pubkey[0] == 0x03);

    // Convert to address
    auto address = crypto::Address::PublicKeyToAddress(pubkey);
    EXPECT_FALSE(address.empty());
    EXPECT_TRUE(address.substr(0, 4) == "din1");

    // Validate address
    EXPECT_TRUE(crypto::Address::Validate(address, "din"));
}

// =============================================================================
// Database Persistence Tests
// =============================================================================

TEST_F(ReferenceWalletTest, Database_MetadataPersistence) {
    std::string db_path = test_dir + "/metadata_unique.db";
    fs::remove(db_path);  // Ensure clean start

    {
        Database db(db_path);
        db.InitializeSchema();
        db.SetMetadata("key1", "value1");
        db.SetMetadata("key2", "value2");
    }

    // Reopen database
    {
        Database db(db_path);
        EXPECT_EQ(db.GetMetadata("key1"), "value1");
        EXPECT_EQ(db.GetMetadata("key2"), "value2");
        EXPECT_EQ(db.GetMetadata("nonexistent", "default"), "default");
    }
}

TEST_F(ReferenceWalletTest, Database_UTXOPersistence) {
    std::string db_path = test_dir + "/utxo_persist_unique.db";
    fs::remove(db_path);  // Ensure clean start

    {
        Database db(db_path);
        db.InitializeSchema();

        Database::UTXORow utxo;
        utxo.txid = "test_txid";
        utxo.vout = 0;
        utxo.amount = 100000;
        utxo.script_pubkey = "script";
        utxo.height = 100;
        utxo.is_coinbase = false;

        db.InsertUTXO(utxo);
    }

    // Reopen and verify
    {
        Database db(db_path);
        auto utxos = db.GetAllUTXOs();

        ASSERT_EQ(utxos.size(), 1);
        EXPECT_EQ(utxos[0].txid, "test_txid");
        EXPECT_EQ(utxos[0].vout, 0);
        EXPECT_EQ(utxos[0].amount, 100000);
    }
}

TEST_F(ReferenceWalletTest, Database_SpentUTXOTracking) {
    std::string db_path = test_dir + "/spent_utxo_unique.db";
    fs::remove(db_path);  // Ensure clean start

    Database db(db_path);
    db.InitializeSchema();

    // Add UTXO
    Database::UTXORow utxo;
    utxo.txid = "original_tx";
    utxo.vout = 0;
    utxo.amount = 100000;
    utxo.script_pubkey = "script";
    utxo.height = 100;
    utxo.is_coinbase = false;

    db.InsertUTXO(utxo);

    // Mark as spent
    db.MarkUTXOSpent("original_tx", 0, "spending_tx", 105);

    // Verify it's spent
    EXPECT_TRUE(db.IsUTXOSpent("original_tx", 0));

    // Verify it's removed from UTXOs
    auto utxos = db.GetAllUTXOs();
    EXPECT_EQ(utxos.size(), 0);
}

// =============================================================================
// Wallet Integration Tests
// =============================================================================

TEST_F(ReferenceWalletTest, Wallet_CreateAndLoad) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    // Create wallet
    auto wallet1 = ReferenceWallet::CreateFromMnemonic("test", mnemonic, "", GetTestPath("test"));
    std::string address1 = wallet1->GetAddress();
    wallet1.reset();  // Close wallet

    // Load wallet
    auto wallet2 = ReferenceWallet::Load("test", GetTestPath("test"));
    std::string address2 = wallet2->GetAddress();

    // Should have same address
    EXPECT_EQ(address1, address2);
}

TEST_F(ReferenceWalletTest, Wallet_GetWalletInfo) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    auto wallet = ReferenceWallet::CreateFromMnemonic("test", mnemonic, "", GetTestPath("test"));

    auto info = wallet->GetWalletInfo();

    EXPECT_EQ(info.name, "test");
    EXPECT_FALSE(info.address.empty());
    EXPECT_GT(info.creation_time, 0);
    EXPECT_EQ(info.last_block_height, 0);  // No blocks processed yet
}

TEST_F(ReferenceWalletTest, Wallet_EmptyBalance) {
    std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    auto wallet = ReferenceWallet::CreateFromMnemonic("test", mnemonic, "", GetTestPath("test"));

    auto balance = wallet->GetBalance();

    EXPECT_EQ(balance.confirmed, 0);
    EXPECT_EQ(balance.unconfirmed, 0);
    EXPECT_EQ(balance.immature, 0);
    EXPECT_EQ(balance.total, 0);
}

// =============================================================================
// Cryptographic Primitives Tests
// =============================================================================

TEST_F(ReferenceWalletTest, Crypto_ECCSignVerify) {
    auto privkey = crypto::ECC::GeneratePrivateKey();
    auto pubkey = crypto::ECC::DerivePublicKey(privkey, true);

    // Create message hash
    std::vector<uint8_t> message(32, 0xaa);
    auto hash = crypto::Hash::SHA256(message);

    // Sign
    auto signature = crypto::ECC::Sign(privkey, hash);
    EXPECT_EQ(signature.size(), 64);

    // Verify
    EXPECT_TRUE(crypto::ECC::Verify(pubkey, hash, signature));

    // Verify with wrong message should fail
    std::vector<uint8_t> wrong_hash(32, 0xbb);
    EXPECT_FALSE(crypto::ECC::Verify(pubkey, wrong_hash, signature));
}

TEST_F(ReferenceWalletTest, Crypto_Hash160) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    auto hash160 = crypto::Hash::Hash160(data);
    EXPECT_EQ(hash160.size(), 20);

    // Same input should produce same hash
    auto hash160_2 = crypto::Hash::Hash160(data);
    EXPECT_EQ(hash160, hash160_2);
}

TEST_F(ReferenceWalletTest, Crypto_Hash256) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    auto hash256 = crypto::Hash::Hash256(data);
    EXPECT_EQ(hash256.size(), 32);

    // Double SHA256 should not equal single SHA256
    auto sha256 = crypto::Hash::SHA256(data);
    EXPECT_NE(hash256, sha256);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
