#include "policy/mempool_policy.h"
#include "wallet/transaction.h"
#include "common/logger.h"
#include <algorithm>
#include <cmath>

namespace dinero {
namespace policy {

MempoolPolicy::MempoolPolicy(const MempoolConfig& config) : config_(config) {}

ValidationResult MempoolPolicy::checkStandardness(const Transaction& tx) const {
    // v7: only transparent (v1, v2) and shielded txs are standard.
    // v3 (ring) and v4 (ring-covenant) were excised on Apr 17 2026.
    if (tx.version != 1 && tx.version != 2 &&
        !Transaction::IsShieldedVersion(tx.version)) {
        return ValidationResult::INVALID_SCRIPT;
    }

    // Check outputs for standard script types
    for (const auto& output : tx.vout) {
        // Empty scripts are not standard
        if (output.scriptPubKey.empty()) {
            return ValidationResult::INVALID_SCRIPT;
        }

        // Check if output script is a standard type
        if (!isStandardScript(output.scriptPubKey)) {
            return ValidationResult::INVALID_SCRIPT;
        }

        // Zero-value outputs are not standard (except OP_RETURN)
        // Phase M.6.2: Compare with AmountUna::Zero()
        if (output.value == AmountUna::Zero() && output.scriptPubKey[0] != 0x6a) {  // 0x6a = OP_RETURN
            return ValidationResult::INVALID_DUST;
        }
    }

    return ValidationResult::VALID;
}

ValidationResult MempoolPolicy::checkSizeAndWeight(const Transaction& tx) const {
    // BIP141-compliant size and weight validation
    // Total size (with witness data)
    uint32_t tx_size = tx.GetSize();
    if (tx_size > config_.max_tx_size) {
        return ValidationResult::INVALID_SIZE;
    }

    // BIP141 weight: (base_size * 3) + total_size
    // This gives witness data a 75% discount
    uint32_t tx_weight = tx.GetWeight();
    if (tx_weight > config_.max_tx_weight) {
        return ValidationResult::INVALID_WEIGHT;
    }

    // Virtual size (for fee calculation): (weight + 3) / 4
    [[maybe_unused]] uint32_t tx_vsize = tx.GetVirtualSize();
    // Optional: enforce minimum vsize to prevent dust spam
    // if (tx_vsize < config_.min_tx_vsize) {
    //     return ValidationResult::TOO_SMALL;
    // }

    return ValidationResult::VALID;
}

ValidationResult MempoolPolicy::checkSigOps(const Transaction& tx) const {
    uint32_t sigops = 0;

    // Calculate serialized size for rate limiting
    auto serialized = tx.Serialize();
    uint32_t tx_size = serialized.size(); // Size in bytes
    if (tx_size == 0) {
        return ValidationResult::INVALID_SCRIPT;
    }
    
    // Count signature operations (simplified)
    sigops += static_cast<uint32_t>(tx.vout.size()); // 1 sigop per output (simplified)
    
    if (sigops > config_.max_sigops) {
        return ValidationResult::INVALID_SIGOPS;
    }
    
    return ValidationResult::VALID;
}

ValidationResult MempoolPolicy::checkFees(const Transaction& tx, uint64_t fee) const {
    auto serialized = tx.Serialize();
    uint32_t tx_size = serialized.size(); // Size in bytes
    uint64_t min_fee = (config_.min_relay_fee_rate * tx_size) / 1000;
    
    if (fee < min_fee) {
        return ValidationResult::INVALID_FEE;
    }
    
    return ValidationResult::VALID;
}

ValidationResult MempoolPolicy::checkDust(const Transaction& tx) const {
    for (const auto& output : tx.vout) {
        if (isDustOutput(output)) {
            return ValidationResult::INVALID_DUST;
        }
    }
    
    return ValidationResult::VALID;
}

// RBF validation removed - use RBFPolicy for BIP125-compliant RBF checking
// (Previously had placeholder fee logic that was never called)

ValidationResult MempoolPolicy::checkCPFP(const Transaction& tx,
                                          uint32_t ancestor_count,
                                          uint32_t descendant_count,
                                          uint64_t ancestor_size,
                                          uint64_t descendant_size) const {
    if (!config_.enable_cpfp) {
        return ValidationResult::VALID;
    }
    
    if (ancestor_count > config_.max_ancestor_count) {
        return ValidationResult::INVALID_SIZE;
    }
    
    if (descendant_count > config_.max_descendant_count) {
        return ValidationResult::INVALID_SIZE;
    }
    
    if (ancestor_size > config_.max_ancestor_size) {
        return ValidationResult::INVALID_SIZE;
    }
    
    if (descendant_size > config_.max_descendant_size) {
        return ValidationResult::INVALID_SIZE;
    }
    
    return ValidationResult::VALID;
}

ValidationResult MempoolPolicy::validateTransaction(const Transaction& tx, uint64_t fee) const {
    // Run all validation checks
    ValidationResult result;
    
    result = checkStandardness(tx);
    if (result != ValidationResult::VALID) return result;
    
    result = checkSizeAndWeight(tx);
    if (result != ValidationResult::VALID) return result;
    
    result = checkSigOps(tx);
    if (result != ValidationResult::VALID) return result;
    
    result = checkFees(tx, fee);
    if (result != ValidationResult::VALID) return result;
    
    result = checkDust(tx);
    if (result != ValidationResult::VALID) return result;
    
    return ValidationResult::VALID;
}

std::string MempoolPolicy::getErrorMessage(ValidationResult result) const {
    switch (result) {
        case ValidationResult::VALID:
            return "Transaction is valid";
        case ValidationResult::INVALID_SIZE:
            return "Transaction size exceeds limit";
        case ValidationResult::INVALID_WEIGHT:
            return "Transaction weight exceeds limit";
        case ValidationResult::INVALID_SIGOPS:
            return "Transaction has too many signature operations";
        case ValidationResult::INVALID_FEE:
            return "Transaction fee is insufficient";
        case ValidationResult::INVALID_SCRIPT:
            return "Transaction contains non-standard script";
        case ValidationResult::INVALID_DUST:
            return "Transaction creates dust output";
        case ValidationResult::INVALID_RBF:
            return "Replace-by-fee policy violation";
        case ValidationResult::ALREADY_IN_MEMPOOL:
            return "Transaction already in mempool";
        case ValidationResult::CONFLICTING_TX:
            return "Transaction conflicts with existing mempool entry";
        default:
            return "Unknown validation error";
    }
}

// Private helper methods

uint32_t MempoolPolicy::calculateTxWeight(const Transaction& tx) const {
    // BIP141-compliant weight calculation
    // Weight = (base_size * 3) + total_size
    // This is implemented in Transaction::GetWeight()
    return tx.GetWeight();
}

uint32_t MempoolPolicy::calculateSigOps(const Transaction& tx) const {
    uint32_t sigops = 0;
    
    // Count signature operations in inputs and outputs
    sigops += static_cast<uint32_t>(tx.vin.size()); // 1 sigop per input (simplified)
    
    for (const auto& output : tx.vout) {
        // Check for multisig outputs
        if (output.scriptPubKey.size() > 0) {
            // Simplified - would need proper script parsing
            sigops += 1;
        }
    }
    
    return sigops;
}

bool MempoolPolicy::isStandardScript(const std::vector<uint8_t>& script) const {
    if (script.empty()) return false;
    if (script.size() > config_.max_script_size) return false;

    // Standard script opcodes (from Bitcoin Core)
    constexpr uint8_t OP_DUP = 0x76;
    constexpr uint8_t OP_HASH160 = 0xa9;
    constexpr uint8_t OP_EQUAL = 0x87;
    constexpr uint8_t OP_EQUALVERIFY = 0x88;
    constexpr uint8_t OP_CHECKSIG = 0xac;
    constexpr uint8_t OP_0 = 0x00;
    constexpr uint8_t OP_1 = 0x51;
    constexpr uint8_t OP_RETURN = 0x6a;

    // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    if (script.size() == 25 &&
        script[0] == OP_DUP &&
        script[1] == OP_HASH160 &&
        script[2] == 20 &&
        script[23] == OP_EQUALVERIFY &&
        script[24] == OP_CHECKSIG) {
        return true;
    }

    // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
    if (script.size() == 23 &&
        script[0] == OP_HASH160 &&
        script[1] == 20 &&
        script[22] == OP_EQUAL) {
        return true;
    }

    // P2WPKH: OP_0 <20 bytes>
    if (script.size() == 22 &&
        script[0] == OP_0 &&
        script[1] == 20) {
        return true;
    }

    // P2WSH: OP_0 <32 bytes>
    if (script.size() == 34 &&
        script[0] == OP_0 &&
        script[1] == 32) {
        return true;
    }

    // P2TR (Taproot): OP_1 <32 bytes>
    if (script.size() == 34 &&
        script[0] == OP_1 &&
        script[1] == 32) {
        return true;
    }

    // OP_RETURN (data carrier): OP_RETURN <data>
    // Standard OP_RETURN must be ≤ 80 bytes total
    if (script.size() >= 1 && script[0] == OP_RETURN) {
        if (script.size() <= 83) {  // OP_RETURN + OP_PUSHDATA + 80 bytes
            return true;
        }
    }

    // Reject all other script types as non-standard
    return false;
}

bool MempoolPolicy::isDustOutput(const TxOutput& output) const {
    // Phase M.6.2: Compare AmountUna with threshold
    return output.value < AmountUna::Una(config_.dust_threshold);
}

bool MempoolPolicy::hasRBFSignal(const Transaction& tx) const {
    // Check if any input has sequence number < 0xfffffffe
    for (const auto& input : tx.vin) {
        if (input.sequence < 0xfffffffe) {
            return true;
        }
    }
    return false;
}

// EvictionPolicy implementation

std::vector<std::string> EvictionPolicy::selectForEviction(
    const std::vector<EvictionCandidate>& candidates,
    uint64_t bytes_to_free) const {
    
    std::vector<std::pair<double, std::string>> scored_candidates;
    
    // Score all candidates
    for (const auto& candidate : candidates) {
        double score = calculateEvictionScore(candidate);
        scored_candidates.emplace_back(score, candidate.txid);
    }
    
    // Sort by score (lowest first - most likely to evict)
    std::sort(scored_candidates.begin(), scored_candidates.end());
    
    // Select transactions until we free enough bytes
    std::vector<std::string> to_evict;
    uint64_t bytes_freed = 0;
    
    for (const auto& candidate : scored_candidates) {
        to_evict.push_back(candidate.second);
        bytes_freed += 250; // Estimate 250 bytes per transaction
        
        if (bytes_freed >= 1024) { // Free at least 1KB
            break;
        }
    }
    
    return to_evict;
}

double EvictionPolicy::calculateEvictionScore(const EvictionCandidate& candidate) const {
    // Lower score = more likely to be evicted
    // Factors: fee rate (higher is better), time (older is worse), descendants (more is worse)
    
    double fee_score = static_cast<double>(candidate.fee_rate) / 1000.0; // Normalize
    double time_score = 1.0 / (1.0 + candidate.time_added / 3600.0); // Older = lower score
    double descendant_penalty = candidate.descendant_count * 0.1;
    double parent_penalty = candidate.has_unconfirmed_parents ? 0.5 : 0.0;
    
    return fee_score - time_score - descendant_penalty - parent_penalty;
}

} // namespace policy
} // namespace dinero
