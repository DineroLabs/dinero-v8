// Forest checkpoint delta campaign — phase 3, bridge-side restore
// (docs/design/forest-checkpoint-deltas.md).
//
// With every-N checkpoints the bridge can no longer read the checkpoint at
// exactly H-1 to serve a historical proof. RestoreHistoricalForest must
// rebuild the forest at ANY height from the nearest checkpoint at-or-below
// plus UD sidecar replay, byte-identical to the continuously-built forest,
// verifying each replayed block against its header's utreexo_root and
// failing loudly on any missing/tampered material.

#include "storage/forest_restore.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <rocksdb/write_batch.h>

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta.h"
#include "consensus/utreexo_delta_codec.h"
#include "dinero/core/consensus/chainparams.h"
#include "primitives/block.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::MakeUtreexoDeltaUndoKey;
using dinero::SerializeUtreexoDelta;
using dinero::Status;
using dinero::consensus::UtreexoDelta;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;
using dinero::storage::RestoreHistoricalForest;

namespace {

UtreexoHash MakeLeaf(uint64_t ordinal) {
    UtreexoHash h(32);
    for (size_t i = 0; i < 32; ++i) {
        h[i] = static_cast<uint8_t>((ordinal * 131) ^ (0x3C + i));
    }
    h[28] = static_cast<uint8_t>(ordinal >> 0);
    h[29] = static_cast<uint8_t>(ordinal >> 8);
    h[30] = static_cast<uint8_t>(ordinal >> 16);
    h[31] = static_cast<uint8_t>(ordinal >> 24);
    return h;
}

dinero::uint256 MakeBlockHash(uint32_t height) {
    dinero::uint256 h;
    for (int i = 0; i < 32; ++i) {
        h.data[i] = static_cast<uint8_t>((height * 37) ^ (0x91 + i));
    }
    h.data[0] = static_cast<uint8_t>(height);
    return h;
}

// A ChainDB pre-populated exactly like an every-5-checkpoint node writes it:
// header + height index + UD sidecar every block, full checkpoints only at
// heights % 5 == 0 (and genesis).
class ForestRestoreFixture : public ::testing::Test {
protected:
    static constexpr uint32_t kBlocks = 12;
    static constexpr uint32_t kInterval = 5;

