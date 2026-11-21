// Bitcoin header chain for swaps without Bitcoin Core: real mainnet headers,
// Bitcoin Core's own retarget vectors, and mined regtest chains for the rules
// a hostile header source would try to break.

#include "wallet/swap/btc_header_chain.h"
#include "wallet/swap/btc_header_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace dinero::swap;

namespace {

constexpr uint32_t kNow = 1'900'000'000;

Bytes32 Hex(const std::string& hex) {
    const auto v = BtcHashFromHex(hex);
    if (!v) throw std::invalid_argument("bad test hash " + hex);
    return *v;
}

// The next header on `parent` that meets its own bits (regtest-level work).
BtcBlockHeader Mine(const BtcBlockHeader& parent, uint32_t time, uint32_t bits, uint32_t salt = 0) {
    BtcBlockHeader h;
    h.version = 0x20000000;
    h.prev_hash = BtcBlockHash(parent);
    h.merkle_root[0] = static_cast<uint8_t>(salt);
    h.merkle_root[1] = static_cast<uint8_t>(salt >> 8);
    h.merkle_root[2] = static_cast<uint8_t>(salt >> 16);
    h.time = time;
    h.bits = bits;
    for (h.nonce = 0; !BtcCheckProofOfWork(h, 0x207fffff); ++h.nonce) {
        if (h.nonce == UINT32_MAX) throw std::runtime_error("cannot mine");
    }
    return h;
}

// A header with valid structure whose hash does NOT meet its bits.
BtcBlockHeader MineBad(const BtcBlockHeader& parent, uint32_t time, uint32_t bits) {
    BtcBlockHeader h = Mine(parent, time, bits);
    while (BtcCheckProofOfWork(h, 0x207fffff)) ++h.nonce;
    return h;
}

// Extends `chain` by `count` valid headers, `spacing` seconds apart; returns the last.
BtcBlockHeader Extend(BtcHeaderChain& chain, uint32_t count, uint32_t spacing = 600) {
    BtcBlockHeader tip = chain.Tip();
    for (uint32_t i = 0; i < count; ++i) {
        const BtcBlockHeader next = Mine(tip, tip.time + spacing, tip.bits);
        const auto r = chain.Accept(next, kNow);
        if (r.status != BtcHeaderChain::Status::Accepted) throw std::runtime_error("extend: " + r.reason);
        tip = next;
    }
    return tip;
}

BtcBlockHeader MainnetBlock(const std::string& prev, const std::string& merkle, uint32_t time, uint32_t nonce) {
    BtcBlockHeader h;
    h.version = 1;
    h.prev_hash = Hex(prev);
    h.merkle_root = Hex(merkle);
    h.time = time;
    h.bits = 0x1d00ffff;
    h.nonce = nonce;
    return h;
}

const char* kGenesis = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
const char* kBlock1 = "00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048";
const char* kBlock2 = "000000006a625f06636b8bb6ac7b960a8d03705d1ace08b1a19da3fdcc99ddbd";

BtcBlockHeader Block1() {
    return MainnetBlock(kGenesis, "0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098", 1231469665, 2573394689U);
}
BtcBlockHeader Block2() {
    return MainnetBlock(kBlock1, "9b0fc92260312ce44e74ef369f5c66bbb85848f2eddd5a7a1cde251e54ccfdd5", 1231469744, 1639830024U);
}

}  // namespace

TEST(BtcHeaderChain, RealMainnetHeadersHashAndConnect) {
    BtcHeaderChain chain(BtcChainParams::MainnetFromGenesis());
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), kGenesis);
    EXPECT_EQ(BtcHashToHex(BtcBlockHash(Block1())), kBlock1);
    EXPECT_EQ(BtcHashToHex(BtcBlockHash(Block2())), kBlock2);

    EXPECT_EQ(chain.Accept(Block2(), kNow).status, BtcHeaderChain::Status::Orphan);  // out of order
    EXPECT_EQ(chain.Accept(Block1(), kNow).status, BtcHeaderChain::Status::Accepted);
    EXPECT_EQ(chain.Accept(Block2(), kNow).status, BtcHeaderChain::Status::Accepted);
    EXPECT_EQ(chain.Accept(Block2(), kNow).status, BtcHeaderChain::Status::Duplicate);
    EXPECT_EQ(chain.TipHeight(), 2u);
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), kBlock2);
    EXPECT_EQ(chain.Confirmations(Hex(kBlock1)), 2u);
    EXPECT_EQ(chain.Confirmations(Hex(kGenesis)), 3u);
    EXPECT_EQ(chain.HeightOf(Hex(kBlock2)), 2u);
    EXPECT_EQ(chain.AtHeight(1), Block1());
    EXPECT_FALSE(chain.AtHeight(3).has_value());

    // One changed bit anywhere and the real proof of work is gone.
    BtcBlockHeader forged = Block1();
    forged.merkle_root[0] ^= 1;
    BtcHeaderChain fresh(BtcChainParams::MainnetFromGenesis());
    const auto r = fresh.Accept(forged, kNow);
    EXPECT_EQ(r.status, BtcHeaderChain::Status::Invalid);
    EXPECT_EQ(r.reason, "proof of work");
}

