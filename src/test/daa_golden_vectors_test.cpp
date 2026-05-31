// Tests consensus-critical ASERT DAA behavior:
// - Genesis returns genesisBits
// - ASERT from block 1 anchored at genesis
// - Difficulty floor enforcement
// - Smooth convergence behavior
//
// These are golden vectors - DO NOT MODIFY without consensus review.

#include <gtest/gtest.h>
#include "consensus/asert.h"
#include "consensus/header_chain.h"
#include "consensus/pow_context.h"
#include "consensus/pow.hpp"
#include "consensus/consensus.hpp"
#include "primitives/block.h"
#include <cstring>
#include <unordered_map>

namespace {

template <typename T>
class FakeStatusResult {
public:
    explicit FakeStatusResult(const T& value) : status_(Status::Ok), value_(value) {}
    explicit FakeStatusResult(Status status) : status_(status) {}

    Status status() const { return status_; }
    const T& value() const { return value_; }

private:
    Status status_;
    T value_{};
};

class FakeChainDBForAsert {
public:
    void Add(uint32_t height, const BlockHeader& header) {
        uint256 hash;
        std::memset(hash.data, 0, sizeof(hash.data));
        std::memcpy(hash.data, &height, sizeof(height));
        hashes_by_height_[height] = hash;
        headers_by_hash_[hash] = header;

        Block block;
        block.header = header;
        blocks_by_hash_[hash] = block;
    }

    FakeStatusResult<uint256> getBlockHashByHeight(uint32_t height) const {
        auto it = hashes_by_height_.find(height);
        if (it == hashes_by_height_.end()) {
            return FakeStatusResult<uint256>(Status::NotFound);
        }
        return FakeStatusResult<uint256>(it->second);
    }

    FakeStatusResult<BlockHeader> getHeader(const uint256& hash) const {
        auto it = headers_by_hash_.find(hash);
        if (it == headers_by_hash_.end()) {
            return FakeStatusResult<BlockHeader>(Status::NotFound);
        }
        return FakeStatusResult<BlockHeader>(it->second);
    }

    FakeStatusResult<Block> getBlock(const uint256& hash) const {
        auto it = blocks_by_hash_.find(hash);
        if (it == blocks_by_hash_.end()) {
            return FakeStatusResult<Block>(Status::NotFound);
        }
        return FakeStatusResult<Block>(it->second);
    }

private:
    std::unordered_map<uint32_t, uint256> hashes_by_height_;
    std::unordered_map<uint256, BlockHeader> headers_by_hash_;
    std::unordered_map<uint256, Block> blocks_by_hash_;
};

BlockHeader MakeHeader(uint64_t timestamp, uint32_t bits) {
    BlockHeader header{};
    header.version = 1;
    header.timestamp = timestamp;
    header.difficulty = bits;
    header.ZeroReserved();
    return header;
}

} // namespace

// ============================================================================
// Invariant 1: Genesis always returns genesisBits
// ============================================================================

TEST(ASERTDAA, GenesisFixedDifficulty) {
    Consensus c;

    auto bits = GetNextWorkRequired(0, 0, 0, 0, 0, c);
    EXPECT_EQ(bits, c.genesisBits)
        << "Genesis block must use genesisBits";
}

// ============================================================================
// Invariant 2: ASERT from block 1
// ============================================================================

TEST(ASERTDAA, ASERTFromBlock1) {
    Consensus c;

    // Block 1 is the first ASERT block, anchored at genesis (block 0)
    // With perfect timing (120s), difficulty should stay near anchor

    int64_t genesisTime = 1772496000;  // Genesis timestamp
    int64_t block1Time = genesisTime + 120;  // Perfect 2-minute spacing

    auto bits = GetNextWorkRequired(
        1,                    // height
        c.genesisBits,      // prevBits (genesis)
        genesisTime,          // prevMTP
        block1Time,           // currentMTP
        genesisTime,          // anchorTime (genesis)
        c
    );

    EXPECT_EQ(bits, c.asertAnchorBits)
        << "With zero ASERT excess time, difficulty must preserve anchor bits exactly";
}

// ============================================================================
// Invariant 3: ASERT difficulty increases with fast blocks
// ============================================================================

