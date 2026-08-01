#include "consensus/covenants.h"
#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;

void WriteLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> SerializeState(const ContractState& state) {
    std::vector<uint8_t> out;
    out.insert(out.end(), state.stateHash.begin(), state.stateHash.end());
    out.insert(out.end(), state.codeHash.begin(), state.codeHash.end());
    WriteLE32(out, state.counter);
    WriteLE32(out, static_cast<uint32_t>(state.data.size()));
    out.insert(out.end(), state.data.begin(), state.data.end());
    return out;
}

std::vector<uint8_t> ParseHex(const char* hex) {
    const auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        return static_cast<uint8_t>(c - 'a' + 10);
    };
    std::vector<uint8_t> out;
    while (hex[0] != '\0' && hex[1] != '\0') {
        out.push_back(static_cast<uint8_t>(
            (nibble(hex[0]) << 4) | nibble(hex[1])));
        hex += 2;
    }
    return out;
}

template <size_t N>
std::vector<uint8_t> ToVector(const std::array<uint8_t, N>& value) {
    return {value.begin(), value.end()};
}

class CcvSuccessorBindingTest : public ::testing::Test {
protected:
    static constexpr uint64_t kValue = 250'000;

    const std::vector<uint8_t> script_{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    ContractState previous_;
    ContractState next_;
    std::array<uint8_t, 32> merkle_root_{};
    std::array<uint8_t, 32> internal_key_{};
    uint8_t parity_{0};
    Transaction tx_;
    std::vector<UTXOEntry> inputs_;

    void SetUp() override {
        for (size_t index = 0; index < merkle_root_.size(); ++index) {
            merkle_root_[index] = static_cast<uint8_t>(index + 1);
        }
        previous_.codeHash = ComputeContractCodeHash(script_);
        previous_.counter = 41;
        previous_.data = {0x10, 0x20, 0x30};
        previous_.stateHash = ComputeContractStateHash(previous_);

        next_.codeHash = previous_.codeHash;
        next_.counter = 42;
        next_.data = {0x40, 0x50, 0x60};
        next_.stateHash = ComputeContractStateHash(next_);

        ASSERT_TRUE(DeriveContractInternalKey(previous_, internal_key_));
        std::vector<uint8_t> current_script;
        ASSERT_TRUE(ComputeContractOutputScript(
            previous_, merkle_root_, current_script, &parity_));
        inputs_.emplace_back(
            AmountUna::Una(kValue), current_script, 100, false);

        tx_.vin.emplace_back();
        std::vector<uint8_t> successor_script;
        ASSERT_TRUE(ComputeContractOutputScript(
            next_, merkle_root_, successor_script));
        tx_.vout.emplace_back(AmountUna::Una(kValue), successor_script);
    }

    bool Verify() const {
        const ContractSpendContext context{
            inputs_, script_, internal_key_, merkle_root_, parity_};
        return VerifyContractTransition(
            tx_, 0, previous_, next_, context);
    }
};

TEST_F(CcvSuccessorBindingTest, AcceptsCompleteTransparentTransition) {
    EXPECT_TRUE(Verify());
}

TEST_F(CcvSuccessorBindingTest, MatchesGoldenCommitments) {
    EXPECT_EQ(ToVector(previous_.codeHash), ParseHex(
        "bce3b94e7f9f1a041b490e366d98e384"
        "42cbcef077610b90c4e5a7b63a80c8f7"));
    EXPECT_EQ(ToVector(previous_.stateHash), ParseHex(
        "820f04fe93cdb43b27668a99fae6b47c"
        "1b1ca67258f59ecdeb2261d8615043ac"));
    EXPECT_EQ(ToVector(internal_key_), ParseHex(
        "1110c456999cb753d39d73a8f57e0b0f"
        "669760e9ddafc15f339c5ee05a4216ee"));
    EXPECT_EQ(inputs_[0].scriptPubKey, ParseHex(
        "51202c06cfbb5f2149007203323d7cc7"
        "9fa8cfed6cfab865fb2809bcefac7603b507"));
    EXPECT_EQ(tx_.vout[0].scriptPubKey, ParseHex(
        "5120a4bb46440cba36303dcbc734e5a6"
        "145340ae8448a057c770ff18bac86a3fa8dd"));
}

TEST_F(CcvSuccessorBindingTest, RequiresSuccessorAtMatchingIndex) {
    tx_.vout.insert(
        tx_.vout.begin(), TxOutput(AmountUna::Una(kValue), {OP_TRUE}));
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, PreservesExactTransparentValue) {
    tx_.vout[0].value = AmountUna::Una(kValue - 1);
    EXPECT_FALSE(Verify());
    tx_.vout[0].value = AmountUna::Una(kValue);
    inputs_[0].is_confidential = true;
    EXPECT_FALSE(Verify());
    inputs_[0].is_confidential = false;
    tx_.vout[0].is_confidential = true;
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, BindsStateCodeTreeAndParity) {
    previous_.stateHash[0] ^= 1;
    EXPECT_FALSE(Verify());
    previous_.stateHash[0] ^= 1;

    next_.codeHash[0] ^= 1;
    next_.stateHash = ComputeContractStateHash(next_);
    EXPECT_FALSE(Verify());
    next_.codeHash[0] ^= 1;
    next_.stateHash = ComputeContractStateHash(next_);

    merkle_root_[0] ^= 1;
    EXPECT_FALSE(Verify());
    merkle_root_[0] ^= 1;
    parity_ ^= 1;
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsCounterWrapAndOversizedState) {
    previous_.counter = std::numeric_limits<uint32_t>::max();
    previous_.stateHash = ComputeContractStateHash(previous_);
    next_.counter = 0;
    next_.stateHash = ComputeContractStateHash(next_);
    EXPECT_FALSE(Verify());

    next_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE + 1, 0x42);
    next_.stateHash = ComputeContractStateHash(next_);
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsDuplicateSuccessor) {
    tx_.vout.push_back(tx_.vout[0]);
    EXPECT_FALSE(Verify());
}