TEST(BtcHeaderChain, RegtestGenesisIsBitcoinCores) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206");
}

TEST(BtcHeaderChain, HeaderBytesRoundTrip) {
    const auto raw = SerializeBtcBlockHeader(Block1());
    ASSERT_EQ(raw.size(), 80u);
    EXPECT_EQ(ParseBtcBlockHeader(raw), Block1());
    auto shorter = raw;
    shorter.pop_back();
    EXPECT_FALSE(ParseBtcBlockHeader(shorter).has_value());
    auto longer = raw;
    longer.push_back(0);
    EXPECT_FALSE(ParseBtcBlockHeader(longer).has_value());
    EXPECT_FALSE(BtcHashFromHex("00").has_value());
    EXPECT_FALSE(BtcHashFromHex(std::string(64, 'g')).has_value());
    EXPECT_EQ(BtcHashToHex(Hex(kBlock1)), kBlock1);
}

// Bitcoin Core src/test/pow_tests.cpp: get_next_work, _pow_limit, _lower_limit_actual, _upper_limit_actual.
TEST(BtcHeaderChain, RetargetMatchesBitcoinCoreVectors) {
    EXPECT_EQ(BtcNextWorkRequired(1261130161, 1262152739, 0x1d00ffff, 0x1d00ffff), 0x1d00d86aU);
    EXPECT_EQ(BtcNextWorkRequired(1231006505, 1233061996, 0x1d00ffff, 0x1d00ffff), 0x1d00ffffU);
    EXPECT_EQ(BtcNextWorkRequired(1279008237, 1279297671, 0x1c05a3f4, 0x1d00ffff), 0x1c0168fdU);
    EXPECT_EQ(BtcNextWorkRequired(1263163443, 1269211443, 0x1c387f6f, 0x1d00ffff), 0x1d00e1fdU);
    // Exact for any limit: the intermediate product is never allowed to wrap.
    EXPECT_EQ(BtcNextWorkRequired(1000, 1000 + kBtcRetargetTimespan, 0x207fffff, 0x207fffff), 0x207fffffU);      // on time
    EXPECT_EQ(BtcNextWorkRequired(1000, 1000 + kBtcRetargetTimespan / 4, 0x207fffff, 0x207fffff), 0x201fffffU);  // 4x fast
    EXPECT_EQ(BtcNextWorkRequired(1000, 1001, 0x207fffff, 0x207fffff), 0x201fffffU);                             // clamped
    EXPECT_EQ(BtcNextWorkRequired(5000, 1000, 0x207fffff, 0x207fffff), 0x201fffffU);                             // time ran backwards
    EXPECT_EQ(BtcNextWorkRequired(1000, 1000 + 8 * kBtcRetargetTimespan, 0x201fffff, 0x207fffff), 0x207ffffcU);  // 4x slow, clamped
    EXPECT_EQ(BtcNextWorkRequired(1000, 1000 + 8 * kBtcRetargetTimespan, 0x207fffff, 0x207fffff), 0x207fffffU);  // never above the limit
}

