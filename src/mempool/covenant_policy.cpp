/**
 * Phase 29: Mempool Covenant Policy Implementation
 */

#include "mempool/covenant_policy.h"
#include <sstream>

namespace dinero {
namespace mempool {

// ============================================================================
// CovenantPolicyValidator Implementation
// ============================================================================

CovenantPolicyValidator::CovenantPolicyValidator(const CovenantPolicyConfig& config)
    : config_(config)
{
}

std::string CovenantPolicyValidator::rejectCodeToString(CovenantRejectCode code) {
    switch (code) {
        case CovenantRejectCode::VALID:
            return "Valid";

        // CTV errors
        case CovenantRejectCode::CTV_HASH_MISMATCH:
            return "CTV hash mismatch";
        case CovenantRejectCode::CTV_OUTPUT_COUNT_MISMATCH:
            return "CTV output count mismatch";
        case CovenantRejectCode::CTV_OUTPUT_VALUE_MISMATCH:
            return "CTV output value mismatch";
        case CovenantRejectCode::CTV_OUTPUT_SCRIPT_MISMATCH:
            return "CTV output script mismatch";
        case CovenantRejectCode::CTV_VERSION_MISMATCH:
            return "CTV version mismatch";
        case CovenantRejectCode::CTV_LOCKTIME_MISMATCH:
            return "CTV locktime mismatch";
        case CovenantRejectCode::CTV_SEQUENCE_MISMATCH:
            return "CTV sequence mismatch";
        case CovenantRejectCode::CTV_TEMPLATE_NOT_FOUND:
            return "CTV template not found";
        case CovenantRejectCode::CTV_TOO_MANY_OUTPUTS:
            return "CTV too many outputs";

        // CSFS errors
        case CovenantRejectCode::CSFS_INVALID_SIGNATURE:
            return "CSFS invalid signature";
        case CovenantRejectCode::CSFS_INVALID_PUBKEY:
            return "CSFS invalid pubkey";
        case CovenantRejectCode::CSFS_MESSAGE_TOO_LARGE:
            return "CSFS message too large";
        case CovenantRejectCode::CSFS_DELEGATION_NOT_FOUND:
            return "CSFS delegation not found";
        case CovenantRejectCode::CSFS_DELEGATION_EXPIRED:
            return "CSFS delegation expired";
        case CovenantRejectCode::CSFS_DELEGATION_USED:
            return "CSFS delegation already used";

        // TXHASH errors
        case CovenantRejectCode::TXHASH_TOO_MANY_OPS:
            return "Too many TXHASH operations";
        case CovenantRejectCode::TXHASH_INVALID_FLAG:
            return "Invalid TXHASH flag";
        case CovenantRejectCode::TXHASH_INTROSPECTION_FAILED:
            return "TXHASH introspection failed";

        // CCV errors
        case CovenantRejectCode::CCV_STATE_HASH_MISMATCH:
            return "Contract state hash mismatch";
        case CovenantRejectCode::CCV_COUNTER_NOT_INCREMENTED:
            return "Contract counter not incremented";
        case CovenantRejectCode::CCV_CODE_HASH_CHANGED:
            return "Contract code hash changed";
        case CovenantRejectCode::CCV_STATE_TOO_LARGE:
            return "Contract state too large";
        case CovenantRejectCode::CCV_MAX_TRANSITIONS_EXCEEDED:
            return "Contract max transitions exceeded";

        // General covenant errors
        case CovenantRejectCode::COVENANT_SCRIPT_TOO_LARGE:
            return "Covenant script too large";
        case CovenantRejectCode::COVENANT_DEPTH_EXCEEDED:
            return "Covenant depth exceeded";
        case CovenantRejectCode::COVENANT_TOO_MANY_INPUTS:
            return "Too many covenant inputs";
        case CovenantRejectCode::COVENANT_FEE_INSUFFICIENT:
            return "Covenant fee insufficient";
        case CovenantRejectCode::COVENANT_UNCONFIRMED_PARENT:
            return "Cannot spend unconfirmed covenant UTXO";
        case CovenantRejectCode::COVENANT_RBF_NOT_SIGNALED:
            return "Covenant spend requires RBF signal";
        case CovenantRejectCode::COVENANT_RBF_FEE_TOO_LOW:
            return "Covenant RBF fee bump too low";
        case CovenantRejectCode::COVENANT_UNKNOWN_TYPE:
            return "Unknown covenant type";

        // Script errors
        case CovenantRejectCode::SCRIPT_INVALID:
            return "Invalid script";
        case CovenantRejectCode::SCRIPT_EVALUATION_FAILED:
            return "Script evaluation failed";

        default:
            return "Unknown error code: " + std::to_string(static_cast<int>(code));
    }
}

CovenantValidationResult CovenantPolicyValidator::validate(
    const Transaction& tx,
    std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup,
    wallet::CovenantWallet* covenant_wallet) const
{
    CovenantValidationResult result;
    result.accepted = true;

    // Count covenant inputs
    for (size_t i = 0; i < tx.vin.size(); i++) {
        auto utxo = utxo_lookup(tx.vin[i].prevout.txid, tx.vin[i].prevout.vout);
        if (utxo) {
            result.covenant_inputs++;
            result.covenant_value_spent += utxo->value;

            // Check covenant input limits
            if (result.covenant_inputs > config_.max_covenant_inputs_per_tx) {
                result.accepted = false;
                result.reject_code = static_cast<int>(CovenantRejectCode::COVENANT_TOO_MANY_INPUTS);
                result.reject_reason = rejectCodeToString(CovenantRejectCode::COVENANT_TOO_MANY_INPUTS);
                return result;
            }

            // Validate each covenant input
            CovenantValidationResult::InputValidation iv;
            iv.input_index = i;
            iv.type = utxo->covenant_type;
            iv.valid = true;

            // Type-specific validation
            switch (utxo->covenant_type) {
                case wallet::CovenantType::CTV:
                    // CTV validation would happen at script execution
                    break;
                case wallet::CovenantType::CSFS:
                    // CSFS signature verification at script execution
                    break;
                case wallet::CovenantType::CCV:
                    // Contract state validation
                    if (utxo->requires_state_transition) {
                        iv.valid = true; // Full validation at execution
                    }
                    break;
                default:
                    break;
            }

            result.input_validations.push_back(iv);
        }
    }

    // Count covenant outputs
    for (size_t i = 0; i < tx.vout.size(); i++) {
        // Simple heuristic: check for covenant opcodes in output scripts
        if (tx.vout[i].scriptPubKey.size() > 2) {
            result.covenant_outputs++;
            result.covenant_value_created += tx.vout[i].value;
        }
    }

    // Fee calculation
    uint64_t total_in = 0;
    for (size_t i = 0; i < tx.vin.size(); i++) {
        auto utxo = utxo_lookup(tx.vin[i].prevout.txid.AsUint256().GetHex(), tx.vin[i].prevout.vout);
        if (utxo) {
            total_in += utxo->value;
        }
    }

    uint64_t total_out = 0;
    for (const auto& out : tx.vout) {
        total_out += out.value;
    }

    result.base_fee = (total_in > total_out) ? (total_in - total_out) : 0;
    result.covenant_fee_premium = (result.base_fee * config_.covenant_fee_premium_percent) / 100;
    result.required_fee = result.base_fee + result.covenant_fee_premium;
    result.fee_sufficient = result.base_fee >= config_.min_covenant_fee_rate;

    return result;
}

bool CovenantPolicyValidator::hasCovenantInputs(
    const Transaction& tx,
    std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup) const
{
    for (const auto& vin : tx.vin) {
        auto utxo = utxo_lookup(vin.prevout.txid.AsUint256().GetHex(), vin.prevout.vout);
        if (utxo && utxo->covenant_type != wallet::CovenantType::NONE) {
            return true;
        }
    }
    return false;
}

uint64_t CovenantPolicyValidator::estimateCovenantFeePremium(
    const Transaction& tx,
    std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup) const
{
    uint32_t covenant_count = 0;
    for (const auto& vin : tx.vin) {
        auto utxo = utxo_lookup(vin.prevout.txid.AsUint256().GetHex(), vin.prevout.vout);
        if (utxo && utxo->covenant_type != wallet::CovenantUTXO::NONE) {
            covenant_count++;
        }
    }

    // Estimate: base premium + per-covenant-input premium
    return covenant_count * 100; // 100 una per covenant input
}

uint32_t CovenantPolicyValidator::countTxHashOps(const std::vector<uint8_t>& script) const {
    uint32_t count = 0;
    const uint8_t OP_TXHASH = 0xbd;

    for (size_t i = 0; i < script.size(); i++) {
        if (script[i] == OP_TXHASH) {
            count++;
        }
    }
    return count;
}

uint32_t CovenantPolicyValidator::measureCovenantDepth(const std::vector<uint8_t>& script) const {
    // Simplified depth measurement
    // Real implementation would parse script and track nesting
    return 1;
}

uint64_t CovenantPolicyValidator::calculateRequiredFee(
    const Transaction& tx,
    uint32_t covenant_inputs,
    uint64_t fee_rate) const
{
    // Estimate transaction size (simplified)
    uint64_t base_size = 10 + tx.vin.size() * 148 + tx.vout.size() * 34;
    uint64_t witness_size = covenant_inputs * 200; // Estimate for covenant witness data

    uint64_t vsize = base_size + (witness_size / 4);
    uint64_t base_fee = vsize * fee_rate;

    // Add covenant premium
    uint64_t premium = (base_fee * config_.covenant_fee_premium_percent) / 100;

    return base_fee + premium;
}

// ============================================================================
// CovenantMempoolTracker Implementation
// ============================================================================

CovenantMempoolTracker::CovenantMempoolTracker() {}

bool CovenantMempoolTracker::addTransaction(
    const Transaction& tx,
    const std::vector<std::pair<uint256, uint32_t>>& covenant_inputs)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Generate a tracking ID from first input (if any) or use a placeholder
    // Phase M.0: tracking_key is a string-based map key, not a txid type
    std::string tracking_key;
    if (!tx.vin.empty()) {
        tracking_key = tx.vin[0].prevout.txid.AsUint256().GetHex() + ":" + std::to_string(tx.vin[0].prevout.vout) + ":spend";
    } else {
        tracking_key = "coinbase:" + std::to_string(tx.vout.size());
    }
    std::vector<std::string> outpoints;

