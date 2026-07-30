#include "rpc/methods_wallet_ccv.h"

#include "consensus/chainparams.h"
#include "consensus/limits.h"
#include "primitives/transaction.h"
#include "wallet/ccv_wallet_v1.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dinero::rpc {
namespace {

constexpr const char *RPC_SCHEMA = "din.wallet.ccv.v1";

din::Json Error(const std::string &message) {
  din::Json response;
  response["rpc_schema"] = RPC_SCHEMA;
  response["success"] = false;
  response["error"] = message;
  return response;
}

char HexDigit(uint8_t nibble) {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

std::string Hex(const uint8_t *begin, const uint8_t *end) {
  std::string result;
  result.resize(static_cast<size_t>(end - begin) * 2);
  size_t cursor = 0;
  for (const uint8_t *it = begin; it != end; ++it) {
    result[cursor++] = HexDigit(static_cast<uint8_t>(*it >> 4));
    result[cursor++] = HexDigit(static_cast<uint8_t>(*it & 0x0f));
  }
  return result;
}

template <typename Bytes> std::string Hex(const Bytes &bytes) {
  if (bytes.empty()) {
    return {};
  }
  return Hex(bytes.data(), bytes.data() + bytes.size());
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool ParseHex(const din::Json &value, const std::string &name, size_t maxBytes,
              std::vector<uint8_t> &bytes, std::string &error) {
  if (!value.isString()) {
    error = name + " must be a hex string";
    return false;
  }
  const std::string text = value.asString();
  if ((text.size() & 1U) != 0) {
    error = name + " must contain an even number of hex characters";
    return false;
  }
  if (text.size() / 2 > maxBytes) {
    error = name + " exceeds its size limit";
    return false;
  }

  bytes.clear();
  bytes.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = HexValue(text[i]);
    const int low = HexValue(text[i + 1]);
    if (high < 0 || low < 0) {
      error = name + " contains a non-hex character";
      bytes.clear();
      return false;
    }
    bytes.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return true;
}

din::Json DescriptorJson(const wallet::ccv::ContractOutput &output) {
  din::Json descriptor;
  descriptor["tapscript_hex"] = Hex(output.tapscript);
  descriptor["counter"] = static_cast<Json::UInt>(output.state.counter);
  descriptor["state_data_hex"] = Hex(output.state.data);
  din::Json path(Json::arrayValue);
  for (const auto &sibling : output.merklePath) {
    path.append(Hex(sibling));
  }
  descriptor["merkle_path"] = std::move(path);
  return descriptor;
}

din::Json ContractOutputJson(const wallet::ccv::ContractOutput &output) {
  din::Json result;
  result["descriptor"] = DescriptorJson(output);
  result["state_hash"] = Hex(output.state.stateHash);
  result["code_hash"] = Hex(output.state.codeHash);
  result["merkle_root"] = Hex(output.merkleRoot);
  result["internal_key"] = Hex(output.internalKey);
  result["output_key_parity"] = static_cast<Json::UInt>(output.outputKeyParity);
  result["script_pubkey_hex"] = Hex(output.scriptPubKey);
  result["control_block_hex"] = Hex(output.controlBlock);
  return result;
}

bool ParseDescriptor(const din::Json &json, wallet::ccv::ContractOutput &output,
                     std::string &error) {
  if (!json.isObject()) {
    error = "descriptor must be an object";
    return false;
  }
  if (!json.isMember("tapscript_hex") || !json.isMember("counter")) {
    error = "descriptor requires tapscript_hex and counter";
    return false;
  }
  if (!json["counter"].isUInt()) {
    error = "descriptor.counter must be a uint32";
    return false;
  }

  std::vector<uint8_t> tapscript;
  if (!ParseHex(json["tapscript_hex"], "descriptor.tapscript_hex",
                consensus::MAX_SCRIPT_SIZE, tapscript, error)) {
    return false;
  }

  std::vector<uint8_t> stateData;
  if (json.isMember("state_data_hex") &&
      !ParseHex(json["state_data_hex"], "descriptor.state_data_hex",
                consensus::MAX_CONTRACT_STATE_DATA_SIZE, stateData, error)) {
    return false;
  }

  std::vector<std::array<uint8_t, 32>> merklePath;
  if (json.isMember("merkle_path")) {
    if (!json["merkle_path"].isArray()) {
      error = "descriptor.merkle_path must be an array";
      return false;
    }
    if (json["merkle_path"].size() >
        wallet::ccv::MAX_TAPROOT_MERKLE_PATH_NODES) {
      error = "descriptor.merkle_path exceeds the control-block limit";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < json["merkle_path"].size(); ++i) {
      std::vector<uint8_t> siblingBytes;
      if (!ParseHex(json["merkle_path"][i], "descriptor.merkle_path entry", 32,
                    siblingBytes, error)) {
        return false;
      }
      if (siblingBytes.size() != 32) {
        error = "descriptor.merkle_path entries must be 32 bytes";
        return false;
      }
      std::array<uint8_t, 32> sibling{};
      std::copy(siblingBytes.begin(), siblingBytes.end(), sibling.begin());
      merklePath.push_back(sibling);
    }
  }

  return wallet::ccv::BuildContractOutput(tapscript, json["counter"].asUInt(),
                                          stateData, merklePath, output, error);
}

} // namespace

WalletCcvRpcGate EvaluateWalletCcvRpcGate(bool requested, bool isRegtest) {
  if (!requested) {
    return WalletCcvRpcGate::Disabled;
  }
  return isRegtest ? WalletCcvRpcGate::Enabled
                   : WalletCcvRpcGate::RefusedNonRegtest;
}

WalletCcvRpcGate EvaluateWalletCcvRpcGateForSelectedChain(bool requested) {
  return EvaluateWalletCcvRpcGate(requested, Params().name == "regtest");
}

din::Json HandleWalletCcvCreateOutput(const ExecutionContext &,
                                      const din::Json &params) {
  wallet::ccv::ContractOutput output;
  std::string error;
  if (!ParseDescriptor(params, output, error)) {
    return Error(error);
  }

  din::Json response = ContractOutputJson(output);
  response["rpc_schema"] = RPC_SCHEMA;
  response["success"] = true;
  response["construction_only"] = true;
  response["signed"] = false;
  response["broadcast"] = false;
  return response;
}

din::Json HandleWalletCcvBuildTransition(const ExecutionContext &,
                                         const din::Json &params) {
  if (!params.isObject() || !params.isMember("tx_hex") ||
      !params.isMember("input_index") || !params.isMember("locked_value_una") ||
      !params.isMember("previous") || !params.isMember("successor_data_hex")) {
    return Error("tx_hex, input_index, locked_value_una, previous, and "
                 "successor_data_hex are required");
  }
  if (!params["input_index"].isUInt()) {
    return Error("input_index must be a uint32");
  }
  if (!params["locked_value_una"].isUInt64()) {
    return Error("locked_value_una must be a uint64");
  }

  const uint64_t rawValue = params["locked_value_una"].asUInt64();
  const AmountUna lockedValue = AmountUna::Una(rawValue);
  if (!lockedValue.IsPositive() || !lockedValue.IsWithinSupply()) {
    return Error("locked_value_una must be within the positive money range");
  }

  std::string error;
  std::vector<uint8_t> txBytes;
  if (!ParseHex(params["tx_hex"], "tx_hex", consensus::MAX_TX_SIZE, txBytes,
                error) ||
      txBytes.empty()) {
    return Error(error.empty() ? "tx_hex must not be empty" : error);
  }
  Transaction tx;
  size_t consumed = 0;
  if (!TransactionSerializer::Deserialize(tx, txBytes, consumed) ||
      consumed != txBytes.size() ||
      tx.Serialize(TxSerializationMode::WithWitness) != txBytes) {
    return Error("tx_hex is not one canonical complete transaction");
  }

  wallet::ccv::ContractOutput previous;
  if (!ParseDescriptor(params["previous"], previous, error)) {
    return Error("invalid previous descriptor: " + error);
  }

  std::vector<uint8_t> successorData;
  if (!ParseHex(params["successor_data_hex"], "successor_data_hex",
                consensus::MAX_CONTRACT_STATE_DATA_SIZE, successorData,
                error)) {
    return Error(error);
  }

  std::vector<std::vector<uint8_t>> witnessPrefix;
  if (params.isMember("witness_prefix")) {
    if (!params["witness_prefix"].isArray()) {
      return Error("witness_prefix must be an array");
    }
    if (params["witness_prefix"].size() > consensus::MAX_STACK_SIZE - 2) {
      return Error("witness_prefix exceeds the Tapscript stack limit");
    }
    witnessPrefix.reserve(params["witness_prefix"].size());
    for (Json::ArrayIndex i = 0; i < params["witness_prefix"].size(); ++i) {
      std::vector<uint8_t> element;
      if (!ParseHex(params["witness_prefix"][i], "witness_prefix entry",
                    consensus::MAX_SCRIPT_ELEMENT_SIZE, element, error)) {
        return Error(error);
      }
      witnessPrefix.push_back(std::move(element));
    }
  }

  wallet::ccv::Transition transition;
  if (!wallet::ccv::PopulateTransition(tx, params["input_index"].asUInt(),
                                       lockedValue, previous, successorData,
                                       witnessPrefix, transition, error)) {
    return Error(error);
  }

  const auto serialized = tx.Serialize(TxSerializationMode::WithWitness);
  if (serialized.size() > consensus::MAX_TX_SIZE) {
    return Error("constructed transaction exceeds the consensus size limit");
  }

  din::Json response;
  response["rpc_schema"] = RPC_SCHEMA;
  response["success"] = true;
  response["construction_only"] = true;
  response["signed"] = false;
  response["broadcast"] = false;
  response["consensus_validated"] = false;
  response["input_index"] = params["input_index"];
  response["locked_value_una"] = Json::Value::UInt64(rawValue);
  response["tx_hex"] = Hex(serialized);
  response["successor"] = ContractOutputJson(transition.successor);
  return response;
}

} // namespace dinero::rpc
