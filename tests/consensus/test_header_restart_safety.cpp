/**
 * Phase N.1: Header Restart Safety Test
 *
 * Purpose: Verify headers and best tip survive restart.
 *
 * Exit Criteria:
 * ✅ Headers persist to storage
 * ✅ Best header survives restart
 * ✅ Forks survive restart
 * ✅ Parent pointers rebuilt correctly
 * ✅ Chainwork preserved
 *
 * Requirements:
 * - No bodies stored
 * - No UTXO state
 * - Fast loading
 */

#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <iostream>
#include <filesystem>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using namespace dinero;
using namespace dinero::consensus;

// Test helper: Create a block header
BlockHeader CreateTestHeader(
    const uint256& prev_hash,
    uint32_t time,
    uint32_t bits = 0x1d00ffff
) {
    BlockHeader header;
    header.version = 1;
    header.prev_block_hash = prev_hash;  // Phase M.0: uint256 identity
    header.merkle_root = uint256();  // Null hash
    header.timestamp = time;  // Updated field name
    header.difficulty = bits;  // Updated field name
    header.nonce = 1;
    header.utreexo_root = uint256();  // Null hash
    return header;
}

void AssertSchemaMetadataPresent(const HeaderStore& store) {
    assert(store.HasSchemaMetadata());
    assert(store.IsSchemaCompatible());
    assert(!store.NeedsSchemaRecovery());

    const auto metadata = store.GetPersistedSchemaMetadata();
    assert(metadata.has_value());
    assert(metadata->version > 0);
    assert(!metadata->network.empty());
    assert(metadata->header_size == 128);
}

