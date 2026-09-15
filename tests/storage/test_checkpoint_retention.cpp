// Retention safety on a real RocksDB database. These tests exercise the
// offline retention pass and historical forest/proof reconstruction; they
// do not substitute for the daemon's disconnect/connect reorg gates.

#include "storage/checkpoint_retention.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

#include <rocksdb/write_batch.h>

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta.h"
#include "consensus/utreexo_delta_codec.h"
#include "dinero/core/consensus/chainparams.h"
#include "primitives/block.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "storage/forest_restore.h"

namespace {

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Status;
using dinero::consensus::UtreexoDelta;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;
using dinero::storage::CheckpointRetentionPass;
using dinero::storage::CheckpointRetentionPhase;
using dinero::storage::CheckpointRetentionPolicy;
using dinero::storage::RestoreHistoricalForest;

dinero::uint256 BlockHash(uint32_t height) {
    dinero::uint256 hash;
    for (size_t i = 0; i < 32; ++i) {
        hash.data[i] = static_cast<uint8_t>((height * 37) ^ (0x91 + i));
    }
    for (size_t i = 0; i < 4; ++i) {
        hash.data[i] = static_cast<uint8_t>(height >> (i * 8));
    }
    return hash;
}

UtreexoHash Leaf(uint64_t ordinal) {
    UtreexoHash leaf(32);
    for (size_t i = 0; i < 32; ++i) {
        leaf[i] = static_cast<uint8_t>((ordinal * 131) ^ (0x3c + i));
    }
    for (size_t i = 0; i < 8; ++i) {
        leaf[24 + i] = static_cast<uint8_t>(ordinal >> (i * 8));
    }
    return leaf;
}

class CheckpointRetentionFixture : public ::testing::Test {
protected:
    static constexpr uint32_t kTip = 32;
    static constexpr uint32_t kFuture = 80;
    static constexpr size_t kMaxSteps = 10000;

    static CheckpointRetentionPolicy Policy() {
        CheckpointRetentionPolicy policy;
        policy.recent_blocks = 6;
        policy.historical_interval = 10;
        policy.replay_blocks_per_step = 2;
        policy.delete_heights_per_batch = 3;
        policy.protected_heights = {13};
        return policy;
    }

