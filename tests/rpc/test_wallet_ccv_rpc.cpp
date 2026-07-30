#include <gtest/gtest.h>

#include "consensus/chainparams.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "rpc/methods_wallet_ccv.h"
#include "wallet/ccv_wallet_v1.h"

namespace dinero::rpc {
namespace {

constexpr auto LOCKED_VALUE = AmountUna::Una(250'000);

std::string Hex(const std::vector<uint8_t> &bytes) {
  return TransactionSerializer::ToHex(bytes);
}

din::Json Descriptor(uint32_t counter, const std::string &dataHex) {
  din::Json descriptor;
  descriptor["tapscript_hex"] = TransactionSerializer::ToHex(
      {static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
       static_cast<uint8_t>(consensus::OP_TRUE)});
  descriptor["counter"] = static_cast<Json::UInt>(counter);
  descriptor["state_data_hex"] = dataHex;
  descriptor["merkle_path"] = din::Json(Json::arrayValue);
  return descriptor;
}

Transaction PlaceholderTransaction() {
  Transaction tx;
  tx.version = Transaction::TX_VERSION_SEGWIT;
  tx.vin.emplace_back();
  tx.vout.emplace_back();
  return tx;
}

TEST(WalletCcvRpc, GateIsOptInAndRegtestOnly) {
  EXPECT_EQ(EvaluateWalletCcvRpcGate(false, false), WalletCcvRpcGate::Disabled);
  EXPECT_EQ(EvaluateWalletCcvRpcGate(false, true), WalletCcvRpcGate::Disabled);
  EXPECT_EQ(EvaluateWalletCcvRpcGate(true, false),
            WalletCcvRpcGate::RefusedNonRegtest);
  EXPECT_EQ(EvaluateWalletCcvRpcGate(true, true), WalletCcvRpcGate::Enabled);

  SelectParams(Chain::MAINNET);
  EXPECT_EQ(EvaluateWalletCcvRpcGateForSelectedChain(true),
            WalletCcvRpcGate::RefusedNonRegtest);
  SelectParams(Chain::REGTEST);
  EXPECT_EQ(EvaluateWalletCcvRpcGateForSelectedChain(true),
            WalletCcvRpcGate::Enabled);
  SelectParams(Chain::MAINNET);
}

TEST(WalletCcvRpc, CreateOutputReturnsRoundTrippableDescriptor) {
  ExecutionContext context;
  const auto response =
      HandleWalletCcvCreateOutput(context, Descriptor(7, "1020"));

  ASSERT_TRUE(response["success"].asBool()) << response["error"].asString();
  EXPECT_EQ(response["rpc_schema"].asString(), "din.wallet.ccv.v1");
  EXPECT_TRUE(response["construction_only"].asBool());
  EXPECT_FALSE(response["signed"].asBool());
  EXPECT_FALSE(response["broadcast"].asBool());
  EXPECT_EQ(response["script_pubkey_hex"].asString().size(), 68u);
  EXPECT_EQ(response["control_block_hex"].asString().size(), 66u);
  EXPECT_EQ(response["descriptor"]["counter"].asUInt(), 7u);
  EXPECT_EQ(response["descriptor"]["state_data_hex"].asString(), "1020");
}

TEST(WalletCcvRpc, TransitionRoundTripsThroughActivatedConsensus) {
  ExecutionContext context;
  const auto previousResponse =
      HandleWalletCcvCreateOutput(context, Descriptor(20, "01"));
  ASSERT_TRUE(previousResponse["success"].asBool());

  const Transaction placeholder = PlaceholderTransaction();
  din::Json params;
  params["tx_hex"] =
      Hex(placeholder.Serialize(TxSerializationMode::WithWitness));
  params["input_index"] = 0;
  params["locked_value_una"] = Json::Value::UInt64(LOCKED_VALUE.GetUna());
  params["previous"] = previousResponse["descriptor"];
  params["successor_data_hex"] = "02";
  params["witness_prefix"] = din::Json(Json::arrayValue);

  const auto response = HandleWalletCcvBuildTransition(context, params);
  ASSERT_TRUE(response["success"].asBool()) << response["error"].asString();
  EXPECT_TRUE(response["construction_only"].asBool());
  EXPECT_FALSE(response["signed"].asBool());
  EXPECT_FALSE(response["broadcast"].asBool());
  EXPECT_FALSE(response["consensus_validated"].asBool());
  EXPECT_EQ(response["successor"]["descriptor"]["counter"].asUInt(), 21u);

  Transaction tx;
  const auto raw =
      TransactionSerializer::FromHex(response["tx_hex"].asString());
  size_t consumed = 0;
  ASSERT_TRUE(TransactionSerializer::Deserialize(tx, raw, consumed));
  ASSERT_EQ(consumed, raw.size());

  wallet::ccv::ContractOutput previous;
  std::string error;
  ASSERT_TRUE(wallet::ccv::BuildContractOutput(
      {static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
       static_cast<uint8_t>(consensus::OP_TRUE)},
      20, {0x01}, {}, previous, error));
  const std::vector<consensus::UTXOEntry> inputs{
      consensus::UTXOEntry(LOCKED_VALUE, previous.scriptPubKey, 100, false)};
  EXPECT_TRUE(consensus::ScriptVerifier::VerifyTaproot(
      tx, 0, inputs, error,
      consensus::SCRIPT_VERIFY_STANDARD |
          consensus::SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING))
      << error;
}

TEST(WalletCcvRpc, RejectsNonHexTrailingBytesAndUnsafeAmounts) {
  ExecutionContext context;

  auto malformed = Descriptor(0, "");
  malformed["tapscript_hex"] = "be5z";
  auto response = HandleWalletCcvCreateOutput(context, malformed);
  EXPECT_FALSE(response["success"].asBool());
  EXPECT_NE(response["error"].asString().find("non-hex"), std::string::npos);

  auto oversized = Descriptor(
      0, std::string((consensus::MAX_CONTRACT_STATE_DATA_SIZE + 1) * 2, '0'));
  response = HandleWalletCcvCreateOutput(context, oversized);
  EXPECT_FALSE(response["success"].asBool());
  EXPECT_NE(response["error"].asString().find("size limit"), std::string::npos);

  const auto previousResponse =
      HandleWalletCcvCreateOutput(context, Descriptor(0, ""));
  ASSERT_TRUE(previousResponse["success"].asBool());

  const Transaction placeholder = PlaceholderTransaction();
  din::Json params;
  params["tx_hex"] =
      Hex(placeholder.Serialize(TxSerializationMode::WithWitness)) + "00";
  params["input_index"] = 0;
  params["locked_value_una"] = Json::Value::UInt64(LOCKED_VALUE.GetUna());
  params["previous"] = previousResponse["descriptor"];
  params["successor_data_hex"] = "";
  response = HandleWalletCcvBuildTransition(context, params);
  EXPECT_FALSE(response["success"].asBool());
  EXPECT_NE(response["error"].asString().find("complete"), std::string::npos);

  params["tx_hex"] =
      Hex(placeholder.Serialize(TxSerializationMode::WithWitness));
  params["locked_value_una"] = Json::Value::UInt64(MAX_SUPPLY_UNA_CONST + 1);
  response = HandleWalletCcvBuildTransition(context, params);
  EXPECT_FALSE(response["success"].asBool());
  EXPECT_NE(response["error"].asString().find("money range"),
            std::string::npos);
}

} // namespace
} // namespace dinero::rpc
