#pragma once

#include "consensus/utxo_entry.h"
#include "primitives/block.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::consensus {

/**
 * Read-only reward accounting for CSN replay, using the same fee semantics and
 * reward comparator as ConnectBlock. Every non-coinbase input consumes one
 * supplied spent-output record in transaction/input order; missing or surplus
 * records fail closed. No records are needed when there are no such inputs.
 *
 * This does NOT authenticate the supplied amounts, validate scripts/shielded
 * proofs, or apply state. Callers must retain Utreexo proof/target checks before
 * accepting or committing a block. Legacy hash-only replay with inputs cannot
 * establish exact fees and must obtain the missing metadata instead of guessing.
 */
bool CheckBlockRewardFromSpentOutputs(
    const Block& block, uint32_t height,
    const std::vector<SpentOutputData>* spent_outputs, std::string& error);

// Shared implementation primitives for ordinary ConnectBlock and proof-based
// replay. Kept in the pure consensus library so network proof validation does
// not acquire daemon/storage dependencies merely to check reward arithmetic.
namespace reward_detail {
bool HasConfidentialInputs(const std::vector<UTXOEntry>& input_utxos);
bool UsesShieldedValueSemantics(const Transaction& tx);
bool ComputeValidatedTransactionFee(const Transaction& tx,
    const std::vector<UTXOEntry>& input_utxos,
    uint64_t total_input_value, uint64_t total_output_value,
    uint64_t& fee, std::string& error);
bool AddRewardAmount(uint64_t value, uint64_t& total,
    const char* overflow_error, std::string& error);
bool CheckCoinbaseReward(const Transaction& coinbase, uint32_t height,
    uint64_t total_fees, std::string& error);
} // namespace reward_detail

} // namespace dinero::consensus