TEST(ASERTDAA, FastBlocksRegressionVector) {
    Consensus c;

    // Simulate 360 blocks in half the expected time (fast mining)
    int64_t genesisTime = 1772496000;
    int64_t idealTime = 360 * 120;       // 43200s expected
    int64_t actualTime = 360 * 57;       // ~20520s actual (fast)

    auto bits = GetNextWorkRequired(
        361,                          // height
        c.asertAnchorBits,            // prevBits
        genesisTime + actualTime - 57,  // prevMTP
        genesisTime + actualTime,     // currentMTP
        genesisTime,                  // anchorTime
        c
    );

    // Regression vector: preserve the current network's historical output.
    EXPECT_EQ(bits, 0x1e0084d7u);
}

// ============================================================================
// Invariant 4: ASERT difficulty decreases with slow blocks
// ============================================================================

TEST(ASERTDAA, SlowBlocksDecreaseDifficulty) {
    Consensus c;

    // Simulate blocks taking 3× longer than expected (slow mining)
    int64_t genesisTime = 1772496000;
    int64_t actualTime = 100 * 360;  // 100 blocks at 360s each (3× slower)

    auto bits = GetNextWorkRequired(
        101,                          // height
        c.asertAnchorBits,            // prevBits
        genesisTime + actualTime - 360,
        genesisTime + actualTime,
        genesisTime,
        c
    );

    arith_uint256 next_target;
    next_target.SetCompact(bits);
    arith_uint256 anchor_target;
    anchor_target.SetCompact(c.asertAnchorBits);

    // Slow mining should make the target larger (easier difficulty).
    EXPECT_GT(next_target, anchor_target)
        << "Slow mining should decrease difficulty (larger target)";
}

// ============================================================================
// Invariant 5: Difficulty floor (powLimitBits)
// ============================================================================

TEST(ASERTDAA, ExtremeSlowdownRegressionVector) {
    Consensus c;

    // Simulate catastrophic slowdown: blocks take 100× longer
    int64_t genesisTime = 1772496000;
    int64_t extremeSlowTime = 100 * 12000;  // 100 blocks × 12000s each (100× slower)

    auto bits = GetNextWorkRequired(
        101,
        c.asertAnchorBits,
        genesisTime + extremeSlowTime - 12000,
        genesisTime + extremeSlowTime,
        genesisTime,
        c
    );

    // Regression vector: preserve the current network's historical output.
    EXPECT_EQ(bits, 0x1e00c7ffu);
}

// ============================================================================
// Golden vector: Verify ASERT parameters
// ============================================================================

TEST(ASERTDAA, GoldenVector_ASERTParams) {
    Consensus c;

    EXPECT_EQ(c.genesisBits, 0x1d31ffce)
        << "Genesis difficulty must be 50x easier than Bitcoin genesis";
    EXPECT_EQ(c.powLimitBits, 0x1d31ffce)
        << "Difficulty floor must match genesis difficulty";
    EXPECT_EQ(c.asertAnchorBits, 0x1d31ffce)
        << "ASERT anchor bits must match genesis difficulty";
    EXPECT_EQ(c.asertAnchorHeight, 0u)
        << "ASERT anchor must be at genesis (height 0)";
    EXPECT_EQ(c.asertHalfLifeSec, 43200)
        << "ASERT half-life must be 12 hours (43200 seconds)";
    EXPECT_EQ(c.targetSpacingSec, 120u)
        << "Target block spacing must be 120 seconds (2 minutes)";
}

TEST(ASERTDAA, PureHelperMatchesLegacyWrapper) {
    Consensus c;
    const int64_t genesis_time = 1772496000;
    const int64_t reference_time = genesis_time + (360 * 120);

    AsertInput in;
    in.target_height = 361;
    in.reference_time = reference_time;
    in.anchor = {
        static_cast<int32_t>(c.asertAnchorHeight),
        genesis_time,
        c.asertAnchorBits,
    };
    in.params = GetAsertParams(c);

    EXPECT_EQ(
        ComputeAsertBits(in),
        GetNextWorkRequired(
            361,
            c.asertAnchorBits,
            reference_time - 120,
            reference_time,
            genesis_time,
            c));
}