    void SetUp() override {
        // Testnet activates canonical roots at height 0 — the whole synthetic
        // chain runs in canonical mode, matching post-fork mainnet reality.
        dinero::SelectParams(dinero::Chain::TESTNET);

        dir_ = std::filesystem::temp_directory_path() /
               ("forest_restore_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
        ASSERT_EQ(db_.init(dir_.string()), Status::Ok);

        forest_.setCanonicalEmptyRoots(true);
        serialized_after_.assign(kBlocks + 1, {});
        serialized_after_[0] = forest_.serialize();

        ChainWriteToken token = ChainWriteToken::CreateForTesting();
        ASSERT_EQ(db_.putUtreexoCheckpoint(token, 0, serialized_after_[0]),
                  Status::Ok);

        std::mt19937_64 rng(20260717);
        uint64_t next_ordinal = 0;
        std::vector<UtreexoHash> live;

        for (uint32_t h = 1; h <= kBlocks; ++h) {
            // Validator two-pass order: deletes first, then adds.
            UtreexoDelta delta;
            delta.numLeavesBefore = forest_.getNumLeaves();
            const size_t deletes =
                live.empty() ? 0 : (rng() % std::min<size_t>(3, live.size()));
            for (size_t i = 0; i < deletes; ++i) {
                const size_t victim = rng() % live.size();
                const UtreexoHash leaf = live[victim];
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
                const auto pos = forest_.findLeafPosition(leaf);
                ASSERT_TRUE(pos.has_value());
                ASSERT_TRUE(forest_.removeAtKnownPosition(*pos, leaf));
                delta.recordDelete(*pos, leaf);
            }
            const size_t adds = 2 + (rng() % 4);
            for (size_t i = 0; i < adds; ++i) {
                const UtreexoHash leaf = MakeLeaf(next_ordinal++);
                const uint64_t pos = forest_.add(leaf);
                ASSERT_NE(pos, UINT64_MAX);
                delta.recordAdd(leaf, pos);
                live.push_back(leaf);
            }
            serialized_after_[h] = forest_.serialize();

            const dinero::uint256 hash = MakeBlockHash(h);
            dinero::BlockHeader header{};
            header.version = 1;
            header.prev_block_hash = MakeBlockHash(h - 1);
            const auto commitment = forest_.getCommitment();
            ASSERT_EQ(commitment.size(), 32u);
            std::memcpy(header.utreexo_root.data, commitment.data(), 32);
            header.timestamp = 1000 + h;

            ASSERT_EQ(db_.putHeader(token, hash, header, static_cast<int>(h),
                                    dinero::arith_uint256(h)),
                      Status::Ok);
            ASSERT_EQ(db_.putHeightIndex(token, static_cast<int>(h), hash),
                      Status::Ok);

            std::string blob, error;
            ASSERT_TRUE(SerializeUtreexoDelta(delta, blob, error)) << error;
            rocksdb::WriteBatch batch;
            batch.Put(MakeUtreexoDeltaUndoKey(hash), blob);
            ASSERT_EQ(db_.writeBatch(token, std::move(batch), true), Status::Ok);

            if (h % kInterval == 0) {
                ASSERT_EQ(db_.putUtreexoCheckpoint(token, static_cast<int>(h),
                                                   serialized_after_[h]),
                          Status::Ok);
            }
        }
    }

    void TearDown() override {
        db_.close();
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
    ChainDB db_;
    UtreexoForest forest_;
    std::vector<std::vector<uint8_t>> serialized_after_;
};

TEST_F(ForestRestoreFixture, NearestCheckpointQuery) {
    auto r7 = db_.getLatestUtreexoCheckpointAtOrBelow(7);
    ASSERT_EQ(r7.status(), Status::Ok);
    EXPECT_EQ(r7.value().first, 5);

    auto r10 = db_.getLatestUtreexoCheckpointAtOrBelow(10);
    ASSERT_EQ(r10.status(), Status::Ok);
    EXPECT_EQ(r10.value().first, 10);

    auto r12 = db_.getLatestUtreexoCheckpointAtOrBelow(12);
    ASSERT_EQ(r12.status(), Status::Ok);
    EXPECT_EQ(r12.value().first, 10);

    auto r4 = db_.getLatestUtreexoCheckpointAtOrBelow(4);
    ASSERT_EQ(r4.status(), Status::Ok);
    EXPECT_EQ(r4.value().first, 0);
}

TEST_F(ForestRestoreFixture, RestoreAtExactCheckpointHeight) {
    UtreexoForest restored;
    std::string error;
    ASSERT_EQ(RestoreHistoricalForest(db_, 10, restored, error), Status::Ok)
        << error;
    EXPECT_EQ(restored.serialize(), serialized_after_[10]);
}

TEST_F(ForestRestoreFixture, RestoreBetweenCheckpointsViaReplay) {
    for (uint32_t target : {6u, 7u, 9u, 11u, 12u}) {
        SCOPED_TRACE("target=" + std::to_string(target));
        UtreexoForest restored;
        std::string error;
        ASSERT_EQ(RestoreHistoricalForest(db_, target, restored, error),
                  Status::Ok)
            << error;
        EXPECT_EQ(restored.serialize(), serialized_after_[target]);
        EXPECT_EQ(restored.getCommitment(), // proof-serving equivalence
                  UtreexoForest::deserialize(serialized_after_[target])
                      .getCommitment());
    }
}

TEST_F(ForestRestoreFixture, MissingSidecarFailsLoudly) {
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    rocksdb::WriteBatch batch;
    batch.Delete(MakeUtreexoDeltaUndoKey(MakeBlockHash(8)));
    ASSERT_EQ(db_.writeBatch(token, std::move(batch), true), Status::Ok);

    UtreexoForest restored;
    std::string error;
    EXPECT_NE(RestoreHistoricalForest(db_, 9, restored, error), Status::Ok);
    EXPECT_NE(error.find("8"), std::string::npos) << error;
}

TEST_F(ForestRestoreFixture, TamperedCheckpointHeightHeaderFailsLoudly) {
    // Exact-checkpoint-hit restores replay nothing, so the checkpoint blob
    // itself must be verified against its own height's header root — a
    // corrupt-but-parseable checkpoint must not restore silently.
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    auto header_result = db_.getHeader(MakeBlockHash(10));
    ASSERT_EQ(header_result.status(), Status::Ok);
    dinero::BlockHeader tampered = header_result.value();
    tampered.utreexo_root.data[0] ^= 0xFF;
    ASSERT_EQ(db_.putHeader(token, MakeBlockHash(10), tampered, 10,
                            dinero::arith_uint256(10)),
              Status::Ok);

    UtreexoForest restored;
    std::string error;
    EXPECT_NE(RestoreHistoricalForest(db_, 10, restored, error), Status::Ok);
    EXPECT_NE(error.find("10"), std::string::npos) << error;
}

TEST_F(ForestRestoreFixture, TamperedHeaderRootFailsLoudly) {
    // Rewrite block 12's header with a corrupted utreexo_root; the replayed
    // forest must refuse instead of serving proofs off unverified state.
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    auto header_result = db_.getHeader(MakeBlockHash(12));
    ASSERT_EQ(header_result.status(), Status::Ok);
    dinero::BlockHeader tampered = header_result.value();
    tampered.utreexo_root.data[0] ^= 0xFF;
    ASSERT_EQ(db_.putHeader(token, MakeBlockHash(12), tampered, 12,
                            dinero::arith_uint256(12)),
              Status::Ok);

    UtreexoForest restored;
    std::string error;
    EXPECT_NE(RestoreHistoricalForest(db_, 12, restored, error), Status::Ok);
    EXPECT_NE(error.find("12"), std::string::npos) << error;
}

}  // namespace
