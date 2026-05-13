// SPDX-License-Identifier: MIT
// Phase H.5 — Restart Recovery Test (Crash Safety Proof)

#include <gtest/gtest.h>
#include "consensus/header_sync_manager.h"
#include "consensus/chain_manager_interface.h"
#include "consensus/block_index.h"
#include "consensus/pow.h"
#include "consensus/chainparams.h"  // For SelectParams(Chain::REGTEST)
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "primitives/block.h"
#include "common/logger.h"
#include <filesystem>
#include <memory>
#include <iostream>

namespace dinero {
namespace test {

/**
 * Mock ChainManager for testing
 *
 * This mock implements IChainManager to provide test control over
 * active tip state without running full consensus validation.
 * The test focuses on header sync restart correctness, not consensus logic.
 */
class MockChainManager : public IChainManager {
public:
    MockChainManager() {
        // Create genesis block index
        genesis_index_ = std::make_unique<CBlockIndex>();
        genesis_index_->hash = uint256::FromHexUnsafe(std::string(64, '0'));
        genesis_index_->prev_hash.SetNull();  // Null hash for genesis
        genesis_index_->height = 0;
        genesis_index_->chainwork = "0000000000000000000000000000000000000000000000000000000000000000";
        genesis_index_->status = BLOCK_VALID_CHAIN;

        active_tip_ = genesis_index_.get();
        block_index_[genesis_index_->hash] = genesis_index_.get();
    }

    // IChainManager interface implementation
    CBlockIndex* GetTip() const override {
        return active_tip_;
    }

    uint32_t GetHeight() const override {
        return active_tip_ ? active_tip_->height : 0;
    }

    // Test helper: Get best block hash as hex string (non-interface method)
    std::string GetBestBlockHash() const {
        return active_tip_ ? active_tip_->hash.GetHex() : "";
    }

    // Test helper: Simulate block connection (update active tip)
    void SetActiveTip(const std::string& block_hash_hex) {
        uint256 block_hash = uint256::FromHexUnsafe(block_hash_hex);
        auto it = block_index_.find(block_hash);
        if (it != block_index_.end()) {
            active_tip_ = it->second;
        }
    }

    // Test helper: Add block to index
    void AddBlockIndex(const std::string& hash_hex, uint32_t height, const std::string& chainwork) {
        auto index = std::make_unique<CBlockIndex>();
        index->hash = uint256::FromHexUnsafe(hash_hex);
        index->height = height;
        index->chainwork = chainwork;
        index->status = BLOCK_VALID_CHAIN;

        std::cout << "[MOCK] Adding block: hash=" << hash_hex.substr(0,16) << " height=" << height
                  << " GetBlockHash()=" << index->GetBlockHash().GetHex().substr(0,16) << std::endl;

        block_index_[index->hash] = index.get();
        owned_indices_.push_back(std::move(index));
    }

    // Test helper: Get all block indices (for state preservation)
    const std::unordered_map<uint256, CBlockIndex*>& GetAllIndices() const {
        return block_index_;
    }

private:
    CBlockIndex* active_tip_{nullptr};
    std::unique_ptr<CBlockIndex> genesis_index_;
    std::vector<std::unique_ptr<CBlockIndex>> owned_indices_;
    std::unordered_map<uint256, CBlockIndex*> block_index_;
};

/**
 * Phase H.5 — Restart Recovery Test
 *
 * This test proves that HeaderSyncManager persistence (H.3) and
 * IBD detection (H.4) are crash-correct.
 *
 * Proof Strategy:
 * 1. Feed headers A → B → C → D
 * 2. Download blocks A, B (partial sync)
 * 3. Verify IsInitialBlockDownload() == true (diverged)
 * 4. Simulate crash (hard stop)
 * 5. Restart node
 * 6. Verify all state restored from disk
 * 7. Verify IsInitialBlockDownload() == true (still diverged)
 * 8. Download blocks C, D (complete sync)
 * 9. Verify IsInitialBlockDownload() == false (converged)
 *
 * If this test passes, IBD is crash-correct and restart-safe.
 */
class HeaderSyncRestartTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize regtest network parameters
        // This ensures HeaderSyncManager uses regtest PoW rules (require_standard = false)
        SelectParams(Chain::REGTEST);