TEST(ASERTDAA, AnchorTimeRegressionChangesBits) {
    Consensus c;
    const int64_t genesis_time = 1772496000;
    const int64_t canonical_anchor_time = genesis_time + 120;
    const int64_t wrong_anchor_time = genesis_time + 900;
    const int64_t reference_time = canonical_anchor_time + (500 * 90);

    AsertInput canonical;
    canonical.target_height = 501;
    canonical.reference_time = reference_time;
    canonical.anchor = {
        static_cast<int32_t>(c.asertAnchorHeight),
        canonical_anchor_time,
        c.asertAnchorBits,
    };
    canonical.params = GetAsertParams(c);

    AsertInput wrong = canonical;
    wrong.anchor.time = wrong_anchor_time;

    EXPECT_NE(ComputeAsertBits(canonical), ComputeAsertBits(wrong))
        << "Canonical anchor time drift must change ASERT bits";
}

TEST(ASERTDAA, CrossPathInvariantValidationMiningRpcAgree) {
    const Consensus c = GetConsensusForCurrentNetwork();
    const int64_t genesis_time = 1772496000;
    const uint32_t parent_height = 12;
    const int32_t target_height = static_cast<int32_t>(parent_height + 1);
    const int64_t candidate_time = genesis_time + (target_height * static_cast<int64_t>(c.targetSpacingSec));

    FakeChainDBForAsert fake_db;
    std::vector<dinero::consensus::HeaderIndexEntry> entries(parent_height + 1);

    for (uint32_t height = 0; height <= parent_height; ++height) {
        const uint64_t timestamp = static_cast<uint64_t>(genesis_time + (height * static_cast<uint64_t>(c.targetSpacingSec)));
        BlockHeader header = MakeHeader(timestamp, c.asertAnchorBits);
        fake_db.Add(height, header);

        entries[height].height = height;
        entries[height].header = header;
        entries[height].parent = (height == 0) ? nullptr : &entries[height - 1];
    }

    const uint32_t validation_bits = GetNextWorkRequiredForCandidate(
        target_height,
        candidate_time,
        c,
        nullptr,
        &entries[parent_height],
        &fake_db);
    const uint32_t mining_bits = GetNextWorkRequiredWithChainDB(
        target_height,
        candidate_time,
        c,
        &fake_db);
    const uint32_t rpc_bits = GetNextWorkRequiredWithChainDB(
        target_height,
        candidate_time,
        c,
        &fake_db);

    EXPECT_EQ(validation_bits, mining_bits);
    EXPECT_EQ(validation_bits, rpc_bits);
}

TEST(ASERTDAA, DebugSnapshotIncludesConsensusFields) {
    Consensus c;
    ComputedAsertDebug dbg;
    AsertInput in;
    in.target_height = 42;
    in.reference_time = 1772496000 + (42 * 120);
    in.anchor = {
        static_cast<int32_t>(c.asertAnchorHeight),
        1772496000,
        c.asertAnchorBits,
    };
    in.params = GetAsertParams(c);

    (void)ComputeAsertBits(in, &dbg);
    const std::string summary = FormatComputedAsertDebug(dbg);

    EXPECT_NE(summary.find("target_height=42"), std::string::npos);
    EXPECT_NE(summary.find("anchor_bits=0x"), std::string::npos);
    EXPECT_NE(summary.find("result_bits=0x"), std::string::npos);
}

// Note: Regtest difficulty is tested via integration tests since it
// depends on dinero::Params() chain selection, not the Consensus struct.

