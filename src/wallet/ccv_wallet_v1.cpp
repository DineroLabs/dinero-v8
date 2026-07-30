#include "wallet/ccv_wallet_v1.h"

#include "consensus/limits.h"
#include "consensus/script.h"
#include "wallet/taproot_keys.h"

#include <limits>
#include <utility>

namespace dinero::wallet::ccv {
namespace {

void WriteLE32(std::vector<uint8_t> &bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 24));
}

bool ContainsCcvOpcode(const std::vector<uint8_t> &script) {
  size_t cursor = 0;
  bool foundCcv = false;
  while (cursor < script.size()) {
    const uint8_t opcode = script[cursor++];
    if (opcode == consensus::OP_CHECKCONTRACTVERIFY) {
      foundCcv = true;
      continue;
    }

    uint64_t pushLength = 0;
    size_t lengthBytes = 0;
    if (opcode <= 0x4b) {
      pushLength = opcode;
    } else if (opcode == consensus::OP_PUSHDATA1) {
      lengthBytes = 1;
    } else if (opcode == consensus::OP_PUSHDATA2) {
      lengthBytes = 2;
    } else if (opcode == consensus::OP_PUSHDATA4) {
      lengthBytes = 4;
    } else {
      continue;
    }

    if (cursor > script.size() || script.size() - cursor < lengthBytes) {
      return false;
    }
    for (size_t i = 0; i < lengthBytes; ++i) {
      pushLength |= static_cast<uint64_t>(script[cursor + i]) << (8 * i);
    }
    cursor += lengthBytes;
    if (pushLength > std::numeric_limits<size_t>::max() ||
        cursor > script.size() ||
        static_cast<size_t>(pushLength) > script.size() - cursor) {
      return false;
    }
    cursor += static_cast<size_t>(pushLength);
  }
  return foundCcv;
}

bool ComputeMerkleRoot(const std::vector<uint8_t> &tapscript,
                       const std::vector<std::array<uint8_t, 32>> &merklePath,
                       std::array<uint8_t, 32> &root) {
  if (!TaprootKeys::ComputeTapleafHash(tapscript, TAPSCRIPT_LEAF_VERSION,
                                       root)) {
    return false;
  }
  for (const auto &sibling : merklePath) {
    std::array<uint8_t, 32> branch{};
    if (!TaprootKeys::ComputeTapBranchHash(root, sibling, branch)) {
      return false;
    }
    root = branch;
  }
  return true;
}

} // namespace

std::vector<uint8_t>
SerializeContractState(const consensus::ContractState &state) {
  std::vector<uint8_t> bytes;
  bytes.reserve(72 + state.data.size());
  bytes.insert(bytes.end(), state.stateHash.begin(), state.stateHash.end());
  bytes.insert(bytes.end(), state.codeHash.begin(), state.codeHash.end());
  WriteLE32(bytes, state.counter);
  WriteLE32(bytes, static_cast<uint32_t>(state.data.size()));
  bytes.insert(bytes.end(), state.data.begin(), state.data.end());
  return bytes;
}

bool BuildContractOutput(const std::vector<uint8_t> &tapscript,
                         uint32_t counter,
                         const std::vector<uint8_t> &stateData,
                         const std::vector<std::array<uint8_t, 32>> &merklePath,
                         ContractOutput &output, std::string &error) {
  error.clear();
  if (tapscript.empty() || !ContainsCcvOpcode(tapscript)) {
    error = "tapscript is malformed or lacks OP_CHECKCONTRACTVERIFY in opcode "
            "position";
    return false;
  }
  if (tapscript.size() > consensus::MAX_SCRIPT_SIZE) {
    error = "tapscript exceeds the consensus script-size limit";
    return false;
  }
  if (stateData.size() > consensus::MAX_CONTRACT_STATE_DATA_SIZE) {
    error = "CCV state data exceeds the canonical witness limit";
    return false;
  }
  if (merklePath.size() > MAX_TAPROOT_MERKLE_PATH_NODES) {
    error = "Taproot Merkle path exceeds the BIP341 control-block limit";
    return false;
  }

  ContractOutput candidate;
  candidate.tapscript = tapscript;
  candidate.merklePath = merklePath;
  candidate.state.codeHash = consensus::ComputeContractCodeHash(tapscript);
  candidate.state.counter = counter;
  candidate.state.data = stateData;
  candidate.state.stateHash =
      consensus::ComputeContractStateHash(candidate.state);

  if (!ComputeMerkleRoot(candidate.tapscript, candidate.merklePath,
                         candidate.merkleRoot) ||
      !consensus::DeriveContractInternalKey(candidate.state,
                                            candidate.internalKey) ||
      !consensus::ComputeContractOutputScript(
          candidate.state, candidate.merkleRoot, candidate.scriptPubKey,
          &candidate.outputKeyParity)) {
    error = "failed to derive the CCV Taproot output";
    return false;
  }

  candidate.controlBlock.reserve(33 + 32 * candidate.merklePath.size());
  candidate.controlBlock.push_back(
      static_cast<uint8_t>(TAPSCRIPT_LEAF_VERSION | candidate.outputKeyParity));
  candidate.controlBlock.insert(candidate.controlBlock.end(),
                                candidate.internalKey.begin(),
                                candidate.internalKey.end());
  for (const auto &sibling : candidate.merklePath) {
    candidate.controlBlock.insert(candidate.controlBlock.end(), sibling.begin(),
                                  sibling.end());
  }

  output = std::move(candidate);
  return true;
}