// Bitcoin Core pow_tests.cpp CheckProofOfWork_test_*: targets no hash may satisfy.
TEST(BtcHeaderChain, MalformedTargetsNeverPass) {
    BtcBlockHeader h = Block1();
    for (uint32_t bits : {0x00000000U,   // zero
                          0x1d000000U,   // zero mantissa
                          0x1d800001U,   // negative
                          0x01800000U,   // negative zero
                          0x23000001U,   // wider than 256 bits
                          0x2200ffffU,   // overflows
                          0x1d010000U,   // above the mainnet limit (easier than allowed)
                          0x207fffffU}) {  // regtest's limit on mainnet
        h.bits = bits;
        EXPECT_FALSE(BtcCheckProofOfWork(h, 0x1d00ffff)) << std::hex << bits;
    }
    EXPECT_TRUE(BtcCheckProofOfWork(Block1(), 0x1d00ffff));
    // A negative target below the limit in magnitude: refused for its sign alone.
    BtcBlockHeader easy = Mine(BtcBlockHeader{}, 1, 0x207fffff);
    ASSERT_TRUE(BtcCheckProofOfWork(easy, 0x207fffff));
    easy.bits = 0x20ffffff;  // the same magnitude with the sign bit set
    EXPECT_FALSE(BtcCheckProofOfWork(easy, 0x207fffff));
    easy.bits = 0x1f800001;
    EXPECT_FALSE(BtcCheckProofOfWork(easy, 0x207fffff));
}

TEST(BtcHeaderChain, RefusesBrokenRules) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const BtcBlockHeader genesis = chain.Tip();
    using S = BtcHeaderChain::Status;

    auto r = chain.Accept(MineBad(genesis, genesis.time + 600, genesis.bits), kNow);
    EXPECT_EQ(r.status, S::Invalid);
    EXPECT_EQ(r.reason, "proof of work");

    // Regtest never retargets: any other bits are refused, even harder ones.
    r = chain.Accept(Mine(genesis, genesis.time + 600, 0x207ffffe), kNow);
    EXPECT_EQ(r.status, S::Invalid);
    EXPECT_EQ(r.reason, "wrong difficulty bits");

    // Two hours ahead of our clock is the limit.
    EXPECT_EQ(chain.Accept(Mine(genesis, kNow + 7200, genesis.bits), kNow).status, S::Accepted);
    BtcHeaderChain other(BtcChainParams::Regtest());
    r = other.Accept(Mine(genesis, kNow + 7201, genesis.bits), kNow);
    EXPECT_EQ(r.status, S::Invalid);
    EXPECT_EQ(r.reason, "time more than two hours ahead");
    r = other.Accept(Mine(genesis, UINT32_MAX, genesis.bits), UINT32_MAX - 10);  // no wrap-around
    EXPECT_EQ(r.status, S::Accepted);

    EXPECT_EQ(chain.TipHeight(), 1u);
}

TEST(BtcHeaderChain, TimeMustPassTheMedianOfEleven) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const uint32_t t0 = chain.Tip().time;
    Extend(chain, 20, 600);  // heights 1..20 at t0 + 600 * height
    ASSERT_EQ(chain.TipHeight(), 20u);
    // The tip and its ten ancestors are heights 10..20: the median is height 15.
    ASSERT_TRUE(chain.MedianTimePast().has_value());
    EXPECT_EQ(*chain.MedianTimePast(), t0 + 600 * 15);

    const BtcBlockHeader tip = chain.Tip();
    auto r = chain.Accept(Mine(tip, t0 + 600 * 15, tip.bits), kNow);  // equal to the median
    EXPECT_EQ(r.status, BtcHeaderChain::Status::Invalid);
    EXPECT_EQ(r.reason, "time not above the median of the previous eleven");
    // One second above the median is enough, although it is below the parent's time.
    EXPECT_EQ(chain.Accept(Mine(tip, t0 + 600 * 15 + 1, tip.bits), kNow).status, BtcHeaderChain::Status::Accepted);
}

TEST(BtcHeaderChain, MostWorkWinsNotMostHeaders) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const BtcBlockHeader genesis = chain.Tip();
    const BtcBlockHeader a1 = Mine(genesis, genesis.time + 600, genesis.bits, 1);
    const BtcBlockHeader b1 = Mine(genesis, genesis.time + 601, genesis.bits, 2);
    const BtcBlockHeader b2 = Mine(b1, b1.time + 600, b1.bits, 2);
    using S = BtcHeaderChain::Status;
    ASSERT_EQ(chain.Accept(a1, kNow).status, S::Accepted);
    ASSERT_EQ(chain.Accept(b1, kNow).status, S::Accepted);
    EXPECT_EQ(chain.TipHash(), BtcBlockHash(a1));  // equal work: the first seen stays
    EXPECT_EQ(chain.Confirmations(BtcBlockHash(b1)), 0u);
    EXPECT_EQ(chain.side_header_count(), 1u);

    ASSERT_EQ(chain.Accept(b2, kNow).status, S::Accepted);
    EXPECT_EQ(chain.TipHash(), BtcBlockHash(b2));  // more work: reorganized
    EXPECT_EQ(chain.TipHeight(), 2u);
    EXPECT_EQ(chain.Confirmations(BtcBlockHash(b1)), 2u);
    EXPECT_EQ(chain.Confirmations(BtcBlockHash(a1)), 0u);  // a payment in a1 is no longer confirmed
    EXPECT_FALSE(chain.HeightOf(BtcBlockHash(a1)).has_value());
    EXPECT_EQ(chain.AtHeight(1), b1);
}