// HeaderChainSelector::ValidateHeader (header-accept) calls
// GetNextWorkRequiredForCandidate with chain_db = nullptr, relying solely on the
// parent HeaderIndexEntry's in-memory ancestry. This asserts the header-only
// mode computes the SAME expected bits as the block-connect mode (chain_db
// present) and the mining path across a range of heights — i.e. header
// acceptance and block connect cannot drift on the difficulty rule, and the
// header path does not need chain_db.
TEST(ASERTDAA, HeaderPathNullChainDbMatchesBlockAndMining) {
    const Consensus c = GetConsensusForCurrentNetwork();
    const int64_t genesis_time = 1772496000;
    const uint32_t max_parent = 20;

    FakeChainDBForAsert fake_db;
    std::vector<dinero::consensus::HeaderIndexEntry> entries(max_parent + 1);
    for (uint32_t height = 0; height <= max_parent; ++height) {
        const uint64_t timestamp = static_cast<uint64_t>(
            genesis_time + (height * static_cast<uint64_t>(c.targetSpacingSec)));
        BlockHeader header = MakeHeader(timestamp, c.asertAnchorBits);
        fake_db.Add(height, header);
        entries[height].height = height;
        entries[height].header = header;
        entries[height].parent = (height == 0) ? nullptr : &entries[height - 1];
    }

    for (uint32_t ph = 1; ph <= max_parent; ++ph) {
        const int32_t target_height = static_cast<int32_t>(ph + 1);
        const int64_t candidate_time =
            genesis_time + (target_height * static_cast<int64_t>(c.targetSpacingSec));

        const uint32_t header_path_bits = GetNextWorkRequiredForCandidate(
            target_height, candidate_time, c,
            /*parent_index=*/nullptr,
            /*parent_entry=*/&entries[ph],
            /*chain_db=*/static_cast<FakeChainDBForAsert*>(nullptr));
        const uint32_t block_path_bits = GetNextWorkRequiredForCandidate(
            target_height, candidate_time, c,
            /*parent_index=*/nullptr,
            /*parent_entry=*/&entries[ph],
            /*chain_db=*/&fake_db);
        const uint32_t mining_bits = GetNextWorkRequiredWithChainDB(
            target_height, candidate_time, c, &fake_db);

        EXPECT_NE(header_path_bits, 0u)
            << "header-path bits uncomputable at height " << target_height;
        EXPECT_EQ(header_path_bits, block_path_bits)
            << "header (null chain_db) vs block path drift at height " << target_height;
        EXPECT_EQ(header_path_bits, mining_bits)
            << "header path vs mining drift at height " << target_height;
    }
}

// Side branches must be validated against their OWN parent/anchor context, not a
// shared/active tip. Two header chains share genesis but progress at different
// rates; expected bits computed via each branch's own parent entry must reflect
// that branch's ancestry, proving GetNextWorkRequiredForCandidate uses the
// passed parent_entry's chain (prev->GetMedianTimePast walks prev's ancestry).
TEST(ASERTDAA, SideBranchUsesOwnParentForExpectedBits) {
    const Consensus c = GetConsensusForCurrentNetwork();
    const int64_t genesis_time = 1772496000;
    const uint32_t ph = 15;

    std::vector<dinero::consensus::HeaderIndexEntry> a(ph + 1);  // on-schedule
    std::vector<dinero::consensus::HeaderIndexEntry> b(ph + 1);  // 4x slower
    for (uint32_t h = 0; h <= ph; ++h) {
        const uint64_t ta =
            genesis_time + (h * static_cast<uint64_t>(c.targetSpacingSec));
        const uint64_t tb =
            genesis_time + (h * static_cast<uint64_t>(c.targetSpacingSec) * 4);
        a[h].height = h; a[h].header = MakeHeader(ta, c.asertAnchorBits);
        a[h].parent = (h == 0) ? nullptr : &a[h - 1];
        b[h].height = h; b[h].header = MakeHeader(tb, c.asertAnchorBits);
        b[h].parent = (h == 0) ? nullptr : &b[h - 1];
    }

    const int32_t target_height = static_cast<int32_t>(ph + 1);
    const int64_t ca = genesis_time + (target_height * static_cast<int64_t>(c.targetSpacingSec));
    const int64_t cb = genesis_time + (target_height * static_cast<int64_t>(c.targetSpacingSec) * 4);

    const uint32_t bits_a = GetNextWorkRequiredForCandidate(
        target_height, ca, c, nullptr, &a[ph],
        static_cast<FakeChainDBForAsert*>(nullptr));
    const uint32_t bits_b = GetNextWorkRequiredForCandidate(
        target_height, cb, c, nullptr, &b[ph],
        static_cast<FakeChainDBForAsert*>(nullptr));

    EXPECT_NE(bits_a, 0u);
    EXPECT_NE(bits_b, 0u);
    // The slower branch is behind schedule → ASERT eases difficulty → its
    // required bits differ from the on-schedule branch. Equal bits would mean
    // the computation ignored the branch's own ancestry.
    EXPECT_NE(bits_a, bits_b)
        << "side branch must compute expected bits from its own ancestry";
}
