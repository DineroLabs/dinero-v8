#include "wallet/covenant_profile.h"

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/script.h"
#include "consensus/script_validation.h"
#include "consensus/script_verify.h"
#include "consensus/utxo_entry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::wallet::covenant;

class CovenantProfileWalletTest : public ::testing::Test {
protected:
    void SetUp() override {
        SelectParams(Chain::REGTEST);
    }

    void TearDown() override {
        SelectParams(Chain::MAINNET);
    }
};

TxOutPoint Outpoint(const char* txid, uint32_t vout) {
    return TxOutPoint{
        TxId(uint256::FromHexUnsafe(txid)),
        vout};
}

TEST_F(
    CovenantProfileWalletTest,
    CtvDescriptorRecoversExactTaprootArtifactAndConsensusSpend) {
    const std::vector<Output> outputs{
        Output{
            AmountUna::Una(99'000),
            {static_cast<uint8_t>(OP_TRUE)}}};
    const CTVPlan plan =
        BuildCTVPlan({0xfffffffeU}, 0, outputs);
    const CTVPlan recovered =
        RecoverCTVPlan(plan.recoveryDescriptor);

    EXPECT_EQ(DescriptorType(plan.recoveryDescriptor), ProfileType::CTV);
    EXPECT_EQ(recovered.descriptorId, plan.descriptorId);
    EXPECT_EQ(recovered.templateHash, plan.templateHash);
    EXPECT_EQ(recovered.taproot.tapscript, plan.taproot.tapscript);
    EXPECT_EQ(recovered.taproot.controlBlock, plan.taproot.controlBlock);
    EXPECT_EQ(recovered.taproot.scriptPubKey, plan.taproot.scriptPubKey);
    EXPECT_EQ(plan.taproot.controlBlock.size(), 33U);
    ASSERT_EQ(plan.taproot.tapscript.size(), 34U);
    EXPECT_EQ(
        plan.taproot.tapscript.back(),
        static_cast<uint8_t>(OP_CHECKTEMPLATEVERIFY));

    const auto fundingOutpoint = Outpoint(
        "000000000000000000000000000000000000000000000000000000000000c701",
        1);
    const Transaction spend =
        BuildCTVSpend(plan, {fundingOutpoint});
    const std::vector<UTXOEntry> inputs{
        UTXOEntry(
            AmountUna::Una(100'000),
            plan.taproot.scriptPubKey,
            1,
            false)};
    const uint32_t flags =
        CovenantActivationParams::StandardFlags(20, Params());
    std::string error;
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        spend, 0, inputs, error, flags))
        << error;
    EXPECT_EQ(
        ValidateSpend(spend, 0, inputs[0], 20, inputs),
        ScriptValidationResult::OK)
        << "the wallet artifact must pass the production mempool/block "
           "validation entry point, not only the Taproot verifier";
    const auto wire = spend.Serialize(TxSerializationMode::WithWitness);
    Transaction wire_roundtrip;
    size_t consumed = 0;
    ASSERT_TRUE(TransactionSerializer::Deserialize(
        wire_roundtrip, wire, consumed));
    ASSERT_EQ(consumed, wire.size());
    ASSERT_EQ(wire_roundtrip.vin.size(), spend.vin.size());
    EXPECT_EQ(wire_roundtrip.vin[0].witness, spend.vin[0].witness);
    EXPECT_EQ(
        wire_roundtrip.Serialize(TxSerializationMode::WithWitness), wire);
    error.clear();
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        wire_roundtrip, 0, inputs, error, flags))
        << error;
    EXPECT_EQ(
        ValidateSpend(wire_roundtrip, 0, inputs[0], 20, inputs),
        ScriptValidationResult::OK)
        << "RPC wire serialization must preserve a valid covenant witness";

    Transaction mutated = spend;
    mutated.vout[0].value = AmountUna::Una(98'999);
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        mutated, 0, inputs, error, flags));
}

