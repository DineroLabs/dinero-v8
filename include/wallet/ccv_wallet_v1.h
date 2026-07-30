#pragma once

#include "consensus/covenants.h"
#include "primitives/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::wallet::ccv {

inline constexpr uint8_t TAPSCRIPT_LEAF_VERSION = 0xc0;
inline constexpr size_t MAX_TAPROOT_MERKLE_PATH_NODES = 128;

/**
 * Everything a wallet must retain to spend one transparent CCV output.
 *
 * No private key exists for the state-derived internal key. Spending is
 * script-path only.
 */
struct ContractOutput {
  consensus::ContractState state;
  std::vector<uint8_t> tapscript;
  std::vector<std::array<uint8_t, 32>> merklePath;
  std::array<uint8_t, 32> merkleRoot{};
  std::array<uint8_t, 32> internalKey{};
  uint8_t outputKeyParity{0};
  std::vector<uint8_t> scriptPubKey;
  std::vector<uint8_t> controlBlock;
};

struct Transition {
  ContractOutput successor;
  std::vector<uint8_t> previousStateWitness;
  std::vector<uint8_t> successorStateWitness;
};

/** Canonical CCV witness encoding: stateHash || codeHash || counter_le32 ||
 * data_len_le32 || data. */
std::vector<uint8_t>
SerializeContractState(const consensus::ContractState &state);

/**
 * Build a transparent state-derived P2TR output for a revealed CCV leaf.
 *
 * merklePath contains the sibling hash at each level from leaf to root. This
 * permits recovery/terminal leaves without teaching this constructor their
 * business semantics.
 */
bool BuildContractOutput(const std::vector<uint8_t> &tapscript,
                         uint32_t counter,
                         const std::vector<uint8_t> &stateData,
                         const std::vector<std::array<uint8_t, 32>> &merklePath,
                         ContractOutput &output, std::string &error);

/**
 * Populate an existing transaction's CCV input witness and matching successor
 * output. The caller owns coin selection, fee inputs/outputs, signing, and
 * broadcast. tx.vin[inputIndex] and tx.vout[inputIndex] must already exist;
 * the target witness and same-index output must be empty placeholders so
 * nothing is silently overwritten.
 */
bool PopulateTransition(Transaction &tx, uint32_t inputIndex,
                        AmountUna lockedValue, const ContractOutput &previous,
                        const std::vector<uint8_t> &successorData,
                        const std::vector<std::vector<uint8_t>> &witnessPrefix,
                        Transition &transition, std::string &error);

} // namespace dinero::wallet::ccv
