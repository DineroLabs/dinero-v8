#include <gtest/gtest.h>

#include "consensus/covenants.h"
#include "consensus/limits.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "wallet/ccv_wallet_v1.h"

#include <algorithm>

namespace dinero::wallet::ccv {
namespace {

constexpr auto LOCKED_VALUE = AmountUna::Una(250'000);

std::vector<uint8_t> ContractScript() {
  return {
      static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
      static_cast<uint8_t>(consensus::OP_TRUE),
  };
}

TEST(CcvWalletV1, BuildsStateDerivedScriptPathOnlyOutput) {
  ContractOutput output;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 7, {0x10, 0x20}, {}, output, error))
      << error;

  EXPECT_EQ(output.state.codeHash,
            consensus::ComputeContractCodeHash(output.tapscript));
  EXPECT_EQ(output.state.stateHash,
            consensus::ComputeContractStateHash(output.state));
  EXPECT_EQ(output.scriptPubKey.size(), 34u);
  EXPECT_EQ(output.scriptPubKey[0], 0x51);
  EXPECT_EQ(output.scriptPubKey[1], 0x20);
  ASSERT_EQ(output.controlBlock.size(), 33u);
  EXPECT_EQ(output.controlBlock[0] & 0xfe, TAPSCRIPT_LEAF_VERSION);
  EXPECT_TRUE(std::equal(output.internalKey.begin(), output.internalKey.end(),
                         output.controlBlock.begin() + 1));
}

TEST(CcvWalletV1, SupportsRecoveryLeafMerklePaths) {
  std::array<uint8_t, 32> recoveryLeaf{};
  recoveryLeaf[0] = 0x42;

  ContractOutput output;
  std::string error;
  ASSERT_TRUE(BuildContractOutput(ContractScript(), 0, {}, {recoveryLeaf},
                                  output, error))
      << error;

  EXPECT_EQ(output.controlBlock.size(), 65u);
  EXPECT_TRUE(std::equal(recoveryLeaf.begin(), recoveryLeaf.end(),
                         output.controlBlock.begin() + 33));
}

TEST(CcvWalletV1, RejectsCcvByteHiddenInsidePushdata) {
  ContractOutput output;
  std::string error;
  EXPECT_FALSE(BuildContractOutput(
      {0x01, static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY)}, 0, {},
      {}, output, error));
  EXPECT_NE(error.find("opcode position"), std::string::npos);
}

TEST(CcvWalletV1, RejectsMalformedTailAndOversizeInputs) {
  ContractOutput output;
  std::string error;
  EXPECT_FALSE(BuildContractOutput(
      {static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
       static_cast<uint8_t>(consensus::OP_PUSHDATA1)},
      0, {}, {}, output, error));
  EXPECT_NE(error.find("malformed"), std::string::npos);

  std::vector<uint8_t> oversizeScript{
      static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY)};
  oversizeScript.resize(consensus::MAX_SCRIPT_SIZE + 1,
                        static_cast<uint8_t>(consensus::OP_TRUE));
  EXPECT_FALSE(BuildContractOutput(oversizeScript, 0, {}, {}, output, error));
  EXPECT_NE(error.find("script-size"), std::string::npos);

  EXPECT_FALSE(BuildContractOutput(
      ContractScript(), 0,
      std::vector<uint8_t>(consensus::MAX_CONTRACT_STATE_DATA_SIZE + 1, 0), {},
      output, error));

  std::vector<std::array<uint8_t, 32>> path(MAX_TAPROOT_MERKLE_PATH_NODES + 1);
  EXPECT_FALSE(
      BuildContractOutput(ContractScript(), 0, {}, path, output, error));
}