    for (const auto& input : covenant_inputs) {
        std::string outpoint = makeOutpoint(input.first, input.second);  // Phase M.0: input.first is now uint256

        // Check for conflicts
        auto it = covenant_spends_.find(outpoint);
        if (it != covenant_spends_.end()) {
            return false; // Already being spent
        }

        outpoints.push_back(outpoint);
        covenant_spends_[outpoint] = tracking_key;
    }

    tx_covenant_inputs_[tracking_key] = outpoints;
    return true;
}

void CovenantMempoolTracker::removeTransaction(const uint256& txid) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string txid_hex = txid.GetHex();  // Phase M.0: Convert to string for map lookup
    auto it = tx_covenant_inputs_.find(txid_hex);
    if (it != tx_covenant_inputs_.end()) {
        for (const auto& outpoint : it->second) {
            covenant_spends_.erase(outpoint);
        }
        tx_covenant_inputs_.erase(it);
    }
}

std::optional<uint256> CovenantMempoolTracker::getConflictingTx(
    const uint256& txid, uint32_t vout) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string outpoint = makeOutpoint(txid, vout);
    auto it = covenant_spends_.find(outpoint);
    if (it != covenant_spends_.end()) {
        return uint256::FromHex(it->second);  // Phase M.0: Convert hex string back to uint256
    }
    return std::nullopt;
}

