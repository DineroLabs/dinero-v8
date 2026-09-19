#include "consensus/block_reward.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <algorithm>
#include <limits>

namespace dinero::consensus {
namespace reward_detail {

bool HasConfidentialInputs(const std::vector<UTXOEntry>& input_utxos) {
    return std::any_of(input_utxos.begin(), input_utxos.end(), [](const UTXOEntry& utxo) {
        return utxo.is_confidential;
    });
}

bool UsesConfidentialValueSemantics(const Transaction& tx, const std::vector<UTXOEntry>& input_utxos) {
    return tx.HasConfidentialOutputs() || HasConfidentialInputs(input_utxos);
}

bool UsesShieldedValueSemantics(const Transaction& tx) {
    return Transaction::IsShieldedVersion(tx.version) ||
           !tx.shielded_bundle_bytes.empty();
}

bool ComputeValidatedTransactionFee(const Transaction& tx,
                                    const std::vector<UTXOEntry>& input_utxos,
                                    uint64_t total_input_value,
                                    uint64_t total_output_value,
                                    uint64_t& fee,
                                    std::string& error) {
    if (UsesConfidentialValueSemantics(tx, input_utxos) ||
        (UsesShieldedValueSemantics(tx) && tx.HasExplicitFee())) {
        if (!tx.HasExplicitFee()) {
            error = UsesShieldedValueSemantics(tx)
                ? "Shielded transaction missing explicit fee"
                : "Confidential transaction missing explicit fee";
            return false;
        }
        fee = tx.GetExplicitFee();
        return true;
    }

    if (total_output_value > total_input_value) {
        error = "Outputs exceed inputs (negative fee)";
        return false;
    }

    fee = total_input_value - total_output_value;
    return true;
}

bool AddRewardAmount(uint64_t value, uint64_t& total,
                     const char* overflow_error, std::string& error) {
    if (value > std::numeric_limits<uint64_t>::max() - total) {
        error = overflow_error;
        return false;
    }
    total += value;
    return true;
}

bool SumRewardOutputs(const Transaction& tx, uint64_t& total, std::string& error) {
    total = 0;
    for (const auto& output : tx.vout) {
        // Include every output, including unspendable commitments, exactly as
        // ConnectBlock's SumOutputs does. Overflow is a rejection, never a fee.
        if (!AddRewardAmount(output.value.GetUna(), total,
                             "block-reward-output-total-overflow", error)) {
            return false;
        }
    }
    return true;
}

bool CheckCoinbaseReward(const Transaction& coinbase, uint32_t height,
                         uint64_t total_fees, std::string& error) {
    uint64_t expected_reward = ConsensusSubsidy::GetBlockSubsidy(
        height, Params().sixty_second_activation_height).GetUna();
    if (!AddRewardAmount(total_fees, expected_reward,
                         "block-reward-subsidy-fee-overflow", error)) {
        return false;
    }
    uint64_t coinbase_output_value = 0;
    if (!SumRewardOutputs(coinbase, coinbase_output_value, error)) {
        return false;
    }
    if (coinbase_output_value > expected_reward) {
        error = "Coinbase pays too much: " + std::to_string(coinbase_output_value) +
                " > " + std::to_string(expected_reward);
        return false;
    }
    return true;
}

} // namespace reward_detail

using reward_detail::AddRewardAmount;
using reward_detail::CheckCoinbaseReward;
using reward_detail::ComputeValidatedTransactionFee;
using reward_detail::SumRewardOutputs;

bool CheckBlockRewardFromSpentOutputs(
    const Block& block, uint32_t height,
    const std::vector<SpentOutputData>* spent_outputs, std::string& error) {
    if (block.vtx.empty()) {
        error = "Block has no transactions (missing coinbase)";
        return false;
    }
    if (!block.vtx.front().IsCoinbase()) {
        error = "block-reward-first-transaction-not-coinbase";
        return false;
    }

    const size_t available = spent_outputs ? spent_outputs->size() : 0;
    size_t consumed = 0;
    uint64_t total_fees = 0;
    for (size_t tx_index = 1; tx_index < block.vtx.size(); ++tx_index) {
        const auto& tx = block.vtx[tx_index];
        uint64_t input_total = 0;
        std::vector<UTXOEntry> input_utxos;
        input_utxos.reserve(tx.vin.size());
        for (size_t input = 0; input < tx.vin.size(); ++input) {
            if (consumed >= available) {
                error = spent_outputs ? "block-reward-spent-outputs-underrun"
                                      : "block-reward-missing-spent-outputs";
                return false;
            }
            const auto& spent = (*spent_outputs)[consumed++];
            if (!AddRewardAmount(spent.value, input_total,
                                 "block-reward-input-total-overflow", error)) {
                return false;
            }
            // The value semantics match ConnectBlock's stateless input view.
            // This reconstruction is NOT proof authentication; ReplayBlock
            // must still bind these records to targets and the forest root.
            const bool v2 = IsUtreexoMaturityLeafActive(spent.created_height);
            input_utxos.emplace_back(
                AmountUna::UnsafeFromRaw(spent.value), spent.scriptPubKey,
                v2 ? spent.created_height : 0, v2 && spent.is_coinbase,
                spent.is_confidential, spent.commitment);
        }
        uint64_t output_total = 0;
        if (!SumRewardOutputs(tx, output_total, error)) {
            return false;
        }
        uint64_t fee = 0;
        if (!ComputeValidatedTransactionFee(tx, input_utxos, input_total,
                                            output_total, fee, error) ||
            !AddRewardAmount(fee, total_fees,
                             "block-reward-fee-total-overflow", error)) {
            return false;
        }
    }
    if (consumed != available) {
        error = "block-reward-spent-outputs-surplus";
        return false;
    }
    return CheckCoinbaseReward(block.vtx.front(), height, total_fees, error);
}

} // namespace dinero::consensus
