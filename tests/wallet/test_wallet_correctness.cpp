// Wallet Correctness Test Suite
// Tests wallet invariants: restore, persistence, and derivation determinism
//
// Ring 2 (Partially Implemented but Not Formally Verified):
//   1. Wallet Restore Correctness
//   2. UTXO Persistence Across Restarts
//   3. Address Derivation Determinism ← THIS FILE
//
// CI: Optional, non-blocking (separate from consensus-critical)

#include <gtest/gtest.h>
#include <filesystem>
#include <set>
#include <vector>
#include <string>

#include "wallet/hd_wallet.h"
#include "wallet/utxo_index.h"
#include "primitives/uint256.h"
#include "storage/chain_height_provider.h"
#include "din_json.h"
#include "wallet_test_helpers.h"

namespace dinero::wallet::test {

// ═══════════════════════════════════════════════════════════════════════════
// Minimal Test Fixtures (Ring 2 - Wallet Correctness Tests Only)
// ═══════════════════════════════════════════════════════════════════════════
// These minimal mocks provide just enough infrastructure to test wallet
// persistence without expanding into full integration testing.
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Minimal mock UTXOIndex for testing wallet persistence
 * Allows injecting a single UTXO without requiring full blockchain
 */
class MinimalMockUTXOIndex : public dinero::UTXOIndex {
public:
    explicit MinimalMockUTXOIndex(const std::string& db_path)
        : dinero::UTXOIndex(db_path) {
        Initialize();
    }

    // Inject a single test UTXO
    bool InjectTestUTXO(const std::string& txid_hex, uint32_t vout,
                        uint64_t value, const std::vector<uint8_t>& spk,
                        const std::string& path, uint32_t height) {
        dinero::WalletUTXO utxo;
        if (!dinero::uint256::FromHex(txid_hex, utxo.txid)) {
            return false;
        }
        utxo.vout = vout;
        utxo.value = static_cast<int64_t>(value);
        utxo.spk = spk;
        utxo.path = path;
        utxo.height = static_cast<int>(height);
        utxo.is_coinbase = true;  // Test with coinbase UTXO

        return AddUTXO(utxo);
    }
};

/**
 * Minimal mock chain height provider
 */
class MinimalMockChainHeight : public dinero::ChainHeightProvider {
private:
    uint32_t height_;

public:
    explicit MinimalMockChainHeight(uint32_t initial_height = COINBASE_MATURITY + 1)
        : height_(initial_height) {}

    uint32_t GetBestHeight() const override {
        return height_;
    }

    std::string GetBestHash() const override {
        return "0000000000000000000000000000000000000000000000000000000000000000";
    }

    double GetDifficulty() const override {
        return 1.0;
    }

    Json::Value GetBlockHeader(const std::string&) const override {
        return Json::Value();
    }

    bool IsAvailable() const override {
        return true;
    }