// On a retargeting network, fewer hard headers beat more easy ones.
TEST(BtcHeaderChain, RetargetIsEnforcedAndCountsAsWork) {
    // A network that retargets, easy enough to mine here.
    BtcChainParams params;
    params.name = "retargeting-test";
    params.pow_limit_bits = 0x1f7fffff;
    params.retargets = true;
    params.checkpoint_height = 0;
    params.checkpoint = Mine(BtcBlockHeader{}, 1'700'000'000, 0x1f7fffff);
    BtcHeaderChain chain(params);
    // 2015 headers one minute apart: the period took far less than two weeks,
    // so the next target is a quarter (the clamp).
    const BtcBlockHeader last = Extend(chain, kBtcRetargetInterval - 1, 60);
    ASSERT_EQ(chain.TipHeight(), kBtcRetargetInterval - 1);
    const uint32_t expected = BtcNextWorkRequired(params.checkpoint.time, last.time, last.bits, params.pow_limit_bits);
    EXPECT_EQ(expected, 0x1f1fffffU);

    using S = BtcHeaderChain::Status;
    auto r = chain.Accept(Mine(last, last.time + 60, last.bits), kNow);  // the old, easier target
    EXPECT_EQ(r.status, S::Invalid);
    EXPECT_EQ(r.reason, "wrong difficulty bits");
    const BtcBlockHeader hard = Mine(last, last.time + 60, expected);
    EXPECT_EQ(chain.Accept(hard, kNow).status, S::Accepted);
    EXPECT_EQ(chain.TipHeight(), kBtcRetargetInterval);
    // Inside the new period the bits stay.
    EXPECT_EQ(chain.Accept(Mine(hard, hard.time + 60, last.bits), kNow).status, S::Invalid);
    EXPECT_EQ(chain.Accept(Mine(hard, hard.time + 60, expected), kNow).status, S::Accepted);
}

// The period is measured from its FIRST block (height - 2016) to its last:
// 2015 intervals, Bitcoin's well-known off-by-one. Bitcoin Core's vector above
// names block 30240 as the start for the retarget at 32256.
TEST(BtcHeaderChain, RetargetPeriodStartsAtItsFirstBlock) {
    BtcChainParams params;
    params.name = "retargeting-test";
    params.pow_limit_bits = 0x1f7fffff;
    params.retargets = true;
    params.checkpoint_height = 0;
    params.checkpoint = Mine(BtcBlockHeader{}, 1'700'000'000, 0x1f7fffff);
    BtcHeaderChain chain(params);
    // Ten minutes apart, except the first block a day after the checkpoint: a
    // period of normal length, so no clamp hides which block it is measured from.
    const BtcBlockHeader first = Mine(params.checkpoint, params.checkpoint.time + 86400, 0x1f7fffff);
    ASSERT_EQ(chain.Accept(first, kNow).status, BtcHeaderChain::Status::Accepted);
    const BtcBlockHeader last = Extend(chain, kBtcRetargetInterval - 2, 600);
    ASSERT_EQ(chain.TipHeight(), kBtcRetargetInterval - 1);

    const uint32_t from_checkpoint = BtcNextWorkRequired(params.checkpoint.time, last.time, last.bits, params.pow_limit_bits);
    const uint32_t from_block_one = BtcNextWorkRequired(first.time, last.time, last.bits, params.pow_limit_bits);
    ASSERT_NE(from_checkpoint, from_block_one);
    ASSERT_NE(from_block_one, last.bits);

    const auto r = chain.Accept(Mine(last, last.time + 600, from_block_one), kNow);
    EXPECT_EQ(r.status, BtcHeaderChain::Status::Invalid);
    EXPECT_EQ(r.reason, "wrong difficulty bits");
    EXPECT_EQ(chain.Accept(Mine(last, last.time + 600, from_checkpoint), kNow).status, BtcHeaderChain::Status::Accepted);
}

// Two branches that retarget differently: the shorter one with harder headers
// stays the best chain until the longer one has really done more work.
TEST(BtcHeaderChain, FewerHarderHeadersBeatMoreEasyOnes) {
    BtcChainParams params;
    params.name = "retargeting-test";
    params.pow_limit_bits = 0x1f7fffff;
    params.retargets = true;
    params.checkpoint_height = 0;
    params.checkpoint = Mine(BtcBlockHeader{}, 1'700'000'000, 0x1f7fffff);
    BtcHeaderChain chain(params);
    using S = BtcHeaderChain::Status;

    // Branch "fast": one minute apart, so its next period is four times harder.
    Extend(chain, kBtcRetargetInterval - 1, 60);
    const BtcBlockHeader fast_last = chain.Tip();
    const BtcBlockHeader fast_hard = Mine(fast_last, fast_last.time + 60, 0x1f1fffff);
    ASSERT_EQ(chain.Accept(fast_hard, kNow).status, S::Accepted);
    ASSERT_EQ(chain.TipHeight(), kBtcRetargetInterval);

    // Branch "slow": forty minutes apart from the first block on, so it stays at the limit.
    BtcBlockHeader slow = params.checkpoint;
    for (uint32_t height = 1; height <= kBtcRetargetInterval + 3; ++height) {
        slow = Mine(slow, slow.time + 2400, 0x1f7fffff, 7);
        ASSERT_EQ(chain.Accept(slow, kNow).status, S::Accepted) << height;
    }
    // Slow is three headers longer. From the boundary on it has four easy
    // headers against fast's single one, which is a little over four times as hard.
    EXPECT_EQ(chain.TipHash(), BtcBlockHash(fast_hard));
    EXPECT_EQ(chain.TipHeight(), kBtcRetargetInterval);

    slow = Mine(slow, slow.time + 2400, 0x1f7fffff, 7);  // a fifth easy header: now it is more work
    ASSERT_EQ(chain.Accept(slow, kNow).status, S::Accepted);
    EXPECT_EQ(chain.TipHash(), BtcBlockHash(slow));
    EXPECT_EQ(chain.TipHeight(), kBtcRetargetInterval + 4);
    EXPECT_EQ(chain.Confirmations(BtcBlockHash(fast_hard)), 0u);
}

TEST(BtcHeaderChain, AVeryDeepForkIsRefusedAndStaleSideHeadersAreDropped) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const BtcBlockHeader genesis = chain.Tip();
    const BtcBlockHeader side = Mine(genesis, genesis.time + 1, genesis.bits, 9);
    Extend(chain, 1);
    ASSERT_EQ(chain.Accept(side, kNow).status, BtcHeaderChain::Status::Accepted);
    EXPECT_EQ(chain.side_header_count(), 1u);
    Extend(chain, kBtcMaxReorgDepth + 5);
    EXPECT_EQ(chain.side_header_count(), 0u);  // too old to matter: dropped

    const auto r = chain.Accept(Mine(genesis, genesis.time + 2, genesis.bits, 10), kNow);
    EXPECT_EQ(r.status, BtcHeaderChain::Status::Invalid);
    EXPECT_NE(r.reason.find("fork deeper than"), std::string::npos);
    // A fork inside the window is still a candidate.
    const auto recent = chain.AtHeight(chain.TipHeight() - 3);
    ASSERT_TRUE(recent.has_value());
    EXPECT_EQ(chain.Accept(Mine(*recent, recent->time + 1, recent->bits, 11), kNow).status,
              BtcHeaderChain::Status::Accepted);
}

TEST(BtcHeaderChain, CompetingHeadersAreBounded) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const BtcBlockHeader genesis = chain.Tip();
    Extend(chain, 1);
    for (uint32_t i = 0; i < kBtcMaxSideHeaders; ++i) {
        ASSERT_EQ(chain.Accept(Mine(genesis, genesis.time + 1, genesis.bits, 100 + i), kNow).status,
                  BtcHeaderChain::Status::Accepted) << i;
    }
    const auto r = chain.Accept(Mine(genesis, genesis.time + 1, genesis.bits, 100 + kBtcMaxSideHeaders), kNow);
    EXPECT_EQ(r.status, BtcHeaderChain::Status::Invalid);
    EXPECT_EQ(r.reason, "too many competing headers");
    Extend(chain, 3);  // the best chain itself always grows
    EXPECT_EQ(chain.TipHeight(), 4u);
}

// A checkpoint above genesis: the headers before it are unknown.
TEST(BtcHeaderChain, StartsFromALaterCheckpoint) {
    BtcHeaderChain full(BtcChainParams::Regtest());
    Extend(full, kBtcRetargetInterval, 600);
    BtcChainParams params = BtcChainParams::Regtest();
    params.checkpoint_height = kBtcRetargetInterval;
    params.checkpoint = full.Tip();
    BtcHeaderChain chain(params);
    EXPECT_EQ(chain.TipHeight(), kBtcRetargetInterval);
    EXPECT_FALSE(chain.MedianTimePast().has_value());  // fewer than eleven headers known
    EXPECT_FALSE(chain.AtHeight(kBtcRetargetInterval - 1).has_value());
    EXPECT_EQ(chain.Accept(*full.AtHeight(5), kNow).status, BtcHeaderChain::Status::Orphan);  // below the checkpoint

    // Just above the checkpoint the median cannot be computed, so a header a
    // little older than the checkpoint (valid on Bitcoin) is not refused.
    const BtcBlockHeader cp = chain.Tip();
    EXPECT_EQ(chain.Accept(Mine(cp, cp.time - 30, cp.bits), kNow).status, BtcHeaderChain::Status::Accepted);
    Extend(chain, 8, 600);
    EXPECT_FALSE(chain.MedianTimePast().has_value());  // the checkpoint and nine more: ten
    Extend(chain, 1, 600);
    EXPECT_TRUE(chain.MedianTimePast().has_value());   // eleven

    params.checkpoint_height = kBtcRetargetInterval + 1;  // not a boundary
    EXPECT_THROW(BtcHeaderChain{params}, std::invalid_argument);
    params.checkpoint_height = kBtcRetargetInterval;
    params.checkpoint = MineBad(cp, cp.time + 1, cp.bits);
    EXPECT_THROW(BtcHeaderChain{params}, std::invalid_argument);
}

TEST(BtcHeaderChain, LocatorWalksBackToTheCheckpoint) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    const Bytes32 genesis = chain.TipHash();
    EXPECT_EQ(chain.Locator(), std::vector<Bytes32>{genesis});
    Extend(chain, 500);
    const auto locator = chain.Locator();
    ASSERT_GE(locator.size(), 12u);
    EXPECT_LE(locator.size(), 24u);
    EXPECT_EQ(locator.front(), chain.TipHash());
    EXPECT_EQ(locator.back(), genesis);
    for (size_t i = 1; i < locator.size(); ++i) {
        EXPECT_LT(*chain.HeightOf(locator[i]), *chain.HeightOf(locator[i - 1]));
    }
    EXPECT_EQ(*chain.HeightOf(locator[9]), 491u);   // the ten newest, one by one
}