TEST(CcvSuccessorBindingIntegration, EnforcesThroughAuthenticatedScriptPath) {
    const std::vector<uint8_t> script{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    const std::vector<uint8_t> leaf_hash = TapLeafHash(0xc0, script);
    ASSERT_EQ(leaf_hash.size(), 32U);
    std::array<uint8_t, 32> merkle_root{};
    std::copy(leaf_hash.begin(), leaf_hash.end(), merkle_root.begin());

    ContractState previous;
    previous.codeHash = ComputeContractCodeHash(script);
    previous.counter = 7;
    previous.data = {0xaa};
    previous.stateHash = ComputeContractStateHash(previous);
    ContractState next;
    next.codeHash = previous.codeHash;
    next.counter = 8;
    next.data = {0xbb};
    next.stateHash = ComputeContractStateHash(next);

    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(DeriveContractInternalKey(previous, internal_key));
    uint8_t parity = 0;
    std::vector<uint8_t> current_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        previous, merkle_root, current_script, &parity));
    std::vector<uint8_t> successor_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        next, merkle_root, successor_script));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());

    Transaction tx;
    tx.vin.emplace_back();
    tx.vin[0].witness = {
        SerializeState(previous), SerializeState(next), script, control_block};
    tx.vout.emplace_back(AmountUna::Una(75'000), successor_script);
    const std::vector<UTXOEntry> inputs{
        UTXOEntry(AmountUna::Una(75'000), current_script, 100, false)};

    std::string error;
    SelectParams(Chain::REGTEST);
    const uint32_t activated_flags =
        CovenantActivationParams::StandardFlags(20, Params());
    EXPECT_NE(activated_flags & SCRIPT_VERIFY_CHECKCONTRACT, 0U);
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error, activated_flags))
        << error;

    tx.vout[0].scriptPubKey = {OP_TRUE};
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error, activated_flags));
    EXPECT_NE(error.find("state transition verification failed"),
              std::string::npos);
    SelectParams(Chain::MAINNET);
}

TEST(CcvSuccessorBindingIntegration, LimitsRedundantCcvExecutionPerScript) {
    const std::vector<uint8_t> script{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    const std::vector<uint8_t> leaf_hash = TapLeafHash(0xc0, script);
    std::array<uint8_t, 32> merkle_root{};
    std::copy(leaf_hash.begin(), leaf_hash.end(), merkle_root.begin());

    ContractState previous;
    previous.codeHash = ComputeContractCodeHash(script);
    previous.counter = 11;
    previous.data = {0x01};
    previous.stateHash = ComputeContractStateHash(previous);
    ContractState next;
    next.codeHash = previous.codeHash;
    next.counter = 12;
    next.data = {0x02};
    next.stateHash = ComputeContractStateHash(next);

    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(DeriveContractInternalKey(previous, internal_key));
    uint8_t parity = 0;
    std::vector<uint8_t> current_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        previous, merkle_root, current_script, &parity));
    std::vector<uint8_t> successor_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        next, merkle_root, successor_script));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());
    const auto previous_bytes = SerializeState(previous);
    const auto next_bytes = SerializeState(next);

    Transaction tx;
    tx.vin.emplace_back();
    tx.vin[0].witness = {
        previous_bytes, next_bytes,
        previous_bytes, next_bytes,
        script, control_block};
    tx.vout.emplace_back(AmountUna::Una(75'000), successor_script);
    const std::vector<UTXOEntry> inputs{
        UTXOEntry(
            AmountUna::Una(75'000), current_script, 100, false)};

    SelectParams(Chain::REGTEST);
    const uint32_t activated_flags =
        CovenantActivationParams::StandardFlags(20, Params());
    std::string error;
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error, activated_flags));
    EXPECT_NE(
        error.find("per-tapscript execution limit exceeded"),
        std::string::npos);
    SelectParams(Chain::MAINNET);
}

} // namespace
