/**
 * Phase G.3.3: Consensus Validation Implementation
 *
 * Pure consensus evaluation - deterministic, read-only, no state mutation.
 */

#include "../../include/p2p/consensus_validator.h"
#include "../../include/consensus/subsidy.h"
#include <set>

namespace dinero {
namespace p2p {

//=============================================================================
// Subsidy Function (Economics Layer Link)
//=============================================================================

uint64_t GetBlockSubsidy(uint32_t height, const ConsensusParams& params) {
    (void)params;  // Unused - subsidy is constant per height
    // Phase M.6.3: Extract raw value for boundary type (uint64_t return)
    return ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
}

//=============================================================================
// Transaction Validation
//=============================================================================

ConsensusValidationResult ConsensusValidator::validateTx(
    const Transaction& tx,
    const IUTXOSnapshot& utxo_view,
    const ConsensusParams& params
) {
    // Step 1: Basic sanity checks
    auto sanity = checkTransactionSanity(tx);
    if (!sanity.ok) {
        return sanity;
    }

    // Step 2: Check for duplicate inputs
    if (hasDuplicateInputs(tx)) {
        return ConsensusValidationResult::Fail("Transaction has duplicate inputs");
    }

    // Step 3: Verify inputs against UTXO set
    auto input_check = verifyInputs(tx, utxo_view);
    if (!input_check.ok) {
        return input_check;
    }

    // Note: Full script execution would go here
    // For now, basic structural checks pass

    return ConsensusValidationResult::Ok();
}

//=============================================================================
// Coinbase Validation
//=============================================================================

ConsensusValidationResult ConsensusValidator::validateCoinbase(
    const Transaction& tx,
    uint32_t height,
    const ConsensusParams& params
) {
    // Coinbase must have exactly 1 input
    if (tx.inputs.size() != 1) {
        return ConsensusValidationResult::Fail("Coinbase must have exactly 1 input");
    }

    // Coinbase input must be null outpoint
    if (!tx.inputs[0].prevout.IsNull()) {
        return ConsensusValidationResult::Fail("Coinbase input must be null outpoint");
    }

    // Coinbase must have at least 1 output
    if (tx.outputs.empty()) {
        return ConsensusValidationResult::Fail("Coinbase must have at least 1 output");
    }

    // Note: Subsidy validation would go here (but that's consensus context)
    // For now, basic coinbase shape is valid

    return ConsensusValidationResult::Ok();
}

//=============================================================================
// Helper: Check Transaction Sanity
//=============================================================================

ConsensusValidationResult ConsensusValidator::checkTransactionSanity(const Transaction& tx) const {
    // Must have inputs
    if (tx.inputs.empty()) {
        return ConsensusValidationResult::Fail("Transaction has no inputs");
    }

    // Must have outputs
    if (tx.outputs.empty()) {
        return ConsensusValidationResult::Fail("Transaction has no outputs");
    }

    // Check output values are sane
    uint64_t total_out = 0;
    for (const auto& output : tx.outputs) {
        if (output.value == 0 && !output.scriptPubKey.empty()) {
            // Zero-value outputs are allowed (OP_RETURN)
        }

        // Check for overflow
        uint64_t prev_total = total_out;
        total_out += output.value;
        if (total_out < prev_total) {
            return ConsensusValidationResult::Fail("Output value overflow");
        }
    }

    return ConsensusValidationResult::Ok();
}

//=============================================================================
// Helper: Check Duplicate Inputs
//=============================================================================

bool ConsensusValidator::hasDuplicateInputs(const Transaction& tx) const {
    std::set<OutPoint> seen;

    for (const auto& input : tx.inputs) {
        if (seen.count(input.prevout) > 0) {
            return true;  // Duplicate found
        }
        seen.insert(input.prevout);
    }

    return false;
}

//=============================================================================
// Helper: Verify Inputs
//=============================================================================

ConsensusValidationResult ConsensusValidator::verifyInputs(
    const Transaction& tx,
    const IUTXOSnapshot& utxo_view
) const {
    // Skip coinbase (no inputs to verify)
    if (tx.isCoinbase()) {
        return ConsensusValidationResult::Ok();
    }

    uint64_t total_in = 0;

    for (const auto& input : tx.inputs) {
        // Check UTXO exists
        auto utxo = utxo_view.getUTXO(input.prevout);
        if (!utxo.has_value()) {
            return ConsensusValidationResult::Fail("Input references missing UTXO");
        }

        // Accumulate input value
        uint64_t prev_total = total_in;
        total_in += utxo->value;
        if (total_in < prev_total) {
            return ConsensusValidationResult::Fail("Input value overflow");
        }

        // Note: Script verification would go here
        // For now, we trust that if the UTXO exists, basic spending is valid
    }

    // Calculate output total
    uint64_t total_out = 0;
    for (const auto& output : tx.outputs) {
        total_out += output.value;
    }

    // Check inputs >= outputs (fee is implicit)
    if (total_in < total_out) {
        return ConsensusValidationResult::Fail("Inputs less than outputs (insufficient funds)");
    }

    return ConsensusValidationResult::Ok();
}

//=============================================================================
// Extended Transaction Validation (with Fee Information)
//=============================================================================

TxValidationResult ConsensusValidator::validateTxExtended(
    const Transaction& tx,
    const IUTXOSnapshot& utxo_view,
    const ConsensusParams& params
) {
    // Step 1: Basic sanity checks
    auto sanity = checkTransactionSanity(tx);
    if (!sanity.ok) {
        return TxValidationResult::Fail(sanity.error);
    }

    // Step 2: Check for duplicate inputs
    if (hasDuplicateInputs(tx)) {
        return TxValidationResult::Fail("Transaction has duplicate inputs");
    }

    // Step 3: Calculate input and output totals
    uint64_t total_in = 0;
    uint64_t total_out = 0;

    // Coinbase has no inputs to verify
    if (!tx.isCoinbase()) {
        for (const auto& input : tx.inputs) {
            // Check UTXO exists
            auto utxo = utxo_view.getUTXO(input.prevout);
            if (!utxo.has_value()) {
                return TxValidationResult::Fail("Input references missing UTXO");
            }

            // Accumulate input value (with overflow check)
            uint64_t prev_total = total_in;
            total_in += utxo->value;
            if (total_in < prev_total) {
                return TxValidationResult::Fail("Input value overflow");
            }
        }
    }

    // Calculate output total (with overflow check)
    for (const auto& output : tx.outputs) {
        uint64_t prev_total = total_out;
        total_out += output.value;
        if (total_out < prev_total) {
            return TxValidationResult::Fail("Output value overflow");
        }
    }

    // For non-coinbase: check inputs >= outputs
    if (!tx.isCoinbase()) {
        if (total_in < total_out) {
            return TxValidationResult::Fail("Inputs less than outputs (insufficient funds)");
        }
    }

    // Calculate fee (0 for coinbase)
    uint64_t fee = tx.isCoinbase() ? 0 : (total_in - total_out);

    return TxValidationResult::Ok(total_in, total_out, fee);
}

//=============================================================================
// Block-Level Validation (Authoritative - G.3.4 Depends On This)
//=============================================================================

BlockValidationResult ConsensusValidator::validateBlock(
    const Block& block,
    uint32_t height,
    const IUTXOSnapshot& utxo_view,
    const ConsensusParams& params
) {
    // Block must have at least coinbase
    if (block.transactions.empty()) {
        return BlockValidationResult::Fail("Block has no transactions");
    }

    // First transaction must be coinbase
    if (!block.transactions[0].isCoinbase()) {
        return BlockValidationResult::Fail("First transaction is not coinbase");
    }

    // Only first transaction can be coinbase
    for (size_t i = 1; i < block.transactions.size(); i++) {
        if (block.transactions[i].isCoinbase()) {
            return BlockValidationResult::Fail("Non-first transaction is coinbase");
        }
    }

    // Validate coinbase shape
    auto coinbase_shape = validateCoinbase(block.transactions[0], height, params);
    if (!coinbase_shape.ok) {
        return BlockValidationResult::Fail(coinbase_shape.error);
    }

    // Validate all non-coinbase transactions and accumulate fees
    uint64_t total_fees = 0;

    for (size_t i = 1; i < block.transactions.size(); i++) {
        auto tx_result = validateTxExtended(block.transactions[i], utxo_view, params);
        if (!tx_result.ok) {
            return BlockValidationResult::Fail("Transaction " + std::to_string(i) + " invalid: " + tx_result.error);
        }

        // Accumulate fees (with overflow check)
        uint64_t prev_fees = total_fees;
        total_fees += tx_result.fee;
        if (total_fees < prev_fees) {
            return BlockValidationResult::Fail("Fee accumulation overflow");
        }
    }

    // Calculate coinbase value
    uint64_t coinbase_value = 0;
    for (const auto& output : block.transactions[0].outputs) {
        uint64_t prev_total = coinbase_value;
        coinbase_value += output.value;
        if (coinbase_value < prev_total) {
            return BlockValidationResult::Fail("Coinbase value overflow");
        }
    }

    // Get block subsidy
    uint64_t subsidy = GetBlockSubsidy(height, params);

    // Check coinbase doesn't exceed subsidy + fees (with overflow check)
    uint64_t max_allowed = subsidy;
    if (UINT64_MAX - subsidy >= total_fees) {
        max_allowed = subsidy + total_fees;
    } else {
        return BlockValidationResult::Fail("Subsidy + fees overflow");
    }

    if (coinbase_value > max_allowed) {
        return BlockValidationResult::Fail("Coinbase output exceeds subsidy + fees");
    }

    return BlockValidationResult::Ok(total_fees, coinbase_value, subsidy);
}

} // namespace p2p
} // namespace dinero