    void SetUp() override {
        dinero::SelectParams(dinero::Chain::TESTNET);
        static std::atomic<uint64_t> sequence{0};
        dir_ = std::filesystem::temp_directory_path() /
            ("checkpoint_retention_" + std::to_string(::getpid()) + "_" +
             std::to_string(sequence.fetch_add(1)));
        ASSERT_FALSE(std::filesystem::exists(dir_));
        ASSERT_EQ(db_.init(dir_.string()), Status::Ok);

        UtreexoForest forest;
        forest.setCanonicalEmptyRoots(true);
        std::mt19937_64 rng(20260914);
        uint64_t next_leaf = 0;
        std::vector<UtreexoHash> live;
        serialized_.resize(kTip + 1);
        checksums_.resize(kTip + 1);
        live_after_.resize(kTip + 1);
        proofs_after_.resize(kTip + 1);
        sidecars_.resize(kTip + 1);

        for (uint32_t height = 0; height <= kTip; ++height) {
            SCOPED_TRACE(height);
            if (height > 0) {
                // Real two-pass transitions exercise deleted leaf positions,
                // forest holes, and proofs, rather than append-only roots.
                UtreexoDelta delta;
                delta.numLeavesBefore = forest.getNumLeaves();
                const size_t deletes = live.empty() ? 0 :
                    rng() % (std::min<size_t>(3, live.size()) + 1);
                for (size_t i = 0; i < deletes; ++i) {
                    const size_t victim = rng() % live.size();
                    const auto leaf = live[victim];
                    const auto position = forest.findLeafPosition(leaf);
                    ASSERT_TRUE(position.has_value());
                    ASSERT_TRUE(forest.removeAtKnownPosition(*position, leaf));
                    delta.recordDelete(*position, leaf);
                    live.erase(live.begin() +
                               static_cast<std::ptrdiff_t>(victim));
                }
                const size_t adds = 2 + rng() % 4;
                for (size_t i = 0; i < adds; ++i) {
                    const auto leaf = Leaf(next_leaf++);
                    const auto position = forest.add(leaf);
                    ASSERT_NE(position, UINT64_MAX);
                    delta.recordAdd(leaf, position);
                    live.push_back(leaf);
                }
                std::string error;
                ASSERT_TRUE(dinero::SerializeUtreexoDelta(
                    delta, sidecars_[height], error)) << error;
                rocksdb::WriteBatch batch;
                batch.Put(dinero::MakeUtreexoDeltaUndoKey(BlockHash(height)),
                          sidecars_[height]);
                ASSERT_EQ(db_.writeBatch(token_, std::move(batch), true),
                          Status::Ok);
            }

            serialized_[height] = forest.serialize();
            live_after_[height] = live;
            for (const auto& leaf : live) {
                const auto position = forest.findLeafPosition(leaf);
                ASSERT_TRUE(position.has_value());
                const auto proof = forest.prove(*position);
                ASSERT_TRUE(proof.has_value());
                proofs_after_[height].push_back(proof->serialize());
            }

            dinero::BlockHeader header{};
            header.version = 1;
            if (height > 0) header.prev_block_hash = BlockHash(height - 1);
            const auto commitment = forest.getCommitment();
            ASSERT_EQ(commitment.size(), 32u);
            std::memcpy(header.utreexo_root.data, commitment.data(), 32);
            header.timestamp = 1000 + height;
            ASSERT_EQ(db_.putHeader(token_, BlockHash(height), header,
                                   static_cast<int>(height),
                                   dinero::arith_uint256(height)), Status::Ok);
            ASSERT_EQ(db_.putHeightIndex(token_, static_cast<int>(height),
                                        BlockHash(height)), Status::Ok);
            ASSERT_EQ(db_.putUtreexoCheckpointWithChecksum(
                token_, static_cast<int>(height), serialized_[height]),
                Status::Ok);
            const auto checksum = db_.getUtreexoChecksum(height);
            ASSERT_EQ(checksum.status(), Status::Ok);
            checksums_[height] = checksum.value();
            ASSERT_EQ(db_.putTransitionProof(token_, height,
                {0x51, static_cast<uint8_t>(height), 0xA7}), Status::Ok);
        }

        ASSERT_EQ(db_.setTip(token_, BlockHash(kTip), kTip,
                            dinero::arith_uint256(kTip)), Status::Ok);
        ASSERT_EQ(db_.setValidatedTip(token_, BlockHash(kTip), kTip), Status::Ok);
        ASSERT_EQ(db_.putUtreexoMeta(token_, "retention-test-sentinel",
                                   "keep historical reconstruction material"),
                  Status::Ok);
        ASSERT_EQ(db_.putUtreexoCheckpointWithChecksum(token_, kFuture,
                                                       serialized_.back()),
                  Status::Ok);
    }

    void TearDown() override {
        db_.close();
        std::filesystem::remove_all(dir_);
    }

    void Run(CheckpointRetentionPass& pass) {
        for (size_t i = 0; i < kMaxSteps && !pass.done(); ++i) {
            std::string error;
            ASSERT_EQ(pass.step(token_, error), Status::Ok) << error;
        }
        ASSERT_TRUE(pass.done()) << "retention pass did not finish";
    }

    void AdvanceToDelete(CheckpointRetentionPass& pass) {
        for (size_t i = 0; i < kMaxSteps && !pass.done(); ++i) {
            if (pass.progress().phase == CheckpointRetentionPhase::DeleteInterval)
                return;
            std::string error;
            ASSERT_EQ(pass.step(token_, error), Status::Ok) << error;
        }
        FAIL() << "pass never reached deletion phase";
    }

