#include <gtest/gtest.h>

#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/chainparams.h"
#include "consensus/script.h"
#include "consensus/script_cache.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "consensus/transaction_validator.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>

namespace dinero::consensus {
namespace {

std::array<uint8_t, 32> TaggedHash(
    const char* tag, const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> tag_hash{};
    crypto::CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag))
        .Finalize(tag_hash.data());

    std::array<uint8_t, 32> result{};
    crypto::CSHA256 hasher;
    hasher.Write(tag_hash.data(), tag_hash.size());
    hasher.Write(tag_hash.data(), tag_hash.size());
    hasher.Write(data.data(), data.size());
    hasher.Finalize(result.data());
    return result;
}

std::array<uint8_t, 32> ComputeTapleafRoot(
    const std::vector<uint8_t>& tapscript) {
    std::vector<uint8_t> preimage{
        0xc0, static_cast<uint8_t>(tapscript.size())};
    preimage.insert(preimage.end(), tapscript.begin(), tapscript.end());
    return TaggedHash("TapLeaf", preimage);
}

void WriteLE32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> SerializeContractState(const ContractState& state) {
    std::vector<uint8_t> bytes;
    bytes.reserve(72 + state.data.size());
    bytes.insert(bytes.end(), state.stateHash.begin(), state.stateHash.end());
    bytes.insert(bytes.end(), state.codeHash.begin(), state.codeHash.end());
    WriteLE32(bytes, state.counter);
    WriteLE32(bytes, static_cast<uint32_t>(state.data.size()));
    bytes.insert(bytes.end(), state.data.begin(), state.data.end());
    return bytes;
}

std::vector<uint8_t> ParseHex(const char* hex) {
    std::vector<uint8_t> bytes;
    while (hex[0] != '\0' && hex[1] != '\0') {
        const auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            return static_cast<uint8_t>(c - 'a' + 10);
        };
        bytes.push_back(static_cast<uint8_t>(
            (nibble(hex[0]) << 4) | nibble(hex[1])));
        hex += 2;
    }
    return bytes;
}

template <size_t N>
std::vector<uint8_t> ToVector(const std::array<uint8_t, N>& bytes) {
    return {bytes.begin(), bytes.end()};
}

class CcvSuccessorBindingTest : public ::testing::Test {
protected:
    static constexpr uint64_t kContractValue = 250'000;