int main() {
    // Synthetic headers carry fake PoW (arbitrary bits, nonce=1). Header
    // validation now enforces PoW on mainnet/testnet, so run under regtest
    // (PoW skipped, matching block_acceptor) — correct for restart-safety logic.
    dinero::SelectParams(dinero::Chain::REGTEST);

    std::cout << "=== Phase N.1: Header Restart Safety Test ===" << std::endl;

    // Why std::filesystem::temp_directory_path() instead of "/tmp/...": the
    // hardcoded POSIX path falls back to C:\tmp on Windows and std::system
    // ("rm -rf ...") below has no rm.exe in CMD's PATH, so cleanup silently
    // no-ops and leaves stale RocksDB state from prior runs — making the
    // HeaderCount==3 assertion fail when count==7 from a previous run.
    const std::string test_db_path =
        (std::filesystem::temp_directory_path() / "test_header_restart_db").string();

    // Clean up any existing test database
    std::error_code _ec;
    std::filesystem::remove_all(test_db_path, _ec);

    // ========================================================================
    // Test 1: Headers persist and reload
    // ========================================================================
    {
        std::cout << "\n1. Testing header persistence..." << std::endl;

        // Phase 1a: Create headers and persist
        {
            std::cout << "   [TEST] Creating HeaderStore..." << std::endl;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST] Calling store.Open()..." << std::endl;
            bool opened = store.Open();
            std::cout << "   [TEST] store.Open() returned " << opened << std::endl;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST] Creating HeaderChainSelector..." << std::endl;
            HeaderChainSelector selector(&store);
            std::cout << "   [TEST] HeaderChainSelector created" << std::endl;

            // Genesis
            uint256 null_hash;
            null_hash.SetNull();
            BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
            std::cout << "   [TEST] About to call AddHeader for genesis..." << std::endl << std::flush;
            bool added = selector.AddHeader(genesis);
            std::cout << "   [TEST] AddHeader returned: " << added << std::endl << std::flush;
            assert(added);

            // Block 1
            uint256 genesis_hash = genesis.GetHash();
            BlockHeader block1 = CreateTestHeader(genesis_hash, 1000001);
            std::cout << "   [TEST] About to call AddHeader for block1..." << std::endl << std::flush;
            bool added1 = selector.AddHeader(block1);
            std::cout << "   [TEST] AddHeader for block1 returned: " << added1 << std::endl << std::flush;
            assert(added1);

            // Block 2
            uint256 block1_hash = block1.GetHash();
            BlockHeader block2 = CreateTestHeader(block1_hash, 1000002);
            std::cout << "   [TEST] About to call AddHeader for block2..." << std::endl << std::flush;
            bool added2 = selector.AddHeader(block2);
            std::cout << "   [TEST] AddHeader for block2 returned: " << added2 << std::endl << std::flush;
            assert(added2);

            assert(selector.GetBestHeaderValue()->height == 2);
            std::cout << "   Created 3 headers (height 0-2)" << std::endl;

        }

        // Phase 1b: Reload and verify
        {
            std::cout << "   [TEST 1b] Creating HeaderStore..." << std::endl;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST 1b] Calling store.Open()..." << std::endl;
            bool opened = store.Open();
            std::cout << "   [TEST 1b] store.Open() returned " << opened << std::endl;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST 1b] Creating HeaderChainSelector..." << std::endl;
            HeaderChainSelector selector(&store);
            std::cout << "   [TEST 1b] Selector created, header count = " << selector.GetHeaderCount() << std::endl;

            // Verify headers loaded
            assert(selector.GetHeaderCount() == 3);
            assert(selector.GetBestHeaderValue().has_value());
            assert(selector.GetBestHeaderValue()->height == 2);

            std::cout << "   ✅ Headers persisted and reloaded correctly" << std::endl;

        }
    }

    // ========================================================================
    // Test 2: Forks survive restart
    // ========================================================================
    {
        std::cout << "\n2. Testing fork persistence..." << std::endl;

        // Phase 2a: Create fork
        {
            std::cout << "   [TEST 2a] Creating HeaderStore..." << std::endl << std::flush;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST 2a] Calling store.Open()..." << std::endl << std::flush;
            bool opened = store.Open();
            std::cout << "   [TEST 2a] store.Open() returned: " << opened << std::endl << std::flush;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST 2a] Creating HeaderChainSelector..." << std::endl << std::flush;
            HeaderChainSelector selector(&store);
            std::cout << "   [TEST 2a] Selector created" << std::endl << std::flush;

            // Should have previous chain
            assert(selector.GetHeaderCount() == 3);

            // Debug: Check best header
            const auto best = selector.GetBestHeaderValue();
            std::cout << "   Best header at height " << (best ? best->height : -1) << std::endl;

            // Create fork at height 1
            const auto block1_entry = selector.GetHeaderAtHeightValue(1);
            std::cout << "   Got header at height 1: " << (block1_entry ? "yes" : "null") << std::endl;
            assert(block1_entry.has_value());
            uint256 block1_hash = block1_entry->hash;
            BlockHeader fork_block = CreateTestHeader(block1_hash, 1000010);
            std::cout << "   [TEST 2a] About to add fork block..." << std::endl << std::flush;
            bool added_fork = selector.AddHeader(fork_block);
            std::cout << "   [TEST 2a] AddHeader(fork) returned: " << added_fork << std::endl << std::flush;
            assert(added_fork);

            std::cout << "   [TEST 2a] GetHeaderCount() = " << selector.GetHeaderCount() << std::endl << std::flush;
            assert(selector.GetHeaderCount() == 4);  // Original 3 + 1 fork
            std::cout << "   Created fork at height 1" << std::endl;

        }

        // Phase 2b: Reload and verify fork exists
        {
            std::cout << "   [TEST 2b] Creating HeaderStore..." << std::endl << std::flush;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST 2b] Calling store.Open()..." << std::endl << std::flush;
            bool opened = store.Open();
            std::cout << "   [TEST 2b] store.Open() returned: " << opened << std::endl << std::flush;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST 2b] Creating HeaderChainSelector..." << std::endl << std::flush;
            HeaderChainSelector selector(&store);
            std::cout << "   [TEST 2b] Selector created, count=" << selector.GetHeaderCount() << std::endl << std::flush;

            assert(selector.GetHeaderCount() == 4);
            std::cout << "   ✅ Fork persisted and reloaded correctly" << std::endl;

        }
    }

    // ========================================================================
    // Test 3: Best tip switches and persists
    // ========================================================================
    {
        std::cout << "\n3. Testing best tip persistence..." << std::endl;

        uint256 original_best_hash;

        // Phase 3a: Record original best
        {
            std::cout << "   [TEST 3a] Creating HeaderStore..." << std::endl << std::flush;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST 3a] Calling store.Open()..." << std::endl << std::flush;
            bool opened = store.Open();
            std::cout << "   [TEST 3a] store.Open() returned: " << opened << std::endl << std::flush;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST 3a] Creating HeaderChainSelector..." << std::endl << std::flush;
            HeaderChainSelector selector(&store);
            const auto best = selector.GetBestHeaderValue();
            assert(best.has_value());
            original_best_hash = best->hash;
            uint32_t original_height = best->height;

            std::cout << "   Original best at height " << original_height << std::endl;

        }

        // Phase 3b: Restart and verify best tip unchanged
        {
            std::cout << "   [TEST 3b] Creating HeaderStore..." << std::endl << std::flush;
            HeaderStore store(test_db_path);
            std::cout << "   [TEST 3b] Calling store.Open()..." << std::endl << std::flush;
            bool opened = store.Open();
            std::cout << "   [TEST 3b] store.Open() returned: " << opened << std::endl << std::flush;
            assert(opened);
            AssertSchemaMetadataPresent(store);

            std::cout << "   [TEST 3b] Creating HeaderChainSelector..." << std::endl << std::flush;
            HeaderChainSelector selector(&store);

            const auto best = selector.GetBestHeaderValue();
            assert(best.has_value());
            assert(best->hash == original_best_hash);
            std::cout << "   ✅ Best tip persists across restarts" << std::endl;

        }
    }

    // ========================================================================
    // Test 4: Parent pointers rebuilt correctly
    // ========================================================================
    {
        std::cout << "\n4. Testing parent pointer reconstruction..." << std::endl;

        std::cout << "   [TEST 4] Creating HeaderStore..." << std::endl << std::flush;
        HeaderStore store(test_db_path);
        std::cout << "   [TEST 4] Calling store.Open()..." << std::endl << std::flush;
        bool opened = store.Open();
        std::cout << "   [TEST 4] store.Open() returned: " << opened << std::endl << std::flush;
        assert(opened);
        AssertSchemaMetadataPresent(store);

        std::cout << "   [TEST 4] Creating HeaderChainSelector..." << std::endl << std::flush;
        HeaderChainSelector selector(&store);

        // Get tip and walk backwards
        const auto tip = selector.GetBestHeaderValue();
        assert(tip.has_value());

        uint32_t anchor_height = 0;
        std::vector<std::pair<uint256, uint32_t>> ancestry;
        assert(selector.CollectAncestorsByHash(
            tip->hash, 0, anchor_height, ancestry));
        assert(anchor_height == tip->height);
        assert(ancestry.size() == static_cast<size_t>(tip->height) + 1);
        for (size_t i = 0; i < ancestry.size(); ++i) {
            assert(ancestry[i].second == i);
        }

        std::cout << "   ✅ Parent pointers rebuilt correctly" << std::endl;

    }

    // ========================================================================
    // Test 5: Missing best marker recovers by header scan
    // ========================================================================
    {
        std::cout << "\n5. Testing missing best marker recovery..." << std::endl;

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            assert(store.DeleteBestHeader());
        }

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            HeaderChainSelector selector(&store);

            assert(selector.GetHeaderCount() == 4);
            assert(selector.GetBestHeaderValue().has_value());
            assert(selector.GetBestHeaderValue()->height == 2);

            std::cout << "   ✅ Missing best marker recovered via persisted header scan" << std::endl;
        }
    }

    // ========================================================================
    // Test 6: Stale best marker is ignored in favor of chainwork scan
    // ========================================================================
    {
        std::cout << "\n6. Testing stale best marker recovery..." << std::endl;

        uint256 height_one_hash;
        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            HeaderChainSelector selector(&store);
            const auto height_one = selector.GetHeaderAtHeightValue(1);
            assert(height_one.has_value());
            height_one_hash = height_one->hash;
        }

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            assert(store.StoreBestHeader(height_one_hash));
        }

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            HeaderChainSelector selector(&store);

            assert(selector.GetHeaderCount() == 4);
            assert(selector.GetBestHeaderValue().has_value());
            assert(selector.GetBestHeaderValue()->height == 2);

            std::cout << "   ✅ Stale best marker overridden by chainwork scan" << std::endl;
        }
    }

    // ========================================================================
    // Test 7: Best marker pointing at a missing header is tolerated
    // ========================================================================
    {
        std::cout << "\n7. Testing dangling best marker recovery..." << std::endl;

        uint256 bogus_best_hash;
        for (size_t i = 0; i < sizeof(bogus_best_hash.data); ++i) {
            bogus_best_hash.data[i] = static_cast<uint8_t>(i + 1);
        }

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            assert(store.StoreBestHeader(bogus_best_hash));
        }

        {
            HeaderStore store(test_db_path);
            assert(store.Open());
            AssertSchemaMetadataPresent(store);
            HeaderChainSelector selector(&store);

            assert(selector.GetHeaderCount() == 4);
            assert(selector.GetBestHeaderValue().has_value());
            assert(selector.GetBestHeaderValue()->height == 2);

            std::cout << "   ✅ Dangling best marker ignored; best header recovered from stored headers" << std::endl;
        }
    }

    // Clean up test database
    std::cout << "\nCleaning up test database..." << std::endl;
    std::filesystem::remove_all(test_db_path, _ec);

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    std::cout << "\nPhase N.1 Restart Safety Verification:" << std::endl;
    std::cout << "  ✅ Headers persist to storage" << std::endl;
    std::cout << "  ✅ Best header survives restart" << std::endl;
    std::cout << "  ✅ Forks survive restart" << std::endl;
    std::cout << "  ✅ Parent pointers rebuilt correctly" << std::endl;
    std::cout << "  ✅ Missing/stale best markers recover deterministically" << std::endl;
    std::cout << "\nHeader-first sync is now restart-safe." << std::endl;

    return 0;
}