    void ExpectHeights(const std::set<uint32_t>& expected) {
        const auto heights = db_.listUtreexoCheckpoints();
        ASSERT_EQ(heights.status(), Status::Ok);
        std::set<uint32_t> actual;
        for (int height : heights.value()) actual.insert(height);
        auto with_future = expected;
        with_future.insert(kFuture);
        EXPECT_EQ(actual, with_future);
        for (uint32_t height = 0; height <= kTip; ++height) {
            SCOPED_TRACE(height);
            const auto checkpoint = db_.getUtreexoCheckpoint(height);
            const auto checksum = db_.getUtreexoChecksum(height);
            if (expected.count(height)) {
                ASSERT_EQ(checkpoint.status(), Status::Ok);
                EXPECT_EQ(checkpoint.value(), serialized_[height]);
                ASSERT_EQ(checksum.status(), Status::Ok);
                EXPECT_EQ(checksum.value(), checksums_[height]);
            } else {
                EXPECT_EQ(checkpoint.status(), Status::NotFound);
                EXPECT_EQ(checksum.status(), Status::NotFound);
            }
        }
        const auto future = db_.getUtreexoCheckpoint(kFuture);
        ASSERT_EQ(future.status(), Status::Ok);
        EXPECT_EQ(future.value(), serialized_.back());
        const auto future_checksum = db_.getUtreexoChecksum(kFuture);
        ASSERT_EQ(future_checksum.status(), Status::Ok);
        EXPECT_EQ(future_checksum.value(), checksums_.back());
    }

    static std::set<uint32_t> AllHeights() {
        std::set<uint32_t> heights;
        for (uint32_t height = 0; height <= kTip; ++height) heights.insert(height);
        return heights;
    }

    static std::set<uint32_t> RetainedHeights() {
        return {0, 10, 13, 20, 26, 27, 28, 29, 30, 31, 32};
    }

    void ExpectAuxiliaryMaterial() {
        const auto meta = db_.getUtreexoMeta("retention-test-sentinel");
        ASSERT_EQ(meta.status(), Status::Ok);
        EXPECT_EQ(meta.value(), "keep historical reconstruction material");
        for (uint32_t height = 0; height <= kTip; ++height) {
            SCOPED_TRACE(height);
            const auto proof = db_.getTransitionProof(height);
            ASSERT_EQ(proof.status(), Status::Ok);
            EXPECT_EQ(proof.value(), (std::vector<uint8_t>{
                0x51, static_cast<uint8_t>(height), 0xA7}));
            if (height == 0) continue;
            std::string sidecar;
            ASSERT_EQ(db_.getRaw(dinero::MakeUtreexoDeltaUndoKey(BlockHash(height)),
                                sidecar), Status::Ok);
            EXPECT_EQ(sidecar, sidecars_[height]);
        }
    }

    void ExpectForestAndProofsAtEveryHeight(uint32_t minimum = 0) {
        // Descending requests include all heights on both sides of each
        // pruned span, as restart and old-height proof callers can request.
        for (int height = kTip; height >= static_cast<int>(minimum); --height) {
            SCOPED_TRACE(height);
            UtreexoForest restored;
            std::string error;
            ASSERT_EQ(RestoreHistoricalForest(db_, height, restored, error),
                      Status::Ok) << error;
            EXPECT_EQ(restored.serialize(), serialized_[height]);
            ASSERT_EQ(live_after_[height].size(), proofs_after_[height].size());
            for (size_t i = 0; i < live_after_[height].size(); ++i) {
                const auto position = restored.findLeafPosition(live_after_[height][i]);
                ASSERT_TRUE(position.has_value());
                const auto proof = restored.prove(*position);
                ASSERT_TRUE(proof.has_value());
                EXPECT_EQ(proof->serialize(), proofs_after_[height][i]);
                EXPECT_TRUE(proof->verify(live_after_[height][i],
                                          restored.getRoots()));
            }
        }
    }

    void DeleteSidecar(uint32_t height) {
        rocksdb::WriteBatch batch;
        batch.Delete(dinero::MakeUtreexoDeltaUndoKey(BlockHash(height)));
        ASSERT_EQ(db_.writeBatch(token_, std::move(batch), true), Status::Ok);
    }