    ContractState prev_;
    ContractState next_;
    Transaction tx_;
    std::vector<UTXOEntry> inputs_;
    std::vector<uint8_t> tapscript_{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    std::array<uint8_t, 32> merkle_root_{};
    std::array<uint8_t, 32> internal_key_{};
    uint8_t output_key_parity_{0};

    void SetUp() override {
        for (size_t i = 0; i < merkle_root_.size(); ++i) {
            merkle_root_[i] = static_cast<uint8_t>(i + 1);
        }

        prev_.codeHash = ComputeContractCodeHash(tapscript_);
        prev_.counter = 41;
        prev_.data = {0x10, 0x20, 0x30};
        prev_.stateHash = ComputeContractStateHash(prev_);

        next_.codeHash = prev_.codeHash;
        next_.counter = 42;
        next_.data = {0x40, 0x50, 0x60};
        next_.stateHash = ComputeContractStateHash(next_);

        ASSERT_TRUE(DeriveContractInternalKey(prev_, internal_key_));

        std::vector<uint8_t> current_script;
        ASSERT_TRUE(ComputeContractOutputScript(
            prev_, merkle_root_, current_script, &output_key_parity_));
        inputs_.emplace_back(
            AmountUna::Una(kContractValue), current_script, 100, false);

        tx_.version = Transaction::TX_VERSION_SEGWIT;
        tx_.lockTime = 0;
        tx_.vin.emplace_back();

        std::vector<uint8_t> successor_script;
        ASSERT_TRUE(ComputeContractOutputScript(
            next_, merkle_root_, successor_script));
        tx_.vout.emplace_back(
            AmountUna::Una(kContractValue), successor_script);
    }

    bool Verify(uint32_t input_index = 0) const {
        const ContractSpendContext context{
            inputs_, tapscript_, internal_key_, merkle_root_,
            output_key_parity_};
        return VerifyContractTransition(
            tx_, input_index, prev_, next_, context);
    }

    void RecomputePrev() {
        prev_.stateHash = ComputeContractStateHash(prev_);
    }

    void RecomputeNext() {
        next_.stateHash = ComputeContractStateHash(next_);
    }
};

TEST_F(CcvSuccessorBindingTest, AcceptsValidTransparentTransition) {
    EXPECT_TRUE(Verify());
}

TEST_F(CcvSuccessorBindingTest, MatchesV1GoldenVector) {
    EXPECT_EQ(ToVector(prev_.codeHash), ParseHex(
        "bce3b94e7f9f1a041b490e366d98e384"
        "42cbcef077610b90c4e5a7b63a80c8f7"));
    EXPECT_EQ(ToVector(prev_.stateHash), ParseHex(
        "820f04fe93cdb43b27668a99fae6b47c"
        "1b1ca67258f59ecdeb2261d8615043ac"));
    EXPECT_EQ(ToVector(internal_key_), ParseHex(
        "1110c456999cb753d39d73a8f57e0b0f"
        "669760e9ddafc15f339c5ee05a4216ee"));
    EXPECT_EQ(inputs_[0].scriptPubKey, ParseHex(
        "51202c06cfbb5f2149007203323d7cc7"
        "9fa8cfed6cfab865fb2809bcefac7603b507"));
    EXPECT_EQ(output_key_parity_, 0);
    EXPECT_EQ(ToVector(next_.stateHash), ParseHex(
        "4488f5efa957c58b78aeffa4d81e02cd"
        "22ecde7b31c1e47b279eb1d4c203c5e0"));
    EXPECT_EQ(tx_.vout[0].scriptPubKey, ParseHex(
        "5120a4bb46440cba36303dcbc734e5a6"
        "145340ae8448a057c770ff18bac86a3fa8dd"));
}

TEST_F(CcvSuccessorBindingTest, LegacyRuleDoesNotRequireSuccessor) {
    tx_.vout[0].scriptPubKey = {0x51};

    EXPECT_TRUE(VerifyContractTransition(tx_, 0, prev_, next_));
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RequiresSuccessorAtMatchingOutputIndex) {
    tx_.vout.insert(
        tx_.vout.begin(),
        TxOutput(AmountUna::Una(kContractValue), {0x51}));

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsMissingSuccessorOutput) {
    tx_.vout.clear();

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, PreservesExactTransparentValue) {
    tx_.vout[0].value = AmountUna::Una(kContractValue - 1);

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsConfidentialInputUntilProofIsDefined) {
    inputs_[0].is_confidential = true;
    inputs_[0].commitment.assign(33, 0x02);

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsConfidentialSuccessorUntilProofIsDefined) {
    tx_.vout[0].is_confidential = true;
    tx_.vout[0].commitment.assign(33, 0x03);

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsDuplicateSuccessorState) {
    tx_.vout.push_back(tx_.vout[0]);

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, BindsPreviousStateToSpentOutput) {
    prev_.data.push_back(0xff);
    RecomputePrev();

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, BindsCodeHashToRevealedTapscript) {
    tapscript_.push_back(0x00);

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsForgedPreviousStateHash) {
    prev_.stateHash[0] ^= 0x01;

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsForgedSuccessorStateHash) {
    next_.stateHash[0] ^= 0x01;

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsChangedContractCode) {
    next_.codeHash[0] ^= 0x01;
    RecomputeNext();

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RequiresExactlyOneCounterIncrement) {
    next_.counter = prev_.counter;
    RecomputeNext();
    EXPECT_FALSE(Verify());

    next_.counter = prev_.counter + 2;
    RecomputeNext();
    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, RejectsCounterWrap) {
    prev_.counter = std::numeric_limits<uint32_t>::max();
    RecomputePrev();
    next_.counter = 0;
    RecomputeNext();

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, EnforcesTapscriptStateElementLimit) {
    next_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE + 1, 0x42);
    RecomputeNext();

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, BindsImmutableTaprootTree) {
    merkle_root_[0] ^= 0x01;

    EXPECT_FALSE(Verify());
}

TEST_F(CcvSuccessorBindingTest, SeparatesIndependentInputsByOutputIndex) {
    inputs_.push_back(inputs_[0]);
    tx_.vin.emplace_back();
    tx_.vout.emplace_back(AmountUna::Una(kContractValue), std::vector<uint8_t>{0x51});

    EXPECT_FALSE(Verify(1));

    std::vector<uint8_t> successor_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        next_, merkle_root_, successor_script));
    tx_.vout[1].scriptPubKey = successor_script;
    tx_.vout[0].scriptPubKey = {0x51};
    EXPECT_TRUE(Verify(1));
}