TEST(CcvWalletV1, PopulatesCanonicalSuccessorAndWitness) {
  ContractOutput previous;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 11, {0xaa}, {}, previous, error))
      << error;

  Transaction tx;
  tx.version = Transaction::TX_VERSION_SEGWIT;
  tx.vin.emplace_back();
  tx.vout.emplace_back();

  Transition transition;
  const std::vector<std::vector<uint8_t>> prefix{{0x99}};
  ASSERT_TRUE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {0xbb}, prefix,
                                 transition, error))
      << error;

  EXPECT_EQ(transition.successor.state.counter, 12u);
  EXPECT_EQ(tx.vout[0].value, LOCKED_VALUE);
  EXPECT_EQ(tx.vout[0].scriptPubKey, transition.successor.scriptPubKey);
  ASSERT_EQ(tx.vin[0].witness.size(), 5u);
  EXPECT_EQ(tx.vin[0].witness[0], prefix[0]);
  EXPECT_EQ(tx.vin[0].witness[1], transition.previousStateWitness);
  EXPECT_EQ(tx.vin[0].witness[2], transition.successorStateWitness);
  EXPECT_EQ(tx.vin[0].witness[3], previous.tapscript);
  EXPECT_EQ(tx.vin[0].witness[4], previous.controlBlock);
}

TEST(CcvWalletV1, ConstructedTransitionPassesActivatedConsensus) {
  ContractOutput previous;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 20, {0x01}, {}, previous, error))
      << error;

  Transaction tx;
  tx.version = Transaction::TX_VERSION_SEGWIT;
  tx.vin.emplace_back();
  tx.vout.emplace_back();

  Transition transition;
  ASSERT_TRUE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {0x02}, {},
                                 transition, error))
      << error;

  const std::vector<consensus::UTXOEntry> inputs{
      consensus::UTXOEntry(LOCKED_VALUE, previous.scriptPubKey, 100, false)};
  EXPECT_TRUE(consensus::ScriptVerifier::VerifyTaproot(
      tx, 0, inputs, error,
      consensus::SCRIPT_VERIFY_STANDARD |
          consensus::SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING))
      << error;
}

TEST(CcvWalletV1, RefusesOverwriteWrongIndexAndCounterWrap) {
  ContractOutput previous;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 1, {}, {}, previous, error));

  Transaction tx;
  tx.vin.emplace_back();
  tx.vout.emplace_back();
  tx.vin[0].witness = {{0x01}};
  Transition transition;
  EXPECT_FALSE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {}, {},
                                  transition, error));

  tx.vin[0].witness.clear();
  EXPECT_FALSE(PopulateTransition(tx, 1, LOCKED_VALUE, previous, {}, {},
                                  transition, error));

  ASSERT_TRUE(BuildContractOutput(ContractScript(), UINT32_MAX, {}, {},
                                  previous, error));
  EXPECT_FALSE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {}, {},
                                  transition, error));
}

TEST(CcvWalletV1, RejectsInconsistentPreviousDescriptor) {
  ContractOutput previous;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 2, {0x11}, {}, previous, error));

  Transaction tx;
  tx.vin.emplace_back();
  tx.vout.emplace_back();
  Transition transition;

  previous.state.stateHash[0] ^= 0x01;
  EXPECT_FALSE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {0x22}, {},
                                  transition, error));
  EXPECT_NE(error.find("internally inconsistent"), std::string::npos);
  EXPECT_TRUE(tx.vin[0].witness.empty());
}

TEST(CcvWalletV1, RefusesOutputOverwriteAndDuplicateSuccessor) {
  ContractOutput previous;
  ContractOutput successor;
  std::string error;
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 3, {0x11}, {}, previous, error));
  ASSERT_TRUE(
      BuildContractOutput(ContractScript(), 4, {0x22}, {}, successor, error));

  Transaction tx;
  tx.vin.emplace_back();
  tx.vout.emplace_back(AmountUna::Una(1), std::vector<uint8_t>{0x51});
  Transition transition;
  EXPECT_FALSE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {0x22}, {},
                                  transition, error));
  EXPECT_NE(error.find("overwrite"), std::string::npos);
  EXPECT_TRUE(tx.vin[0].witness.empty());

  tx.vout[0] = TxOutput();
  tx.vout.emplace_back(LOCKED_VALUE, successor.scriptPubKey);
  EXPECT_FALSE(PopulateTransition(tx, 0, LOCKED_VALUE, previous, {0x22}, {},
                                  transition, error));
  EXPECT_NE(error.find("more than once"), std::string::npos);
  EXPECT_TRUE(tx.vin[0].witness.empty());
  EXPECT_TRUE(tx.vout[0].scriptPubKey.empty());
}

} // namespace
} // namespace dinero::wallet::ccv