    ChainWriteToken token_ = ChainWriteToken::CreateForTesting();
    std::filesystem::path dir_;
    ChainDB db_;
    std::vector<std::vector<uint8_t>> serialized_;
    std::vector<std::vector<uint8_t>> checksums_;
    std::vector<std::vector<UtreexoHash>> live_after_;
    std::vector<std::vector<std::vector<uint8_t>>> proofs_after_;
    std::vector<std::string> sidecars_;
};

TEST_F(CheckpointRetentionFixture, DefaultAuditVerifiesWithoutMutating) {
    CheckpointRetentionPass pass(db_, Policy());
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_GT(pass.progress().verified_intervals, 0u);
    EXPECT_GT(pass.progress().replayed_blocks, 0u);
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
    ASSERT_NO_FATAL_FAILURE(ExpectAuxiliaryMaterial());
}

TEST_F(CheckpointRetentionFixture, KeepsSparseRecentProtectedEarliestAndFuture) {
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_EQ(pass.progress().skipped_intervals, 0u);
    EXPECT_EQ(pass.progress().deleted_height_keys, 22u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(RetainedHeights()));
    ASSERT_NO_FATAL_FAILURE(ExpectAuxiliaryMaterial());
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
}

TEST_F(CheckpointRetentionFixture, ReplayAndAtomicDeleteBatchesAreBounded) {
    const auto policy = Policy();
    CheckpointRetentionPass pass(db_, policy, true);
    uint64_t previous_replayed = 0;
    uint64_t previous_deleted = 0;
    size_t previous_count = kTip + 2;  // genesis through tip, plus future
    for (size_t i = 0; i < kMaxSteps && !pass.done(); ++i) {
        std::string error;
        ASSERT_EQ(pass.step(token_, error), Status::Ok) << error;
        const auto& progress = pass.progress();
        EXPECT_LE(progress.replayed_blocks - previous_replayed,
                  policy.replay_blocks_per_step);
        EXPECT_LE(progress.deleted_height_keys - previous_deleted,
                  policy.delete_heights_per_batch);
        const auto heights = db_.listUtreexoCheckpoints();
        ASSERT_EQ(heights.status(), Status::Ok);
        EXPECT_LE(previous_count - heights.value().size(),
                  policy.delete_heights_per_batch);
        for (uint32_t height = 0; height <= kTip; ++height) {
            // Each committed step exposes both U and C, or neither.
            EXPECT_EQ(db_.getUtreexoCheckpoint(height).status(),
                      db_.getUtreexoChecksum(height).status()) << height;
        }
        previous_replayed = progress.replayed_blocks;
        previous_deleted = progress.deleted_height_keys;
        previous_count = heights.value().size();
    }
    ASSERT_TRUE(pass.done());
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(RetainedHeights()));
}

TEST_F(CheckpointRetentionFixture, ImportedDatabasePreservesEarliestCheckpoint) {
    for (uint32_t height = 0; height < 13; ++height) {
        ASSERT_EQ(db_.deleteUtreexoCheckpointWithChecksum(token_, height),
                  Status::Ok);
        if (height) ASSERT_NO_FATAL_FAILURE(DeleteSidecar(height));
    }
    // The imported checkpoint starts after this delta; restoration must not
    // require unavailable pre-import transitions or a genesis checkpoint.
    ASSERT_NO_FATAL_FAILURE(DeleteSidecar(13));
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    ASSERT_NO_FATAL_FAILURE(ExpectHeights({13, 20, 26, 27, 28, 29, 30, 31, 32}));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight(13));
}

TEST_F(CheckpointRetentionFixture, MissingDeltaPreservesWholeIntervalButLaterIntervalsPrune) {
    ASSERT_NO_FATAL_FAILURE(DeleteSidecar(8));
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_GE(pass.progress().skipped_intervals, 1u);
    auto expected = RetainedHeights();
    for (uint32_t height = 1; height < 10; ++height) expected.insert(height);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(expected));
    // Every original height remains usable: retained checkpoints bridge
    // over the unavailable delta in the skipped interval.
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
}

TEST_F(CheckpointRetentionFixture, CorruptDeltaPreservesWholeIntervalButLaterIntervalsPrune) {
    rocksdb::WriteBatch batch;
    batch.Put(dinero::MakeUtreexoDeltaUndoKey(BlockHash(8)), "invalid delta");
    ASSERT_EQ(db_.writeBatch(token_, std::move(batch), true), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_GE(pass.progress().skipped_intervals, 1u);
    auto expected = RetainedHeights();
    for (uint32_t height = 1; height < 10; ++height) expected.insert(height);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(expected));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
}