TEST(CcvSuccessorBindingActivationTest, RegtestBoundaryAndDormantNetworks) {
    EXPECT_FALSE(CcvSuccessorBindingActivationParams::IsActive(
        19, Chain::REGTEST));
    EXPECT_TRUE(CcvSuccessorBindingActivationParams::IsActive(
        20, Chain::REGTEST));

    EXPECT_FALSE(CcvSuccessorBindingActivationParams::IsActive(
        0, Chain::MAINNET));
    EXPECT_FALSE(CcvSuccessorBindingActivationParams::IsActive(
        std::numeric_limits<uint32_t>::max(), Chain::MAINNET));
    EXPECT_FALSE(CcvSuccessorBindingActivationParams::IsActive(
        std::numeric_limits<uint32_t>::max(), Chain::TESTNET));
}

TEST(CcvSuccessorBindingActivationTest, CacheKeysSeparateActivatedRules) {
    uint256 txid;
    const std::vector<std::vector<uint8_t>> witness{{0x01}, {0x02}};
    const auto legacy = ScriptCache::computeKey(
        txid, 0, SCRIPT_VERIFY_STANDARD, witness);
    const auto activated = ScriptCache::computeKey(
        txid, 0,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING,
        witness);

    EXPECT_FALSE(legacy == activated);
}

TEST(CcvSuccessorBindingIntegrationTest, EnforcesRuleThroughTaprootInterpreter) {
    const std::vector<uint8_t> tapscript{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    const auto merkle_root = ComputeTapleafRoot(tapscript);

    ContractState prev;
    prev.codeHash = ComputeContractCodeHash(tapscript);
    prev.counter = 7;
    // This vector deliberately starts with the annex marker. The old parser
    // removed it from the stack and made this valid state unspendable.
    for (uint32_t nonce = 0; nonce <= 0xffff; ++nonce) {
        prev.data = {
            static_cast<uint8_t>(nonce),
            static_cast<uint8_t>(nonce >> 8)};
        prev.stateHash = ComputeContractStateHash(prev);
        if (prev.stateHash[0] == 0x50) {
            break;
        }
    }
    ASSERT_EQ(prev.stateHash[0], 0x50);

    ContractState next;
    next.codeHash = prev.codeHash;
    next.counter = 8;
    next.data = {0xbb};
    next.stateHash = ComputeContractStateHash(next);

    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(DeriveContractInternalKey(prev, internal_key));

    uint8_t output_key_parity = 0;
    std::vector<uint8_t> current_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        prev, merkle_root, current_script, &output_key_parity));

    std::vector<uint8_t> successor_script;
    ASSERT_TRUE(ComputeContractOutputScript(
        next, merkle_root, successor_script));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | output_key_parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());

    Transaction tx;
    tx.version = Transaction::TX_VERSION_SEGWIT;
    tx.witness_version = 1;
    tx.lockTime = 0;
    tx.vin.emplace_back();
    tx.vin[0].witness = {
        SerializeContractState(prev),
        SerializeContractState(next),
        tapscript,
        control_block
    };
    tx.vout.emplace_back(AmountUna::Una(75'000), successor_script);

    const std::vector<UTXOEntry> inputs{
        UTXOEntry(AmountUna::Una(75'000), current_script, 100, false)};
    std::string error;
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING))
        << error;

    tx.vin[0].witness.push_back({0x50, 0x99});
    error.clear();
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING))
        << "BIP341 trailing annex was not removed: " << error;
    tx.vin[0].witness.pop_back();

    tx.vin[0].witness.back()[0] ^= 0x01;
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING));
    tx.vin[0].witness.back()[0] ^= 0x01;

    tx.vout[0].scriptPubKey = {static_cast<uint8_t>(OP_TRUE)};
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        tx, 0, inputs, error,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING));
    EXPECT_NE(error.find("state transition verification failed"),
              std::string::npos);

    // A cached success under the historical flags must not bypass the rule at
    // the regtest activation boundary.
    SelectParams(Chain::REGTEST);
    ShutdownScriptCache();
    ASSERT_TRUE(InitializeScriptCache(1));
    const auto legacy_key = ScriptCache::computeKey(
        tx.GetTxid().AsUint256(), 0, 0, tx.vin[0].witness);
    g_script_cache->insert(legacy_key, true);

    const auto activated_result = TransactionValidator::VerifyInput(
        tx, 0, current_script, 75'000, 0, 20, 0,
        {75'000}, {current_script}, {0}, {{}});
    EXPECT_FALSE(activated_result.valid)
        << "pre-activation cache entry bypassed successor binding";

    ShutdownScriptCache();
    SelectParams(Chain::MAINNET);
}

} // namespace
} // namespace dinero::consensus