bool CovenantMempoolTracker::hasContractPendingTransition(
    const std::string& contract_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return contract_transitions_.find(contract_id) != contract_transitions_.end();
}

std::vector<std::string> CovenantMempoolTracker::getContractPendingTransitions(
    const std::string& contract_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contract_transitions_.find(contract_id);
    if (it != contract_transitions_.end()) {
        return it->second;
    }
    return {};
}

void CovenantMempoolTracker::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    covenant_spends_.clear();
    contract_transitions_.clear();
    tx_covenant_inputs_.clear();
}

CovenantMempoolTracker::Stats CovenantMempoolTracker::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.tracked_transactions = tx_covenant_inputs_.size();
    stats.tracked_covenant_inputs = covenant_spends_.size();
    stats.tracked_contract_transitions = 0;

    for (const auto& pair : contract_transitions_) {
        stats.tracked_contract_transitions += pair.second.size();
    }

    return stats;
}

std::string CovenantMempoolTracker::makeOutpoint(
    const uint256& txid, uint32_t vout) const
{
    return txid.GetHex() + ":" + std::to_string(vout);  // Phase M.0: Convert to hex for string key
}

// ============================================================================
// CovenantMiningPolicy Implementation
// ============================================================================

double CovenantMiningPolicy::calculatePriority(
    const Transaction& tx,
    double base_priority,
    wallet::CovenantType type) const
{
    double multiplier = 1.0;

    switch (type) {
        case wallet::CovenantType::CTV:
            multiplier = ctv_priority_multiplier;
            break;
        case wallet::CovenantType::CSFS:
            multiplier = csfs_priority_multiplier;
            break;
        case wallet::CovenantType::CCV:
            multiplier = ccv_priority_multiplier;
            break;
        default:
            break;
    }

    return base_priority * multiplier;
}

bool CovenantMiningPolicy::canBatch(
    const Transaction& tx1,
    const Transaction& tx2,
    wallet::CovenantType type) const
{
    if (!enable_ctv_batching) return false;
    if (type != wallet::CovenantType::CTV) return false;

    // Simple batching heuristic: same number of outputs
    return tx1.vout.size() == tx2.vout.size();
}

} // namespace mempool
} // namespace dinero
