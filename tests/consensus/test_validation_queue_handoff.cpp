#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "common/test_logger.h"
#include "consensus/chainstate_guard.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/validation_queue.h"
#include "daemon/block_relay_manager.h"

namespace dinero::consensus {

namespace {

class MockConsensusUTXOSet final : public IConsensusUTXOSet {
public:
    bool AddCoin(const OutPoint&, const UTXOEntry&) override { return true; }
    std::unique_ptr<UTXOEntry> SpendCoin(const OutPoint&) override { return nullptr; }
    const UTXOEntry* GetCoin(const OutPoint&) const override { return nullptr; }
    bool HaveCoin(const OutPoint&) const override { return false; }
    bool DeleteCoin(const OutPoint&) override { return true; }

    bool ApplyBlock(const Block&, uint32_t, const uint256&, BlockUndo&, UtreexoHash&,
                    std::string&) override {
        return true;
    }

    bool UndoBlock(const Block&, uint32_t, const BlockUndo&, std::string&) override {
        return true;
    }

    bool SupportsSnapshotRestore() const override { return true; }
    UTXOSnapshot Snapshot() const override { return snapshot_; }
    void Restore(const UTXOSnapshot& snapshot) override { snapshot_ = snapshot; }

    uint32_t GetHeight() const override { return height_; }
    const uint256& GetBestBlock() const override { return best_block_; }
    void SetBestBlock(const uint256& hash, uint32_t height) override {
        best_block_ = hash;
        height_ = height;
    }

    UtreexoHash GetUtreexoRoot() const override { return {}; }
    UtreexoForest& GetForest() override { return forest_; }
    const UtreexoForest& GetForest() const override { return forest_; }

    size_t GetSetSize() const override { return 0; }
    size_t GetMemoryUsage() const override { return 0; }
    void Clear() override {}

private:
    mutable UTXOSnapshot snapshot_;
    mutable UtreexoForest forest_;
    uint256 best_block_;
    uint32_t height_ = 0;
};

Block MakeTestBlock(uint32_t nonce) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash.SetNull();
    block.header.merkle_root.SetNull();
    block.header.timestamp = 1700000000 + nonce;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = nonce;
    block.header.utreexo_root.SetNull();
    return block;
}

}  // namespace

TEST(ValidationQueueHandoff, BlockRelayMarksSeenOnlyAfterCanonicalApplyReturns) {
    MockConsensusUTXOSet utxo_set;
    ChainstateGuard guard;
    ValidationQueue queue(&utxo_set, &guard, ValidationQueue::Config::forNormalOperation());

    std::promise<void> apply_started;
    std::promise<void> release_apply;
    std::shared_future<void> release_future = release_apply.get_future().share();
    std::atomic<int> apply_calls{0};

    queue.setParentReadyCallback([](const uint256& prev_hash) { return prev_hash.IsNull(); });
    queue.setBlockApplyCallback([&](const Block& block) -> BlockAcceptResult {
        apply_calls.fetch_add(1);
        apply_started.set_value();
        release_future.wait();
        return BlockAcceptResult::Accepted(block.GetHash(), 0, true);
    });
    queue.start();

    BlockRelayManager relay(nullptr);
    relay.SetValidateBlockCallback([&](const Block& block, const std::string&) {
        return queue.submitAndWait(block, 0, block.header.prev_block_hash).accepted();
    });

    const Block block = MakeTestBlock(1);
    const uint256 block_hash = block.GetHash();

    std::thread relay_thread([&] {
        relay.HandleBlock("peer1", block);
    });

    ASSERT_EQ(apply_started.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(relay.IsBlockSeen(block_hash));

    release_apply.set_value();
    relay_thread.join();

    EXPECT_EQ(apply_calls.load(), 1);
    EXPECT_TRUE(relay.IsBlockSeen(block_hash));
    EXPECT_EQ(queue.getTotalProcessed(), 1u);

    queue.stop();
}

TEST(ValidationQueueHandoff, RejectedQueuedApplyDoesNotMarkBlockSeen) {
    MockConsensusUTXOSet utxo_set;
    ChainstateGuard guard;
    ValidationQueue queue(&utxo_set, &guard, ValidationQueue::Config::forNormalOperation());

    queue.setParentReadyCallback([](const uint256& prev_hash) { return prev_hash.IsNull(); });
    queue.setBlockApplyCallback([](const Block& block) {
        return BlockAcceptResult::Rejected(
            BlockRejectCode::INVALID_TRANSACTION,
            "rejected by canonical apply",
            block.GetHash(),
            0
        );
    });
    queue.start();

    BlockRelayManager relay(nullptr);
    relay.SetValidateBlockCallback([&](const Block& block, const std::string&) {
        return queue.submitAndWait(block, 0, block.header.prev_block_hash).accepted();
    });

    const Block block = MakeTestBlock(2);
    relay.HandleBlock("peer1", block);

    EXPECT_FALSE(relay.IsBlockSeen(block.GetHash()));
    EXPECT_EQ(queue.getTotalProcessed(), 0u);

    queue.stop();
}

}  // namespace dinero::consensus

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