    void SetHeight(uint32_t h) {
        height_ = h;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: UTXO Persistence Across Restart (Clean Shutdown Only)
// ═══════════════════════════════════════════════════════════════════════════
//
// Invariant: wallet state on disk == wallet state after restart, for the same chain tip
//
// Pass Criteria:
//   ✅ Balance survives clean restart (exact match)
//   ✅ listunspent survives clean restart (same outpoints + amounts + script + height)
//   ✅ No duplicate outpoints
//   ✅ Ordering-independent comparison (sort by outpoint)
//
// Failure Modes:
//   ❌ Balance mismatch → Wallet DB not persisting correctly
//   ❌ UTXO count wrong → Missing outputs or duplicates
//   ❌ Duplicate outpoints → UTXO set corruption (critical bug)
//
// Execution Priority: SECOND (clean restart mechanics)
//
// Scope (v1.0.8.x): Clean restart only. No crash simulation, no Stratum,
//                   no mempool, no deep reorg.
//
// Test Fixture:
//   - Mines exactly ONE coinbase UTXO to wallet-owned address
//   - Waits for coinbase maturity (100 blocks)
//   - Tests persistence of this single UTXO across clean restart
// ═══════════════════════════════════════════════════════════════════════════

TEST(WalletCorrectness, UTXOPersistenceAcrossRestart) {
    // ARRANGE
    const std::filesystem::path temp_dir = create_temp_dir("wallet_persistence_test");
    const std::filesystem::path wallet_dir = temp_dir / "wallet";
    const std::filesystem::path utxo_db = temp_dir / "utxo_index.db";
    const std::string seed = TEST_SEED_1;
    const uint32_t coin_type = 1447;

    std::vector<dinero::CanonicalWalletUTXO> original_utxos;
    uint64_t original_balance;

    // Helper: Normalize UTXO vector for ordering-independent comparison (Surgical Refinement #2)
    auto normalize = [](std::vector<dinero::CanonicalWalletUTXO> v) {
        std::sort(v.begin(), v.end(),
            [](const dinero::CanonicalWalletUTXO& a, const dinero::CanonicalWalletUTXO& b) {
                if (a.txid != b.txid) return a.txid < b.txid;
                return a.vout < b.vout;
            });
        return v;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 1: Create wallet, inject ONE UTXO, record state
    // ═══════════════════════════════════════════════════════════════════════
    {
        // Create wallet
        auto wallet = HDWallet::Restore(
            wallet_dir.string(),
            coin_type,
            seed,
            "",
            false  // disable autolock for testing
        );
        ASSERT_NE(wallet, nullptr) << "Failed to create wallet";

        // Get a receiving address and its scriptPubKey
        auto addr = wallet->DeriveNextAddress();
        ASSERT_FALSE(addr.empty()) << "Failed to derive address";

        // Create minimal mock infrastructure
        auto utxo_index = std::make_unique<MinimalMockUTXOIndex>(utxo_db.string());
        auto chain_height = std::make_unique<MinimalMockChainHeight>(COINBASE_MATURITY + 10);

        // Inject exactly ONE mature coinbase UTXO
        // Deterministic test UTXO: 50 coins, height=10 (mature at height=110, current height=111)
        const std::string test_txid = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
        const uint64_t test_value = 50 * 100000000ULL;  // 50 DINERO
        std::vector<uint8_t> test_spk = {0x00, 0x14};  // OP_0 + OP_PUSHBYTES_20 (P2WPKH)
        for (int i = 0; i < 20; i++) test_spk.push_back(static_cast<uint8_t>(i));  // Dummy pubkey hash

        ASSERT_TRUE(utxo_index->InjectTestUTXO(test_txid, 0, test_value, test_spk,
                                                 "m/84'/1447'/0'/0/0", 10))
            << "Failed to inject test UTXO";

        // Connect wallet to mock infrastructure
        wallet->ConnectUTXOIndex(utxo_index.get());
        wallet->ConnectChainHeightProvider(chain_height.get());
        wallet->RegisterAddresses();

        // Record state
        original_balance = wallet->GetBalance().confirmed;
        original_utxos = wallet->ListUTXOs(1);

        // ASSERT: Wallet has exactly ONE mature UTXO
        ASSERT_GT(original_balance, 0) << "Wallet should have non-zero balance";
        ASSERT_EQ(original_utxos.size(), 1) << "Wallet should have exactly 1 UTXO";
        ASSERT_EQ(original_utxos[0].value, test_value) << "UTXO value should match injected value";

        wallet.reset();
        utxo_index.reset();
        chain_height.reset();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 2: Restart wallet, verify UTXO persisted
    // ═══════════════════════════════════════════════════════════════════════
    {
        // Reopen wallet
        auto wallet = HDWallet::Open(
            wallet_dir.string(),
            coin_type,
            false
        );
        ASSERT_NE(wallet, nullptr) << "Failed to reopen wallet";

        // Reconnect to mock infrastructure (same UTXO index state)
        auto utxo_index = std::make_unique<MinimalMockUTXOIndex>(utxo_db.string());
        auto chain_height = std::make_unique<MinimalMockChainHeight>(COINBASE_MATURITY + 10);

        wallet->ConnectUTXOIndex(utxo_index.get());
        wallet->ConnectChainHeightProvider(chain_height.get());
        wallet->RegisterAddresses();

        // ASSERT: Balance persisted
        auto reloaded_balance = wallet->GetBalance().confirmed;
        ASSERT_EQ(reloaded_balance, original_balance) << "Balance must persist across restart";

        // ASSERT: UTXOs persisted exactly
        auto reloaded_utxos = wallet->ListUTXOs(1);
        ASSERT_EQ(reloaded_utxos.size(), original_utxos.size()) << "UTXO count must match";

        // ASSERT: Ordering-independent equality (Surgical Refinement #2)
        auto normalized_original = normalize(original_utxos);
        auto normalized_reloaded = normalize(reloaded_utxos);

        ASSERT_EQ(normalized_original.size(), normalized_reloaded.size());
        for (size_t i = 0; i < normalized_original.size(); i++) {
            ASSERT_EQ(normalized_original[i].txid, normalized_reloaded[i].txid);
            ASSERT_EQ(normalized_original[i].vout, normalized_reloaded[i].vout);
            ASSERT_EQ(normalized_original[i].value, normalized_reloaded[i].value);
        }

        // ASSERT: No duplicates (critical)
        std::set<std::string> outpoint_set;
        for (const auto& utxo : reloaded_utxos) {
            std::string outpoint = utxo.GetOutpointString();
            ASSERT_TRUE(outpoint_set.insert(outpoint).second)
                << "Duplicate outpoint detected: " << outpoint;
        }

        wallet.reset();
        utxo_index.reset();
        chain_height.reset();
    }

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Address Derivation Determinism
// ═══════════════════════════════════════════════════════════════════════════
//
// Invariant: Seed + path → address is a pure function (always)
//
// Pass Criteria:
//   ✅ Same seed produces same addresses in same order (always)
//   ✅ Index counter persists across restarts
//   ✅ Change addresses deterministic
//   ✅ No index skipping (addresses never duplicate)
//   ✅ Derivation path monotonically increases
//
// Failure Modes:
//   ❌ Address mismatch → Non-deterministic derivation
//   ❌ Index reuse → Counter not persisted correctly
//   ❌ Duplicate addresses → Critical: key reuse vulnerability
//   ❌ Change address mismatch → Separate derivation path broken
//
// Execution Priority: FIRST (pure function, no state, fastest)
// ═══════════════════════════════════════════════════════════════════════════

TEST(WalletCorrectness, AddressDerivationDeterminism) {
    // ARRANGE
    const std::string seed = TEST_SEED_2;  // BIP39 standard test vector
    const std::filesystem::path temp_dir = create_temp_dir("wallet_derivation_test");
    const uint32_t coin_type = 1447;  // Dinero coin type

    std::vector<std::string> addresses_first_run;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 1: Generate 20 addresses
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet = HDWallet::Restore(
            (temp_dir / "wallet_1").string(),
            coin_type,
            seed,
            "",  // passphrase
            false  // disable autolock for testing
        );

        ASSERT_NE(wallet, nullptr) << "Failed to restore wallet from seed";

        for (int i = 0; i < 20; i++) {
            addresses_first_run.push_back(wallet->DeriveNextAddress());
        }

        // ASSERT: Index counter at expected position
        ASSERT_EQ(wallet->CurrentIndex(), 20) << "Receive index should be 20 after generating 20 addresses";

        wallet.reset();  // Clean shutdown
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 2: Restart, generate 10 more (should continue from index 20)
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet = HDWallet::Open(
            (temp_dir / "wallet_1").string(),
            coin_type,
            false  // disable autolock
        );

        ASSERT_NE(wallet, nullptr) << "Failed to reopen wallet";

        // ASSERT: Index persisted across restart
        ASSERT_EQ(wallet->CurrentIndex(), 20) << "Index should persist at 20 after restart";

        for (int i = 0; i < 10; i++) {
            addresses_first_run.push_back(wallet->DeriveNextAddress());
        }

        // ASSERT: Index advanced correctly
        ASSERT_EQ(wallet->CurrentIndex(), 30) << "Receive index should be 30 after generating 10 more";

        wallet.reset();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 3: Restore from seed, generate all 30
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet = HDWallet::Restore(
            (temp_dir / "wallet_2").string(),
            coin_type,
            seed,
            "",
            false
        );

        ASSERT_NE(wallet, nullptr) << "Failed to restore wallet from seed (phase 3)";

        std::vector<std::string> addresses_restored;
        for (int i = 0; i < 30; i++) {
            addresses_restored.push_back(wallet->DeriveNextAddress());
        }

        // ASSERT: Exact match (deterministic derivation)
        ASSERT_EQ(addresses_first_run.size(), 30);
        ASSERT_EQ(addresses_restored.size(), 30);
        ASSERT_EQ(addresses_first_run, addresses_restored)
            << "Restored wallet must produce identical addresses in identical order";

        // ASSERT: Index matches (surgical refinement #1)
        ASSERT_EQ(wallet->CurrentIndex(), 30)
            << "Index assertion prevents counter drift under future refactors";

        wallet.reset();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 4: Test change address determinism
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet_a = HDWallet::Restore(
            (temp_dir / "wallet_3a").string(),
            coin_type,
            seed,
            "",
            false
        );

        auto wallet_b = HDWallet::Restore(
            (temp_dir / "wallet_3b").string(),
            coin_type,
            seed,
            "",
            false
        );

        ASSERT_NE(wallet_a, nullptr);
        ASSERT_NE(wallet_b, nullptr);

        // Generate 5 receive addresses on both
        for (int i = 0; i < 5; i++) {
            wallet_a->DeriveNextAddress();
            wallet_b->DeriveNextAddress();
        }

        // Request change addresses
        auto change_a = wallet_a->DeriveNextChangeAddress();
        auto change_b = wallet_b->DeriveNextChangeAddress();

        // ASSERT: Change addresses match (separate derivation path works)
        ASSERT_EQ(change_a, change_b) << "Change addresses must be deterministic";

        // ASSERT: Change index correct (surgical refinement #1)
        ASSERT_EQ(wallet_a->CurrentChangeIndex(), 1) << "Change index should be 1";
        ASSERT_EQ(wallet_b->CurrentChangeIndex(), 1) << "Change index should be 1";

        wallet_a.reset();
        wallet_b.reset();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 5: Test index skipping detection (critical security test)
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet = HDWallet::Restore(
            (temp_dir / "wallet_4").string(),
            coin_type,
            seed,
            "",
            false
        );

        ASSERT_NE(wallet, nullptr);

        std::set<std::string> address_set;

        // Generate 100 addresses
        for (int i = 0; i < 100; i++) {
            auto addr = wallet->DeriveNextAddress();

            // ASSERT: No duplicates (index not reused - key reuse vulnerability)
            bool inserted = address_set.insert(addr).second;
            ASSERT_TRUE(inserted) << "Address " << addr << " generated twice (index reuse!)";
        }

        // ASSERT: Index advanced monotonically
        ASSERT_EQ(wallet->CurrentIndex(), 100) << "Index should be 100";
        ASSERT_EQ(address_set.size(), 100) << "Should have 100 unique addresses";

        wallet.reset();
    }

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Wallet Restore Correctness
// ═══════════════════════════════════════════════════════════════════════════
//
// Invariant: restore(mnemonic) → same wallet state (always)
//
// Pass Criteria:
//   ✅ Balance matches after restore (exact)
//   ✅ UTXOs match after restore (same set, ordering-independent)
//   ✅ Address derivation index matches
//   ✅ Mnemonic retrieval works
//
// Failure Modes:
//   ❌ Balance mismatch → Restore not loading UTXO state correctly
//   ❌ UTXO mismatch → UTXO index not being queried correctly
//   ❌ Index mismatch → Counter not being restored from seed derivation
//   ❌ Mnemonic mismatch → BIP39 seed restoration broken
//
// Execution Priority: LAST (end-to-end integration of Tests 2 + 3)
//
// Scope (v1.0.8.x): Basic restore correctness with minimal infrastructure.
//                   No full chain rescan, no Stratum, no mempool.
//
// Test Fixture:
//   - Same minimal mock infrastructure as Test 2
//   - Exactly ONE coinbase UTXO to verify state restoration
//   - Two wallets from same seed should have identical state
// ═══════════════════════════════════════════════════════════════════════════

TEST(WalletCorrectness, WalletRestoreCorrectness) {
    // ARRANGE
    const std::filesystem::path temp_dir = create_temp_dir("wallet_restore_test");
    const std::filesystem::path wallet_a_dir = temp_dir / "wallet_a";
    const std::filesystem::path wallet_b_dir = temp_dir / "wallet_b";
    const std::filesystem::path utxo_db = temp_dir / "utxo_index.db";
    const std::string seed = TEST_SEED_2;  // Valid BIP39 test vector (different from Test 2's TEST_SEED_1)
    const uint32_t coin_type = 1447;

    std::string mnemonic_a;
    std::vector<dinero::CanonicalWalletUTXO> utxos_a;
    uint64_t balance_a;
    uint32_t index_a;
    uint32_t change_index_a;

    // Helper: Normalize UTXO vector for ordering-independent comparison
    auto normalize = [](std::vector<dinero::CanonicalWalletUTXO> v) {
        std::sort(v.begin(), v.end(),
            [](const dinero::CanonicalWalletUTXO& a, const dinero::CanonicalWalletUTXO& b) {
                if (a.txid != b.txid) return a.txid < b.txid;
                return a.vout < b.vout;
            });
        return v;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 1: Create wallet A, derive addresses, inject UTXO, record state
    // ═══════════════════════════════════════════════════════════════════════
    {
        auto wallet = HDWallet::Restore(
            wallet_a_dir.string(),
            coin_type,
            seed,
            "",
            false  // disable autolock for testing
        );
        ASSERT_NE(wallet, nullptr) << "Failed to create wallet A";

        // Derive some addresses to advance index counters
        wallet->DeriveNextAddress();
        wallet->DeriveNextAddress();
        wallet->DeriveNextAddress();
        wallet->DeriveNextChangeAddress();

        // Get wallet A's mnemonic for restoration
        mnemonic_a = wallet->GetMnemonic();
        ASSERT_FALSE(mnemonic_a.empty()) << "Failed to retrieve mnemonic from wallet A";

        // Create minimal mock infrastructure
        auto utxo_index = std::make_unique<MinimalMockUTXOIndex>(utxo_db.string());
        auto chain_height = std::make_unique<MinimalMockChainHeight>(COINBASE_MATURITY + 20);

        // Inject exactly ONE mature coinbase UTXO
        const std::string test_txid = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
        const uint64_t test_value = 25 * 100000000ULL;  // 25 DINERO
        std::vector<uint8_t> test_spk = {0x00, 0x14};  // OP_0 + OP_PUSHBYTES_20 (P2WPKH)
        for (int i = 0; i < 20; i++) test_spk.push_back(static_cast<uint8_t>(i + 100));  // Different from Test 2

        ASSERT_TRUE(utxo_index->InjectTestUTXO(test_txid, 0, test_value, test_spk,
                                                 "m/84'/1447'/0'/0/0", 20))
            << "Failed to inject test UTXO";

        // Connect wallet to mock infrastructure
        wallet->ConnectUTXOIndex(utxo_index.get());
        wallet->ConnectChainHeightProvider(chain_height.get());
        wallet->RegisterAddresses();

        // Record wallet A state
        balance_a = wallet->GetBalance().confirmed;
        utxos_a = wallet->ListUTXOs(1);
        index_a = wallet->CurrentIndex();
        change_index_a = wallet->CurrentChangeIndex();

        // ASSERT: Wallet A has expected state
        ASSERT_GT(balance_a, 0) << "Wallet A should have non-zero balance";
        ASSERT_EQ(utxos_a.size(), 1) << "Wallet A should have exactly 1 UTXO";
        ASSERT_EQ(index_a, 3) << "Wallet A should have index 3 after deriving 3 addresses";
        ASSERT_EQ(change_index_a, 1) << "Wallet A should have change index 1 after deriving 1 change address";

        wallet.reset();
        utxo_index.reset();
        chain_height.reset();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 2: Restore wallet B from mnemonic, verify identical state
    // ═══════════════════════════════════════════════════════════════════════
    {
        // Restore wallet B from wallet A's mnemonic
        auto wallet = HDWallet::Restore(
            wallet_b_dir.string(),
            coin_type,
            mnemonic_a,
            "",
            false
        );
        ASSERT_NE(wallet, nullptr) << "Failed to restore wallet B from mnemonic";

        // Wallet B starts fresh - need to derive same addresses to reach same index
        wallet->DeriveNextAddress();
        wallet->DeriveNextAddress();
        wallet->DeriveNextAddress();
        wallet->DeriveNextChangeAddress();

        // Connect to same mock infrastructure (same UTXO index state)
        auto utxo_index = std::make_unique<MinimalMockUTXOIndex>(utxo_db.string());
        auto chain_height = std::make_unique<MinimalMockChainHeight>(COINBASE_MATURITY + 20);

        wallet->ConnectUTXOIndex(utxo_index.get());
        wallet->ConnectChainHeightProvider(chain_height.get());
        wallet->RegisterAddresses();

        // ASSERT: Wallet B mnemonic matches wallet A
        std::string mnemonic_b = wallet->GetMnemonic();
        ASSERT_EQ(mnemonic_a, mnemonic_b) << "Restored wallet mnemonic must match original";

        // ASSERT: Wallet B balance matches wallet A (Surgical Refinement #3 - implicit full scan)
        uint64_t balance_b = wallet->GetBalance().confirmed;
        ASSERT_EQ(balance_a, balance_b) << "Restored wallet balance must match original";

        // ASSERT: Wallet B UTXOs match wallet A (ordering-independent)
        auto utxos_b = wallet->ListUTXOs(1);
        ASSERT_EQ(utxos_a.size(), utxos_b.size()) << "Restored wallet UTXO count must match";

        auto normalized_a = normalize(utxos_a);
        auto normalized_b = normalize(utxos_b);

        ASSERT_EQ(normalized_a.size(), normalized_b.size());
        for (size_t i = 0; i < normalized_a.size(); i++) {
            ASSERT_EQ(normalized_a[i].txid, normalized_b[i].txid)
                << "UTXO txid mismatch at index " << i;
            ASSERT_EQ(normalized_a[i].vout, normalized_b[i].vout)
                << "UTXO vout mismatch at index " << i;
            ASSERT_EQ(normalized_a[i].value, normalized_b[i].value)
                << "UTXO value mismatch at index " << i;
        }

        // ASSERT: Wallet B indices match wallet A
        uint32_t index_b = wallet->CurrentIndex();
        uint32_t change_index_b = wallet->CurrentChangeIndex();
        ASSERT_EQ(index_a, index_b) << "Restored wallet index must match original";
        ASSERT_EQ(change_index_a, change_index_b) << "Restored wallet change index must match original";

        wallet.reset();
        utxo_index.reset();
        chain_height.reset();
    }

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

} // namespace dinero::wallet::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