bool PopulateTransition(Transaction &tx, uint32_t inputIndex,
                        AmountUna lockedValue, const ContractOutput &previous,
                        const std::vector<uint8_t> &successorData,
                        const std::vector<std::vector<uint8_t>> &witnessPrefix,
                        Transition &transition, std::string &error) {
  error.clear();
  if (inputIndex >= tx.vin.size() || inputIndex >= tx.vout.size()) {
    error = "CCV requires an existing input and same-index output slot";
    return false;
  }
  if (!tx.vin[inputIndex].witness.empty()) {
    error = "refusing to overwrite an existing input witness";
    return false;
  }
  const auto &targetOutput = tx.vout[inputIndex];
  if (targetOutput.value != AmountUna::Zero() ||
      !targetOutput.scriptPubKey.empty() || targetOutput.is_confidential ||
      !targetOutput.commitment.empty() || !targetOutput.range_proof.empty() ||
      !targetOutput.nonce.empty()) {
    error = "refusing to overwrite a non-empty same-index output";
    return false;
  }
  if (previous.state.counter == std::numeric_limits<uint32_t>::max()) {
    error = "CCV state counter cannot advance past UINT32_MAX";
    return false;
  }

  ContractOutput reconstructedPrevious;
  if (!BuildContractOutput(previous.tapscript, previous.state.counter,
                           previous.state.data, previous.merklePath,
                           reconstructedPrevious, error)) {
    error = "invalid previous CCV descriptor: " + error;
    return false;
  }
  if (reconstructedPrevious.state.stateHash != previous.state.stateHash ||
      reconstructedPrevious.state.codeHash != previous.state.codeHash ||
      reconstructedPrevious.merkleRoot != previous.merkleRoot ||
      reconstructedPrevious.internalKey != previous.internalKey ||
      reconstructedPrevious.outputKeyParity != previous.outputKeyParity ||
      reconstructedPrevious.scriptPubKey != previous.scriptPubKey ||
      reconstructedPrevious.controlBlock != previous.controlBlock) {
    error = "previous CCV descriptor is internally inconsistent";
    return false;
  }

  ContractOutput successor;
  if (!BuildContractOutput(previous.tapscript, previous.state.counter + 1,
                           successorData, previous.merklePath, successor,
                           error)) {
    return false;
  }
  if (successor.state.codeHash != previous.state.codeHash ||
      successor.merkleRoot != previous.merkleRoot) {
    error = "CCV successor changed immutable contract code or Taproot tree";
    return false;
  }
  for (size_t i = 0; i < tx.vout.size(); ++i) {
    if (i != inputIndex && tx.vout[i].scriptPubKey == successor.scriptPubKey) {
      error = "CCV successor script would appear more than once";
      return false;
    }
  }

  Transition candidate;
  candidate.successor = successor;
  candidate.previousStateWitness = SerializeContractState(previous.state);
  candidate.successorStateWitness = SerializeContractState(successor.state);

  auto witness = witnessPrefix;
  witness.push_back(candidate.previousStateWitness);
  witness.push_back(candidate.successorStateWitness);
  witness.push_back(previous.tapscript);
  witness.push_back(previous.controlBlock);

  tx.witness_version = 1;
  tx.vin[inputIndex].witness = std::move(witness);
  tx.vout[inputIndex] = TxOutput(lockedValue, successor.scriptPubKey);
  transition = std::move(candidate);
  return true;
}

} // namespace dinero::wallet::ccv