TEST_F(
    CovenantProfileWalletTest,
    CtvMultiInputPlanLeavesFeeWitnessForNormalWalletSigner) {
    const CTVPlan plan = BuildCTVPlan(
        {0xfffffffdU, 0xfffffffcU},
        0,
        {
            Output{AmountUna::Una(50'000), {OP_TRUE}},
            Output{AmountUna::Una(40'000), {OP_TRUE}},
        },
        7);
    Transaction spend = BuildCTVSpend(
        plan,
        {
            Outpoint(
                "000000000000000000000000000000000000000000000000000000000000c702",
                0),
            Outpoint(
                "000000000000000000000000000000000000000000000000000000000000c703",
                2),
        });
    ASSERT_EQ(spend.vin.size(), 2U);
    EXPECT_EQ(spend.vin[0].witness.size(), 2U);
    EXPECT_TRUE(spend.vin[1].witness.empty());
    EXPECT_EQ(spend.vin[0].sequence, 0xfffffffdU);
    EXPECT_EQ(spend.vin[1].sequence, 0xfffffffcU);

    spend.vin[1].witness = {{0x01, 0x02, 0x03}};
    std::array<uint8_t, 32> hash{};
    ASSERT_TRUE(TryComputeCTVHash(spend, 0, hash));
    EXPECT_EQ(hash, plan.templateHash)
        << "BIP119 does not commit to fee-input witness data";
}

TEST_F(
    CovenantProfileWalletTest,
    DescriptorChecksumAndCanonicalTemplateFailClosed) {
    const CTVPlan plan = BuildCTVPlan(
        {0xfffffffeU},
        0,
        {Output{AmountUna::Una(10'000), {OP_TRUE}}});
    std::string corrupt = plan.recoveryDescriptor;
    ASSERT_GT(corrupt.size(), 8U);
    corrupt.back() = corrupt.back() == '0' ? '1' : '0';
    EXPECT_THROW(RecoverCTVPlan(corrupt), std::invalid_argument);

    std::string nonCanonical = plan.recoveryDescriptor;
    bool changedCase = false;
    for (size_t i = std::string("dncov1:").size();
         i < nonCanonical.size(); ++i) {
        if (nonCanonical[i] >= 'a' && nonCanonical[i] <= 'f') {
            nonCanonical[i] = static_cast<char>(
                nonCanonical[i] - 'a' + 'A');
            changedCase = true;
        }
    }
    ASSERT_TRUE(changedCase);
    EXPECT_THROW(RecoverCTVPlan(nonCanonical), std::invalid_argument)
        << "one descriptor payload must have exactly one textual id";

    EXPECT_THROW(RecoverCCVPlan(plan.recoveryDescriptor), std::invalid_argument);
    EXPECT_THROW(
        BuildCTVPlan(
            {0xfffffffeU},
            0,
            {Output{AmountUna::Una(10'000), {OP_TRUE}}},
            0,
            3),
        std::invalid_argument)
        << "excised/unknown transaction versions must not produce an "
           "unbroadcastable recovery descriptor";
}

TEST_F(
    CovenantProfileWalletTest,
    CcvDescriptorAdvancesAndRecoversConsensusAcceptedSuccessor) {
    const CCVPlan current = BuildCCVPlan(41, {0x10, 0x20, 0x30});
    const CCVPlan recovered =
        RecoverCCVPlan(current.recoveryDescriptor);

    EXPECT_EQ(DescriptorType(current.recoveryDescriptor), ProfileType::CCV);
    EXPECT_EQ(recovered.state.stateHash, current.state.stateHash);
    EXPECT_EQ(recovered.taproot.scriptPubKey, current.taproot.scriptPubKey);
    EXPECT_EQ(
        current.taproot.tapscript,
        (std::vector<uint8_t>{
            static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
            static_cast<uint8_t>(OP_TRUE)}));

    const std::vector<Input> spendInputs{
        Input{
            Outpoint(
                "000000000000000000000000000000000000000000000000000000000000cc01",
                0),
            0xfffffffeU},
        Input{
            Outpoint(
                "000000000000000000000000000000000000000000000000000000000000cc02",
                1),
            0xfffffffdU},
    };
    CCVTransition transition = BuildCCVTransition(
        current,
        spendInputs,
        AmountUna::Una(250'000),
        {0x40, 0x50, 0x60},
        {Output{AmountUna::Una(90'000), {OP_TRUE}}});

    ASSERT_EQ(transition.tx.vin.size(), 2U);
    ASSERT_EQ(transition.tx.vout.size(), 2U);
    EXPECT_EQ(transition.successor.state.counter, 42U);
    EXPECT_EQ(
        transition.tx.vout[0].scriptPubKey,
        transition.successor.taproot.scriptPubKey);
    EXPECT_EQ(
        RecoverCCVPlan(
            transition.successor.recoveryDescriptor).state.stateHash,
        transition.successor.state.stateHash);
    EXPECT_EQ(transition.tx.vin[0].witness.size(), 4U);
    EXPECT_TRUE(transition.tx.vin[1].witness.empty());

    const std::vector<UTXOEntry> inputs{
        UTXOEntry(
            AmountUna::Una(250'000),
            current.taproot.scriptPubKey,
            1,
            false),
        UTXOEntry(
            AmountUna::Una(100'000),
            {OP_TRUE},
            1,
            false),
    };
    const uint32_t flags =
        CovenantActivationParams::StandardFlags(20, Params());
    std::string error;
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        transition.tx, 0, inputs, error, flags))
        << error;
    EXPECT_EQ(
        ValidateSpend(transition.tx, 0, inputs[0], 20, inputs),
        ScriptValidationResult::OK)
        << "the CCV artifact must pass the canonical validation entry point";
    const auto wire =
        transition.tx.Serialize(TxSerializationMode::WithWitness);
    Transaction wire_roundtrip;
    size_t consumed = 0;
    ASSERT_TRUE(TransactionSerializer::Deserialize(
        wire_roundtrip, wire, consumed));
    ASSERT_EQ(consumed, wire.size());
    ASSERT_EQ(wire_roundtrip.vin.size(), transition.tx.vin.size());
    EXPECT_EQ(
        wire_roundtrip.vin[0].witness,
        transition.tx.vin[0].witness);
    EXPECT_EQ(
        ValidateSpend(wire_roundtrip, 0, inputs[0], 20, inputs),
        ScriptValidationResult::OK)
        << "CCV RPC wire serialization must preserve its script-path witness";

    transition.tx.vout[0].value = AmountUna::Una(249'999);
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        transition.tx, 0, inputs, error, flags));
}

TEST_F(
    CovenantProfileWalletTest,
    CcvRejectsCounterWrapOversizedStateAndDescriptorTampering) {
    const CCVPlan terminal =
        BuildCCVPlan(std::numeric_limits<uint32_t>::max(), {});
    EXPECT_THROW(
        BuildCCVTransition(
            terminal,
            {Input{
                Outpoint(
                    "000000000000000000000000000000000000000000000000000000000000cc03",
                    0),
                0xfffffffeU}},
            AmountUna::Una(1),
            {}),
        std::invalid_argument);

    EXPECT_THROW(
        BuildCCVPlan(
            0,
            std::vector<uint8_t>(
                MAX_CONTRACT_STATE_DATA_SIZE + 1, 0x42)),
        std::invalid_argument);

    std::string corrupt = terminal.recoveryDescriptor;
    corrupt[corrupt.size() - 3] =
        corrupt[corrupt.size() - 3] == 'a' ? 'b' : 'a';
    EXPECT_THROW(RecoverCCVPlan(corrupt), std::invalid_argument);
    EXPECT_THROW(
        BuildCCVTransition(
            BuildCCVPlan(0, {}),
            {Input{
                Outpoint(
                    "000000000000000000000000000000000000000000000000000000000000cc04",
                    0),
                0xfffffffeU}},
            AmountUna::Una(1),
            {},
            {},
            0,
            Transaction::TX_VERSION_SHIELDED),
        std::invalid_argument);
}

TEST_F(
    CovenantProfileWalletTest,
    BuildersRejectInvalidMoneyRangesAndDuplicatePrevouts) {
    EXPECT_THROW(
        BuildCTVPlan(
            {0xfffffffeU}, 0,
            {Output{AmountUna::Zero(), {OP_TRUE}}}),
        std::invalid_argument);
    EXPECT_THROW(
        BuildCTVPlan(
            {0xfffffffeU}, 0,
            {Output{
                AmountUna::Una(AmountUna::Max().GetUna() + 1),
                {OP_TRUE}}}),
        std::invalid_argument);
    EXPECT_THROW(
        BuildCTVPlan(
            {0xfffffffeU}, 0,
            {
                Output{AmountUna::Max(), {OP_TRUE}},
                Output{AmountUna::Una(1), {OP_TRUE}},
            }),
        std::invalid_argument);

    const CTVPlan ctv = BuildCTVPlan(
        {0xfffffffeU, 0xfffffffdU},
        0,
        {Output{AmountUna::Una(1), {OP_TRUE}}});
    const TxOutPoint duplicate = Outpoint(
        "000000000000000000000000000000000000000000000000000000000000c704",
        0);
    EXPECT_THROW(
        BuildCTVSpend(ctv, {duplicate, duplicate}),
        std::invalid_argument);

    const CCVPlan ccv = BuildCCVPlan(0, {});
    EXPECT_THROW(
        BuildCCVTransition(
            ccv,
            {Input{duplicate, 0xfffffffeU}},
            AmountUna::Zero(),
            {}),
        std::invalid_argument);
    EXPECT_THROW(
        BuildCCVTransition(
            ccv,
            {
                Input{duplicate, 0xfffffffeU},
                Input{duplicate, 0xfffffffdU},
            },
            AmountUna::Una(1),
            {}),
        std::invalid_argument);
    EXPECT_THROW(
        BuildCCVTransition(
            ccv,
            {Input{duplicate, 0xfffffffeU}},
            AmountUna::Max(),
            {},
            {Output{AmountUna::Una(1), {OP_TRUE}}}),
        std::invalid_argument);
}

} // namespace