// What another copy of the chain learned is taken in through the same checks.
TEST(BtcHeaderChain, AdoptsWhatACopyLearned) {
    BtcHeaderChain ours(BtcChainParams::Regtest());
    Extend(ours, 10);
    BtcHeaderChain copy = ours;
    Extend(copy, 5);
    EXPECT_EQ(ours.Adopt(copy, kNow), 5u);
    EXPECT_EQ(ours.TipHash(), copy.TipHash());
    EXPECT_EQ(ours.Adopt(copy, kNow), 0u);  // nothing new the second time

    // The copy reorganized onto a longer branch: ours follows.
    BtcHeaderChain forked = ours;
    const BtcBlockHeader base = *forked.AtHeight(12);
    BtcBlockHeader tip = base;
    for (int i = 0; i < 5; ++i) {
        tip = Mine(tip, tip.time + 600, tip.bits, 77);
        ASSERT_EQ(forked.Accept(tip, kNow).status, BtcHeaderChain::Status::Accepted);
    }
    ASSERT_EQ(forked.TipHeight(), 17u);
    EXPECT_EQ(ours.Adopt(forked, kNow), 5u);
    EXPECT_EQ(ours.TipHash(), forked.TipHash());

    // A header our clock says is from the future is not adopted, nor anything after it.
    BtcHeaderChain ahead = ours;
    BtcBlockHeader next = Mine(ahead.Tip(), kNow + 3 * 3600, ahead.Tip().bits);
    ASSERT_EQ(ahead.Accept(next, kNow + 3 * 3600).status, BtcHeaderChain::Status::Accepted);
    for (int i = 0; i < 2; ++i) {
        next = Mine(next, next.time + 600, next.bits);
        ASSERT_EQ(ahead.Accept(next, kNow + 4 * 3600).status, BtcHeaderChain::Status::Accepted);
    }
    EXPECT_EQ(ours.Adopt(ahead, kNow), 0u);
    EXPECT_EQ(ours.TipHeight(), 17u);
    EXPECT_EQ(ours.Adopt(ahead, kNow + 4 * 3600), 3u);  // once the clock has caught up
}

