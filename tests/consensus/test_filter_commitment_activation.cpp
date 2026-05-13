/**
 * Filter commitment activation boundary invariants.
 *
 * These tests lock the DNRF validation rule itself:
 *   - missing commitment is tolerated before activation
 *   - missing commitment is rejected at/after activation
 *   - if a commitment is present at any height, it must match the computed filter hash
 */

#include <gtest/gtest.h>

#include "consensus/block_filter.h"
#include "consensus/filter_commitment.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "util/hex.h"

using namespace dinero;
using namespace dinero::consensus;

namespace {

Transaction CreateCoinbase() {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    TxInput input;
    input.prevout.txid = TxId(uint256());
    input.prevout.vout = 0xffffffff;
    input.scriptSig = {0x01, 0x01};
    input.sequence = 0xffffffff;
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(1'000'000'000ULL);
    output.scriptPubKey = {
        0x51, 0x20,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    coinbase.vout.push_back(output);

    return coinbase;
}

uint256 SampleHash(const char* hex) {
    return uint256S(hex);
}

}  // namespace

TEST(FilterCommitmentActivation, MissingCommitmentAcceptedBeforeActivation) {
    Transaction coinbase = CreateCoinbase();
    const uint256 filter_hash = SampleHash("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    std::string error;
    EXPECT_TRUE(ValidateFilterCommitment(
        coinbase,
        filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT - 1,
        error));
    EXPECT_TRUE(error.empty());
}

TEST(FilterCommitmentActivation, MissingCommitmentRejectedAtActivation) {
    Transaction coinbase = CreateCoinbase();
    const uint256 filter_hash = SampleHash("abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd");

    std::string error;
    EXPECT_FALSE(ValidateFilterCommitment(
        coinbase,
        filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT,
        error));
    EXPECT_NE(error.find("Missing DNRF filter commitment"), std::string::npos);
}

TEST(FilterCommitmentActivation, PresentCommitmentMustMatchEvenBeforeActivation) {
    Transaction coinbase = CreateCoinbase();
    const uint256 committed_hash = SampleHash("1111111111111111111111111111111111111111111111111111111111111111");
    const uint256 computed_hash = SampleHash("2222222222222222222222222222222222222222222222222222222222222222");

    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = BuildFilterCommitmentScript(committed_hash);
    coinbase.vout.push_back(commitment_output);

    std::string error;
    EXPECT_FALSE(ValidateFilterCommitment(
        coinbase,
        computed_hash,
        FilterCommitment::ACTIVATION_HEIGHT - 1,
        error));
    EXPECT_NE(error.find("Filter commitment mismatch"), std::string::npos);
}

TEST(FilterCommitmentActivation, ValidCommitmentAcceptedAtAndAfterActivation) {
    Transaction coinbase = CreateCoinbase();
    const uint256 filter_hash = SampleHash("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");

    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = BuildFilterCommitmentScript(filter_hash);
    coinbase.vout.push_back(commitment_output);

    std::string activation_error;
    EXPECT_TRUE(ValidateFilterCommitment(
        coinbase,
        filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT,
        activation_error));
    EXPECT_TRUE(activation_error.empty());

    std::string post_activation_error;
    EXPECT_TRUE(ValidateFilterCommitment(
        coinbase,
        filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT + 1,
        post_activation_error));
    EXPECT_TRUE(post_activation_error.empty());
}

TEST(FilterCommitmentActivation, PrevBlockHashKeyMatchesCommitmentButBlockHashKeyDoesNot) {
    Block block;
    block.header.prev_block_hash = SampleHash("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const uint256 block_hash = SampleHash("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100");

    Transaction coinbase = CreateCoinbase();
    std::vector<std::vector<uint8_t>> scripts = {coinbase.vout[0].scriptPubKey};
    const uint256 committed_filter_hash =
        GCSFilter::Build(scripts, block.header.prev_block_hash).GetHash();

    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = BuildFilterCommitmentScript(committed_filter_hash);
    coinbase.vout.push_back(commitment_output);
    block.vtx.push_back(coinbase);

    std::string correct_error;
    EXPECT_TRUE(ValidateFilterCommitment(
        block.vtx[0],
        committed_filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT,
        correct_error));
    EXPECT_TRUE(correct_error.empty());

    const uint256 wrong_filter_hash = GCSFilter::Build(scripts, block_hash).GetHash();
    EXPECT_NE(wrong_filter_hash, committed_filter_hash);

    std::string wrong_error;
    EXPECT_FALSE(ValidateFilterCommitment(
        block.vtx[0],
        wrong_filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT,
        wrong_error));
    EXPECT_NE(wrong_error.find("Filter commitment mismatch"), std::string::npos);
}

TEST(FilterCommitmentActivation, HistoricalOutputsOnlyCommitmentCanDifferFromFullFilter) {
    const uint256 prev_block_hash =
        SampleHash("0000000035c4cc36665dc5002574f3f8cc81df5c8282b530967854cb6f25d72f");

    Transaction coinbase = CreateCoinbase();
    coinbase.vout[0].scriptPubKey = util::HexToBytes(
        "5120c716999c20ea7e8d7e6d541a400b6518146e0d8c970505f49ca9155c5297a2e2");

    const std::vector<uint8_t> tx2_prev = util::HexToBytes(
        "51203ccf9e2ed8db78f7df0f8d2d8c8c470fcdf9014b9a243335688f13a258110cf6");
    const std::vector<uint8_t> tx2_out1 = util::HexToBytes(
        "512054b9b538ad302e81bb01d37545a24e7d6058f72f11a0415fd676e26508d1f447");
    const std::vector<uint8_t> tx3_out0 = util::HexToBytes(
        "51205439c5c69bd85954ffad1e0cdd2bd6fdb408929747b7c0c6ed2acab7004031da");

    const std::vector<std::vector<uint8_t>> outputs_only_scripts = {
        coinbase.vout[0].scriptPubKey,
        tx2_out1,
        tx3_out0,
    };
    const uint256 committed_filter_hash =
        GCSFilter::Build(outputs_only_scripts, prev_block_hash).GetHash();

    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = BuildFilterCommitmentScript(committed_filter_hash);
    coinbase.vout.push_back(commitment_output);

    std::string outputs_only_error;
    EXPECT_TRUE(ValidateFilterCommitment(
        coinbase,
        committed_filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT + 173,
        outputs_only_error));
    EXPECT_TRUE(outputs_only_error.empty());

    const std::vector<std::vector<uint8_t>> full_scripts = {
        coinbase.vout[0].scriptPubKey,
        tx2_prev,
        tx2_out1,
        tx3_out0,
    };
    const uint256 full_filter_hash =
        GCSFilter::Build(full_scripts, prev_block_hash).GetHash();
    EXPECT_NE(full_filter_hash, committed_filter_hash);

    std::string full_error;
    EXPECT_FALSE(ValidateFilterCommitment(
        coinbase,
        full_filter_hash,
        FilterCommitment::ACTIVATION_HEIGHT + 173,
        full_error));
    EXPECT_NE(full_error.find("Filter commitment mismatch"), std::string::npos);
}