        test_dir_ = std::filesystem::temp_directory_path() / "dinero_h5_restart_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        InitializeNode();
    }

    void TearDown() override {
        header_sync_.reset();
        mock_chainman_.reset();
        chain_db_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    // Initialize node (used for both initial setup and restart)
    void InitializeNode() {
        // Initialize ChainDB
        chain_db_ = std::make_unique<ChainDB>();
        ASSERT_EQ(chain_db_->init(test_dir_ / "chainstate"), Status::Ok);

        // Initialize MockChainManager
        mock_chainman_ = std::make_unique<MockChainManager>();

        // Initialize HeaderSyncManager (no cast needed - MockChainManager implements IChainManager)
        header_sync_ = std::make_unique<HeaderSyncManager>();
        ASSERT_TRUE(header_sync_->Initialize(
            chain_db_.get(),
            mock_chainman_.get(),
            test_dir_));
    }

    // Simulate crash: destroy node, close ChainDB (NO graceful shutdown)
    void SimulateCrash() {
        // Save chain state before crash (simulates what ChainDB would persist)
        saved_block_indices_.clear();
        std::cout << "[TEST] Saving chain state before crash..." << std::endl;
        for (const auto& [hash, index] : mock_chainman_->GetAllIndices()) {
            std::cout << "[TEST] Saving block at height " << index->height << std::endl;
            saved_block_indices_.push_back({hash.GetHex(), index->height, index->chainwork});
        }
        saved_active_tip_ = mock_chainman_->GetBestBlockHash();
        std::cout << "[TEST] Saved active tip: " << saved_active_tip_.substr(0, 16) << "..." << std::endl;
        std::cout << "[TEST] Saved " << saved_block_indices_.size() << " blocks total" << std::endl;

        // Destroy HeaderSyncManager (no Shutdown() call - simulates SIGKILL)
        header_sync_.reset();

        // Close ChainDB (no flush)
        chain_db_.reset();

        // Destroy ChainManager
        mock_chainman_.reset();
    }

    // Restart node with same datadir
    void RestartNode() {
        InitializeNode();

        // Restore chain state (simulates ChainDB loading persisted blocks)
        std::cout << "[TEST] Restoring " << saved_block_indices_.size() << " block indices..." << std::endl;
        for (const auto& [hash_hex, height, chainwork] : saved_block_indices_) {
            std::cout << "[TEST] Restoring block at height " << height << std::endl;
            mock_chainman_->AddBlockIndex(hash_hex, height, chainwork);
        }
        if (!saved_active_tip_.empty()) {
            std::cout << "[TEST] Restoring active tip: " << saved_active_tip_.substr(0, 16) << "..." << std::endl;
            mock_chainman_->SetActiveTip(saved_active_tip_);
        }
        std::cout << "[TEST] After restore: chain height = " << mock_chainman_->GetHeight() << std::endl;
    }

    // Helper: Mine a header to satisfy PoW
    // Finds a nonce such that CheckProofOfWork returns true
    void MineHeader(BlockHeader& header) {
        // Use actual consensus PoW validation (not a simplified approximation)
        for (uint64_t nonce = 0; nonce < 100'000'000; nonce++) {
            header.nonce = static_cast<uint32_t>(nonce);

            // Use the real consensus CheckProofOfWork function
            if (consensus::CheckProofOfWork(header, false)) {
                std::cout << "[MINE] Found valid nonce: " << nonce
                          << " hash: " << header.GetHash().GetHex().substr(0, 16) << std::endl;
                return;  // Found valid nonce
            }
        }
        // Debug: print why we failed
        std::cerr << "[MINE] FAILED after 100M attempts" << std::endl;
        std::cerr << "[MINE] difficulty bits: 0x" << std::hex << header.difficulty << std::dec << std::endl;
        std::cerr << "[MINE] last hash: " << header.GetHash().GetHex() << std::endl;
        throw std::runtime_error("Failed to mine header after 100M attempts");
    }

    // Helper: Create and mine a valid header
    BlockHeader CreateHeader(uint32_t height, const std::string& prev_hash) {
        BlockHeader header;
        header.version = 1;
        header.prev_block_hash = uint256::FromHexUnsafe(prev_hash);
        header.merkle_root = uint256();  // Zero merkle root
        header.timestamp = 1000000 + height;
        header.difficulty = 0x207fffff;  // Regtest difficulty (instant mining)
        header.nonce = 0;  // Will be set by MineHeader
        header.utreexo_root = uint256();  // Zero utreexo
        std::memset(header.reserved, 0, 12);  // Zero reserved field

        // Mine to find valid nonce satisfying PoW
        MineHeader(header);

        return header;
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<ChainDB> chain_db_;
    std::unique_ptr<MockChainManager> mock_chainman_;
    std::unique_ptr<HeaderSyncManager> header_sync_;

    // State preservation across crash/restart
    std::vector<std::tuple<std::string, uint32_t, std::string>> saved_block_indices_;
    std::string saved_active_tip_;
};

/**
 * Phase H.5 — Core Restart Test
 *
 * This test validates the complete crash recovery flow:
 * - Header persistence survives crash
 * - Block status flags survive crash
 * - IBD state is correctly recomputed on restart
 * - Download scheduling reconstructs correctly
 */
TEST_F(HeaderSyncRestartTest, test_phase_h5_restart_ibd) {
    //==========================================================================
    // 0️⃣ INITIAL STATE ASSERTIONS
    //==========================================================================

    // Initial state: genesis only
    EXPECT_EQ(mock_chainman_->GetHeight(), 0);
    EXPECT_EQ(mock_chainman_->GetBestBlockHash(), std::string(64, '0'));

    // IBD state with genesis only (may be true or false depending on implementation)
    // We'll verify it changes correctly during the test

    //==========================================================================
    // 1️⃣ HEADER INJECTION PHASE (Headers Only: A → B → C → D)
    //==========================================================================

    std::string genesis_hash = std::string(64, '0');

    // Create headers with proper hash computation
    BlockHeader header_a = CreateHeader(1, genesis_hash);
    BlockHeader header_b = CreateHeader(2, header_a.GetHash().GetHex());
    BlockHeader header_c = CreateHeader(3, header_b.GetHash().GetHex());
    BlockHeader header_d = CreateHeader(4, header_c.GetHash().GetHex());

    // Extract hash strings for later use
    std::string hash_a = header_a.GetHash().GetHex();
    std::string hash_b = header_b.GetHash().GetHex();
    std::string hash_c = header_c.GetHash().GetHex();
    std::string hash_d = header_d.GetHash().GetHex();

    // Inject headers
    std::vector<BlockHeader> headers = {header_a, header_b, header_c, header_d};
    std::cout << "[TEST] About to call ProcessHeaders..." << std::endl;
    ASSERT_TRUE(header_sync_->ProcessHeaders("peer1", headers));
    std::cout << "[TEST] ProcessHeaders returned successfully" << std::endl;

    // Assertions: Headers accepted
    // Heights are 0,1,2,3 (since genesis isn't in header index, first header gets height 0)
    std::cout << "[TEST] About to call GetBestHeaderHeight..." << std::endl;
    EXPECT_EQ(header_sync_->GetBestHeaderHeight(), 3);
    std::cout << "[TEST] GetBestHeaderHeight returned" << std::endl;

    std::cout << "[TEST] About to call GetBestHeaderHash..." << std::endl;
    std::string best_hash = header_sync_->GetBestHeaderHash();
    std::cout << "[TEST] GetBestHeaderHash returned: " << best_hash.substr(0, 16) << "..." << std::endl;
    std::cout << "[TEST] About to compare hash..." << std::endl;
    EXPECT_EQ(best_hash, header_d.GetHash().GetHex());
    std::cout << "[TEST] Hash comparison done" << std::endl;

    // Active chain still at genesis (no blocks downloaded yet)
    std::cout << "[TEST] About to call mock_chainman_->GetHeight()..." << std::endl;
    EXPECT_EQ(mock_chainman_->GetHeight(), 0);
    std::cout << "[TEST] GetHeight() done" << std::endl;

    // IBD should be true (header tip ahead of active tip)
    std::cout << "[TEST] About to call IsInitialBlockDownload()..." << std::endl;
    EXPECT_TRUE(header_sync_->IsInitialBlockDownload());
    std::cout << "[TEST] IsInitialBlockDownload() done" << std::endl;

    //==========================================================================
    // 2️⃣ PARTIAL BLOCK DOWNLOAD PHASE (Blocks: A, B)
    //==========================================================================

    // Simulate downloading block A
    std::cout << "[TEST] About to call MarkBlockRequested for A..." << std::endl;
    header_sync_->MarkBlockRequested(header_a.GetHash().GetHex(), "peer1");
    std::cout << "[TEST] About to call MarkBlockReceived for A..." << std::endl;
    header_sync_->MarkBlockReceived(header_a.GetHash().GetHex(), "peer1");
    std::cout << "[TEST] About to call MarkBlockConnected for A..." << std::endl;
    header_sync_->MarkBlockConnected(header_a.GetHash().GetHex());
    std::cout << "[TEST] Block A marked as connected" << std::endl;

    // Update mock chain manager (simulates consensus accepting block A)
    mock_chainman_->AddBlockIndex(header_a.GetHash().GetHex(), 0, "0000000000000000000000000000000000000000000000000000000000000001");
    mock_chainman_->SetActiveTip(header_a.GetHash().GetHex());

    // Simulate downloading block B
    header_sync_->MarkBlockRequested(header_b.GetHash().GetHex(), "peer1");
    header_sync_->MarkBlockReceived(header_b.GetHash().GetHex(), "peer1");
    header_sync_->MarkBlockConnected(header_b.GetHash().GetHex());

    // Update mock chain manager (simulates consensus accepting block B)
    mock_chainman_->AddBlockIndex(header_b.GetHash().GetHex(), 1, "0000000000000000000000000000000000000000000000000000000000000002");
    mock_chainman_->SetActiveTip(header_b.GetHash().GetHex());

    // Assertions: Partial download state
    EXPECT_EQ(mock_chainman_->GetHeight(), 1);
    EXPECT_EQ(mock_chainman_->GetBestBlockHash(), header_b.GetHash().GetHex());
    EXPECT_EQ(header_sync_->GetBestHeaderHeight(), 3);
    EXPECT_EQ(header_sync_->GetBestHeaderHash(), header_d.GetHash().GetHex());

    // Download queue should contain C only (parent-first rule)
    auto next_blocks = header_sync_->GetBlocksToDownload(10);
    EXPECT_FALSE(next_blocks.empty());
    EXPECT_EQ(next_blocks[0], header_c.GetHash().GetHex());

    // IBD should still be true (header tip D > active tip B)
    EXPECT_TRUE(header_sync_->IsInitialBlockDownload());

    //==========================================================================
    // 3️⃣ CRASH SIMULATION (Hard Stop)
    //==========================================================================

    SimulateCrash();

    //==========================================================================
    // 4️⃣ RESTART PHASE
    //==========================================================================

    RestartNode();

    //==========================================================================
    // 5️⃣ POST-RESTART ASSERTIONS (MOST CRITICAL)
    //==========================================================================

    // Header persistence: Headers A–D must exist
    EXPECT_EQ(header_sync_->GetBestHeaderHeight(), 3);
    EXPECT_EQ(header_sync_->GetBestHeaderHash(), hash_d);

    // Block state persistence: A, B have data; C, D do not
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_a));  // Already have
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_b));  // Already have
    EXPECT_TRUE(header_sync_->IsBlockNeeded(hash_c));   // Ready to download (parent B has data)
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_d));  // NOT ready (parent C doesn't have data yet)

    // Chain state: Active tip still at B
    EXPECT_EQ(mock_chainman_->GetHeight(), 1);
    EXPECT_EQ(mock_chainman_->GetBestBlockHash(), hash_b);

    // IBD signal: MUST STILL BE TRUE (header tip D != active tip B)
    // THIS IS THE KEY ASSERTION - if this fails, H.3 or H.4 is broken
    EXPECT_TRUE(header_sync_->IsInitialBlockDownload());

    // Download queue reconstructed: Should start with C
    auto resumed_blocks = header_sync_->GetBlocksToDownload(10);
    EXPECT_FALSE(resumed_blocks.empty());
    EXPECT_EQ(resumed_blocks[0], hash_c);

    //==========================================================================
    // 6️⃣ RESUME + COMPLETION (Download C, D)
    //==========================================================================

    // Simulate downloading block C
    header_sync_->MarkBlockRequested(hash_c, "peer1");
    header_sync_->MarkBlockReceived(hash_c, "peer1");
    header_sync_->MarkBlockConnected(hash_c);

    // Update mock chain manager
    mock_chainman_->AddBlockIndex(hash_c, 2, "0000000000000000000000000000000000000000000000000000000000000003");
    mock_chainman_->SetActiveTip(hash_c);

    // Simulate downloading block D
    header_sync_->MarkBlockRequested(hash_d, "peer1");
    header_sync_->MarkBlockReceived(hash_d, "peer1");
    header_sync_->MarkBlockConnected(hash_d);

    // Update mock chain manager
    mock_chainman_->AddBlockIndex(hash_d, 3, "0000000000000000000000000000000000000000000000000000000000000004");
    mock_chainman_->SetActiveTip(hash_d);

    //==========================================================================
    // 7️⃣ FINAL ASSERTIONS (IBD Complete)
    //==========================================================================

    // Chain state: Active tip == header tip
    EXPECT_EQ(mock_chainman_->GetHeight(), 3);
    EXPECT_EQ(mock_chainman_->GetBestBlockHash(), hash_d);
    EXPECT_EQ(header_sync_->GetBestHeaderHeight(), 3);
    EXPECT_EQ(header_sync_->GetBestHeaderHash(), hash_d);

    // All blocks downloaded
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_a));
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_b));
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_c));
    EXPECT_FALSE(header_sync_->IsBlockNeeded(hash_d));

    // Download queue empty
    auto final_blocks = header_sync_->GetBlocksToDownload(10);
    EXPECT_TRUE(final_blocks.empty());

    // IBD MUST BE FALSE (converged: header tip == active tip)
    // This proves IBD exits exactly when it should
    std::cout << "[TEST] Final check: best_header_hash=" << header_sync_->GetBestHeaderHash().substr(0,16)
              << " active_tip_hash=" << mock_chainman_->GetBestBlockHash().substr(0,16) << std::endl;
    std::cout << "[TEST] Are they equal? " << (header_sync_->GetBestHeaderHash() == mock_chainman_->GetBestBlockHash()) << std::endl;
    bool ibd_status = header_sync_->IsInitialBlockDownload();
    std::cout << "[TEST] IBD status: " << ibd_status << " (expected: false)" << std::endl;
    EXPECT_FALSE(ibd_status);
}

/**
 * Phase H.5 — Failed Block Persistence Test
 *
 * Verifies that BLOCK_FAILED status survives restart.
 */
TEST_F(HeaderSyncRestartTest, test_failed_blocks_persist) {
    // Create and inject header
    std::string genesis_hash = std::string(64, '0');
    BlockHeader header_a = CreateHeader(1, genesis_hash);

    std::vector<BlockHeader> headers = {header_a};
    ASSERT_TRUE(header_sync_->ProcessHeaders("peer1", headers));

    // Mark block as failed
    header_sync_->MarkBlockFailed(header_a.GetHash().GetHex());

    // Verify block not needed (failed)
    EXPECT_FALSE(header_sync_->IsBlockNeeded(header_a.GetHash().GetHex()));

    // Crash and restart
    SimulateCrash();
    RestartNode();

    // Failed status must persist
    EXPECT_FALSE(header_sync_->IsBlockNeeded(header_a.GetHash().GetHex()));
}

} // namespace test
} // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