TEST(BtcHeaderChain, SavedChainIsReplayedNotTrusted) {
    BtcHeaderChain chain(BtcChainParams::Regtest());
    Extend(chain, 50);
    const auto bytes = chain.SerializeBestChain();
    ASSERT_EQ(bytes.size(), 50u * 80u);

    BtcHeaderChain loaded(BtcChainParams::Regtest());
    EXPECT_EQ(loaded.LoadBestChain(bytes, kNow), 50u);
    EXPECT_EQ(loaded.TipHash(), chain.TipHash());
    EXPECT_EQ(loaded.SerializeBestChain(), bytes);

    // A flipped bit in header 21's link to its parent (index 20): everything
    // before it loads, nothing after.
    auto damaged = bytes;
    damaged[20 * 80 + 4] ^= 0x01;
    BtcHeaderChain partial(BtcChainParams::Regtest());
    EXPECT_EQ(partial.LoadBestChain(damaged, kNow), 20u);
    EXPECT_EQ(partial.TipHeight(), 20u);
    EXPECT_EQ(partial.TipHash(), BtcBlockHash(*chain.AtHeight(20)));

    auto truncated = bytes;
    truncated.resize(bytes.size() - 7);  // a torn last record is ignored
    BtcHeaderChain torn(BtcChainParams::Regtest());
    EXPECT_EQ(torn.LoadBestChain(truncated, kNow), 49u);
}