TEST_F(CheckpointRetentionFixture, ReplayRootMismatchPreservesWholeInterval) {
    auto result = db_.getHeader(BlockHash(8));
    ASSERT_EQ(result.status(), Status::Ok);
    auto header = result.value();
    header.utreexo_root.data[0] ^= 0xFF;
    ASSERT_EQ(db_.putHeader(token_, BlockHash(8), header, 8,
                           dinero::arith_uint256(8)), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_GE(pass.progress().skipped_intervals, 1u);
    auto expected = RetainedHeights();
    for (uint32_t height = 1; height < 10; ++height) expected.insert(height);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(expected));
}

TEST_F(CheckpointRetentionFixture, HeaderPreviousHashMismatchFailsBeforeAnyDeletion) {
    auto result = db_.getHeader(BlockHash(8));
    ASSERT_EQ(result.status(), Status::Ok);
    auto header = result.value();
    header.prev_block_hash = BlockHash(6);
    ASSERT_EQ(db_.putHeader(token_, BlockHash(8), header, 8,
                           dinero::arith_uint256(8)), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    Status status = Status::Ok;
    std::string error;
    for (size_t i = 0; i < kMaxSteps && !pass.done() && status == Status::Ok; ++i)
        status = pass.step(token_, error);
    EXPECT_EQ(status, Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, StaleHeightIndexFailsBeforeAnyDeletion) {
    ASSERT_EQ(db_.putHeightIndex(token_, 7, BlockHash(707)), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    Status status = Status::Ok;
    std::string error;
    for (size_t i = 0; i < kMaxSteps && !pass.done() && status == Status::Ok; ++i)
        status = pass.step(token_, error);
    EXPECT_EQ(status, Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, EitherAnchorBytesChangingWithUnchangedChecksumAbortsDeletion) {
    for (bool change_end : {false, true}) {
        SCOPED_TRACE(change_end ? "end anchor" : "start anchor");
        CheckpointRetentionPass pass(db_, Policy(), true);
        ASSERT_NO_FATAL_FAILURE(AdvanceToDelete(pass));
        ASSERT_EQ(pass.progress().deleted_height_keys, 0u);
        const uint32_t anchor = change_end ? pass.progress().interval_end :
                                             pass.progress().interval_start;
        ASSERT_LE(anchor, kTip);
        auto altered = serialized_[anchor];
        ASSERT_FALSE(altered.empty());
        altered.back() ^= 0x01;
        // Intentionally leave C untouched: comparing only C is insufficient.
        ASSERT_EQ(db_.putUtreexoCheckpoint(token_, anchor, altered), Status::Ok);
        const auto checksum = db_.getUtreexoChecksum(anchor);
        ASSERT_EQ(checksum.status(), Status::Ok);
        EXPECT_EQ(checksum.value(), checksums_[anchor]);
        std::string error;
        EXPECT_NE(pass.step(token_, error), Status::Ok) << error;
        EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
        ASSERT_EQ(db_.putUtreexoCheckpointWithChecksum(token_, anchor,
                                                       serialized_[anchor]),
                  Status::Ok);
        ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
    }
}

TEST_F(CheckpointRetentionFixture, SameHeightTipIdentityChangeAbortsDeletion) {
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(AdvanceToDelete(pass));
    ASSERT_EQ(db_.setTip(token_, BlockHash(3200), kTip,
                        dinero::arith_uint256(kTip)), Status::Ok);
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, DifferentValidLegacyAnchorBytesAbortDeletionWithoutChecksum) {
    // No C record exists before or after mutation, isolating actual U-byte
    // identity from the checksum-corruption defense tested above.
    ASSERT_EQ(db_.deleteUtreexoCheckpointWithChecksum(token_, 0), Status::Ok);
    ASSERT_EQ(db_.putUtreexoCheckpoint(token_, 0, serialized_[0]), Status::Ok);
    ASSERT_EQ(db_.getUtreexoChecksum(0).status(), Status::NotFound);
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(AdvanceToDelete(pass));
    ASSERT_EQ(pass.progress().interval_start, 0u);
    ASSERT_EQ(pass.progress().deleted_height_keys, 0u);
    const auto before = db_.listUtreexoCheckpoints();
    ASSERT_EQ(before.status(), Status::Ok);

    // A complete, parseable forest prevents a deserialization rejection from
    // accidentally substituting for the verified-anchor identity check.
    ASSERT_NE(serialized_[1], serialized_[0]);
    const auto replacement = UtreexoForest::deserialize(serialized_[1]);
    ASSERT_GT(replacement.getNumLeaves(), 0u);
    ASSERT_EQ(db_.putUtreexoCheckpoint(token_, 0, serialized_[1]), Status::Ok);
    EXPECT_EQ(db_.getUtreexoChecksum(0).status(), Status::NotFound);
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_NE(error.find("anchor-changed"), std::string::npos) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    const auto after = db_.listUtreexoCheckpoints();
    ASSERT_EQ(after.status(), Status::Ok);
    EXPECT_EQ(after.value(), before.value());
}

TEST_F(CheckpointRetentionFixture, StorageAndValidatedTipMismatchRejectsBeforeDeletion) {
    ASSERT_EQ(db_.setValidatedTip(token_, BlockHash(kTip - 1), kTip - 1), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, ValidatedTipIdentityChangeAfterVerificationAbortsDeletion) {
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(AdvanceToDelete(pass));
    ASSERT_EQ(db_.setValidatedTip(token_, BlockHash(3200), kTip), Status::Ok);
    // The storage marker stays exactly pinned while the validated identity
    // changes, so checking only getTip() cannot satisfy this regression.
    const auto storage_tip = db_.getTip();
    ASSERT_EQ(storage_tip.status(), Status::Ok);
    EXPECT_EQ(storage_tip.value().hash, BlockHash(kTip));
    EXPECT_EQ(storage_tip.value().height, static_cast<int>(kTip));
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, PersistedImportBaseIsProtectedWithoutExplicitPolicyEntry) {
    ASSERT_EQ(db_.replacePreBaseCoins(token_, BlockHash(13), 13, {}), Status::Ok);
    auto policy = Policy();
    policy.protected_heights.clear();
    CheckpointRetentionPass pass(db_, policy, true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(RetainedHeights()));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
    const auto marker = db_.getPreBaseCoinSetBase();
    ASSERT_EQ(marker.status(), Status::Ok);
    EXPECT_EQ(marker.value().first, BlockHash(13));
    EXPECT_EQ(marker.value().second, 13u);
}

TEST_F(CheckpointRetentionFixture, ImportBaseAboveTipRejectsWithoutDeleting) {
    ASSERT_EQ(db_.replacePreBaseCoins(token_, BlockHash(kTip + 1), kTip + 1, {}),
              Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, ImportBaseOnDifferentBranchRejectsWithoutDeleting) {
    ASSERT_EQ(db_.replacePreBaseCoins(token_, BlockHash(1300), 13, {}), Status::Ok);
    CheckpointRetentionPass pass(db_, Policy(), true);
    Status status = Status::Ok;
    std::string error;
    for (size_t i = 0; i < kMaxSteps && !pass.done() && status == Status::Ok; ++i)
        status = pass.step(token_, error);
    EXPECT_EQ(status, Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
}

TEST_F(CheckpointRetentionFixture, TipExtensionAfterFirstBatchPreventsFurtherDeletion) {
    CheckpointRetentionPass pass(db_, Policy(), true);
    for (size_t i = 0; i < kMaxSteps && pass.progress().deleted_height_keys == 0; ++i) {
        ASSERT_FALSE(pass.done());
        std::string error;
        ASSERT_EQ(pass.step(token_, error), Status::Ok) << error;
    }
    const auto deleted = pass.progress().deleted_height_keys;
    ASSERT_GT(deleted, 0u);
    const auto before = db_.listUtreexoCheckpoints();
    ASSERT_EQ(before.status(), Status::Ok);
    ASSERT_EQ(db_.setTip(token_, BlockHash(kTip + 1), kTip + 1,
                        dinero::arith_uint256(kTip + 1)), Status::Ok);
    std::string error;
    EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
    EXPECT_EQ(pass.progress().deleted_height_keys, deleted);
    const auto after = db_.listUtreexoCheckpoints();
    ASSERT_EQ(after.status(), Status::Ok);
    EXPECT_EQ(after.value(), before.value());
}

TEST_F(CheckpointRetentionFixture, ReopenAfterFirstDeleteBatchRestoresEveryHeightAndProof) {
    {
        CheckpointRetentionPass pass(db_, Policy(), true);
        for (size_t i = 0; i < kMaxSteps && pass.progress().deleted_height_keys == 0; ++i) {
            ASSERT_FALSE(pass.done());
            std::string error;
            ASSERT_EQ(pass.step(token_, error), Status::Ok) << error;
        }
        ASSERT_GT(pass.progress().deleted_height_keys, 0u);
        ASSERT_LE(pass.progress().deleted_height_keys,
                  Policy().delete_heights_per_batch);
        ASSERT_FALSE(pass.done());
    }
    db_.close();
    ASSERT_EQ(db_.init(dir_.string()), Status::Ok);
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
    ASSERT_NO_FATAL_FAILURE(ExpectAuxiliaryMaterial());

    // The operation has no external resume journal. Replanning from durable
    // checkpoints must safely finish a pass interrupted after one batch.
    CheckpointRetentionPass resumed(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(resumed));
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(RetainedHeights()));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
    ASSERT_NO_FATAL_FAILURE(ExpectAuxiliaryMaterial());
}

TEST_F(CheckpointRetentionFixture, MissingIntermediateCheckpointSlotsDoNotBreakBatchedDeletion) {
    for (uint32_t height : {2u, 4u, 8u, 14u, 17u, 22u}) {
        ASSERT_EQ(db_.deleteUtreexoCheckpointWithChecksum(token_, height), Status::Ok);
    }
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    ASSERT_NO_FATAL_FAILURE(ExpectHeights(RetainedHeights()));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
}

TEST_F(CheckpointRetentionFixture, MissingExactAnchorsKeepTheirNearestPredecessors) {
    for (uint32_t height : {10u, 13u, 26u}) {
        ASSERT_EQ(db_.deleteUtreexoCheckpointWithChecksum(token_, height), Status::Ok);
    }
    auto policy = Policy();
    policy.protected_heights = {80, 13, 13, 1000};
    CheckpointRetentionPass pass(db_, policy, true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    ASSERT_NO_FATAL_FAILURE(ExpectHeights({0, 9, 12, 20, 25, 27, 28, 29, 30, 31, 32}));
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
}

TEST_F(CheckpointRetentionFixture, LegacyAnchorsWithoutChecksumsStillReconstructProofs) {
    for (uint32_t height : {0u, 10u}) {
        ASSERT_EQ(db_.deleteUtreexoCheckpointWithChecksum(token_, height), Status::Ok);
        ASSERT_EQ(db_.putUtreexoCheckpoint(token_, height, serialized_[height]), Status::Ok);
    }
    CheckpointRetentionPass pass(db_, Policy(), true);
    ASSERT_NO_FATAL_FAILURE(Run(pass));
    EXPECT_EQ(pass.progress().skipped_intervals, 0u);
    EXPECT_EQ(pass.progress().deleted_height_keys, 22u);
    EXPECT_EQ(db_.getUtreexoChecksum(0).status(), Status::NotFound);
    EXPECT_EQ(db_.getUtreexoChecksum(10).status(), Status::NotFound);
    ASSERT_NO_FATAL_FAILURE(ExpectForestAndProofsAtEveryHeight());
    ASSERT_NO_FATAL_FAILURE(ExpectAuxiliaryMaterial());
}

TEST_F(CheckpointRetentionFixture, ZeroPolicyBoundsAreRejectedWithoutMutation) {
    for (unsigned int field = 0; field < 4; ++field) {
        SCOPED_TRACE(field);
        auto policy = Policy();
        switch (field) {
        case 0: policy.recent_blocks = 0; break;
        case 1: policy.historical_interval = 0; break;
        case 2: policy.replay_blocks_per_step = 0; break;
        case 3: policy.delete_heights_per_batch = 0; break;
        }
        CheckpointRetentionPass pass(db_, policy, true);
        std::string error;
        EXPECT_EQ(pass.step(token_, error), Status::Invalid) << error;
        EXPECT_EQ(pass.progress().deleted_height_keys, 0u);
        ASSERT_NO_FATAL_FAILURE(ExpectHeights(AllHeights()));
    }
}

}  // namespace