// ---- The real networks ---------------------------------------------------------

namespace {

std::vector<uint8_t> Fixture(const std::string& name) {
    std::ifstream in(std::string(DINERO_SWAP_FIXTURES) + "/" + name, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture " + name);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name)
        : path((std::filesystem::temp_directory_path() / ("swap_btc_headers_" + name)).string()) {
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".tmp");
    }
};

}  // namespace

// 2,029 real mainnet headers after the checkpoint, fetched from a Bitcoin peer
// and matched against two servers: they cross the retarget at block 973,728.
TEST(BtcHeaderChain, RealMainnetHeadersAcrossARealRetarget) {
    BtcHeaderChain chain(BtcChainParams::Mainnet());
    EXPECT_EQ(chain.TipHeight(), 971712u);
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), "00000000000000000000c185f0906690a08d62d6fe842682a888d5560217e467");

    const auto headers = Fixture("btc_headers_main_971713.bin");
    ASSERT_EQ(headers.size(), 2029u * 80u);
    EXPECT_EQ(chain.LoadBestChain(headers, kNow), 2029u);
    EXPECT_EQ(chain.TipHeight(), 973741u);
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), "00000000000000000000411986a550b3625ae4bb8376b7ff8c8da18e709c3bc7");
    // The network really changed its difficulty there, and the chain required exactly that.
    EXPECT_EQ(chain.AtHeight(973727)->bits, 0x17020093U);
    EXPECT_EQ(chain.AtHeight(973728)->bits, 0x1701e5acU);
    EXPECT_EQ(BtcNextWorkRequired(chain.AtHeight(971712)->time, chain.AtHeight(973727)->time, 0x17020093, 0x1d00ffff),
              0x1701e5acU);
    ASSERT_TRUE(chain.MedianTimePast().has_value());
    EXPECT_GT(*chain.MedianTimePast(), chain.AtHeight(973730)->time);
    EXPECT_LT(*chain.MedianTimePast(), chain.Tip().time);

    // The same headers with the clock set before they were mined: refused as from the future.
    BtcHeaderChain early(BtcChainParams::Mainnet());
    EXPECT_EQ(early.LoadBestChain(headers, 1'700'000'000), 0u);

    // A real header, with a real proof of work, at the wrong place: not accepted.
    BtcHeaderChain other(BtcChainParams::Mainnet());
    const auto far = ParseBtcBlockHeader({headers.begin() + 80 * 100, headers.begin() + 80 * 101});
    ASSERT_TRUE(far.has_value());
    EXPECT_EQ(other.Accept(*far, kNow).status, BtcHeaderChain::Status::Orphan);
}

TEST(BtcHeaderChain, RealTestnet4Headers) {
    BtcHeaderChain chain(BtcChainParams::Testnet4());
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), "000000000000000311e5891b569e988c5c35da1cf4e59d8a7ead8fc8d322e384");
    const auto headers = Fixture("btc_headers_testnet4_157249.bin");
    ASSERT_EQ(headers.size(), 2029u * 80u);
    EXPECT_EQ(chain.LoadBestChain(headers, kNow), 2029u);
    EXPECT_EQ(chain.TipHeight(), 159277u);
    EXPECT_EQ(BtcHashToHex(chain.TipHash()), "000000007ebf9ff8064ecb492ede2d36ba855463c0d3e093298d439353d43e12");
    // Mainnet's rules would refuse testnet's minimum-difficulty blocks: the networks are not interchangeable.
    BtcChainParams strict = BtcChainParams::Testnet4();
    strict.allows_min_difficulty = false;
    BtcHeaderChain mainnet_rules(strict);
    EXPECT_LT(mainnet_rules.LoadBestChain(headers, kNow), 2029u);
}

TEST(BtcHeaderStore, SurvivesRestartAndIgnoresWhatIsNotItsOwn) {
    TempFile file("store");
    const auto headers = Fixture("btc_headers_main_971713.bin");
    BtcHeaderChain chain(BtcChainParams::Mainnet());
    ASSERT_EQ(chain.LoadBestChain({headers.begin(), headers.begin() + 80 * 500}, kNow), 500u);
    SaveBtcHeaderChain(file.path, chain);
    EXPECT_FALSE(std::filesystem::exists(file.path + ".tmp"));

    BtcHeaderChain restarted(BtcChainParams::Mainnet());
    EXPECT_EQ(LoadBtcHeaderChain(file.path, restarted, kNow), 500u);
    EXPECT_EQ(restarted.TipHash(), chain.TipHash());

    // Saving again after more headers replaces the file.
    ASSERT_EQ(chain.LoadBestChain({headers.begin() + 80 * 500, headers.begin() + 80 * 600}, kNow), 100u);
    SaveBtcHeaderChain(file.path, chain);
    BtcHeaderChain again(BtcChainParams::Mainnet());
    EXPECT_EQ(LoadBtcHeaderChain(file.path, again, kNow), 600u);

    // Another network's chain does not read this file, and a missing file is not an error.
    BtcHeaderChain testnet(BtcChainParams::Testnet4());
    EXPECT_EQ(LoadBtcHeaderChain(file.path, testnet, kNow), 0u);
    EXPECT_EQ(testnet.TipHeight(), 157248u);
    BtcHeaderChain from_genesis(BtcChainParams::MainnetFromGenesis());  // same network, another checkpoint
    EXPECT_EQ(LoadBtcHeaderChain(file.path, from_genesis, kNow), 0u);
    BtcHeaderChain fresh(BtcChainParams::Mainnet());
    EXPECT_EQ(LoadBtcHeaderChain(file.path + ".absent", fresh, kNow), 0u);

    // Damage: what is before it is kept, the rest is dropped (and fetched again).
    std::string bytes;
    {
        std::ifstream in(file.path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t label = bytes.size() - 600 * 80;
    bytes[label + 80 * 300 + 10] ^= 0x40;  // inside header 301's link to its parent
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out << bytes;
    }
    BtcHeaderChain damaged(BtcChainParams::Mainnet());
    EXPECT_EQ(LoadBtcHeaderChain(file.path, damaged, kNow), 300u);
    bytes[3] ^= 1;  // the label itself
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out << bytes;
    }
    BtcHeaderChain mislabeled(BtcChainParams::Mainnet());
    EXPECT_EQ(LoadBtcHeaderChain(file.path, mislabeled, kNow), 0u);
}
