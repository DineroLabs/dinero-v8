// SPDX-License-Identifier: MIT
// Phase W.4.2: RBF & CPFP Capability Detection - Implementation

#include "wallet/rbf_cpfp_detector.h"
#include "daemon/mempool.h"  // For getMempoolEntry, MempoolEntry
#include "wallet/wallet_manager.h"
#include "common/logger.h"

namespace dinero {

// ============================================================================
// Struct Constructors
// ============================================================================

RbfCapability::RbfCapability()
    : signals_rbf(false)
    , can_be_replaced(false)
    , min_replacement_fee(0)
    , rbf_status("unknown")
{
}

SpendableOutput::SpendableOutput()
    : txid()
    , vout(0)
    , amount(0)
    , is_confirmed(false)
    , is_wallet_controlled(false)
{
}

CpfpCapability::CpfpCapability()
    : viable(false)
    , outputs()
    , current_ancestor_count(0)
    , max_ancestor_count(DEFAULT_MAX_ANCESTORS)
    , within_package_limits(false)
    , cpfp_status("unknown")
{
}

RescueStrategy::RescueStrategy()
    : rbf_available(false)
    , cpfp_available(false)
    , recommended_action("none")
    , explanation("No rescue strategy available")
    , rbf_details()
    , cpfp_details()
{
}

// ============================================================================
// RbfCpfpDetector Constructor/Destructor
// ============================================================================

RbfCpfpDetector::RbfCpfpDetector()
    : max_ancestor_count_(DEFAULT_MAX_ANCESTORS)
    , min_relay_fee_rate_(DEFAULT_MIN_RELAY_FEE)
{
}

RbfCpfpDetector::~RbfCpfpDetector() {
}

// ============================================================================
// RBF Detection
// ============================================================================

RbfCapability RbfCpfpDetector::CheckRbfCapability(
    const uint256& txid,
    const Mempool* mempool
) const {
    RbfCapability capability;

    // Check if mempool is available
    if (!mempool) {
        capability.signals_rbf = false;
        capability.can_be_replaced = false;
        capability.min_replacement_fee = 0;
        capability.rbf_status = "Mempool unavailable";
        return capability;
    }

    // Query mempool for transaction entry
    auto entry_opt = mempool->getMempoolEntry(txid);
    bool tx_in_mempool = entry_opt.has_value();

    if (!tx_in_mempool) {
        capability.signals_rbf = false;
        capability.can_be_replaced = false;
        capability.min_replacement_fee = 0;
        capability.rbf_status = "Transaction not in mempool (may be confirmed)";
        return capability;
    }

    // Check if transaction signals RBF (BIP125)
    // Any input with sequence < 0xfffffffe signals RBF
    const auto& entry = *entry_opt;
    bool signals_rbf = false;
    for (const auto& vin : entry.tx.vin) {
        if (vin.sequence < 0xfffffffe) {
            signals_rbf = true;
            break;
        }
    }

    capability.signals_rbf = signals_rbf;
    capability.can_be_replaced = signals_rbf && tx_in_mempool;

    if (capability.can_be_replaced) {
        capability.min_replacement_fee = CalculateMinReplacementFee(txid, mempool);
        capability.rbf_status = "RBF enabled, can be replaced";
    } else if (!signals_rbf) {
        capability.rbf_status = "RBF not signaled (nSequence >= 0xfffffffe)";
    } else {
        capability.rbf_status = "Transaction confirmed, cannot be replaced";
    }

    dinero::g_logger.debug("RBF capability check: " + capability.rbf_status);

    return capability;
}

bool RbfCpfpDetector::CanBeReplaced(
    const uint256& txid,
    const Mempool* mempool
) const {
    RbfCapability capability = CheckRbfCapability(txid, mempool);
    return capability.can_be_replaced;
}

uint64_t RbfCpfpDetector::CalculateMinReplacementFee(
    const uint256& txid,
    const Mempool* mempool
) const {
    if (!mempool) {
        return 0;
    }

    // Get original transaction fee from mempool
    auto entry_opt = mempool->getMempoolEntry(txid);
    if (!entry_opt) {
        return 0;  // Transaction not in mempool
    }

    const auto& entry = *entry_opt;

    // BIP125 rules:
    // - Replacement fee must be > original fee
    // - Must pay incremental relay fee (typically 1 una/vB)
    uint64_t original_fee = entry.fee;
    uint64_t tx_size = entry.tx_size > 0 ? entry.tx_size : 250;  // Fallback if size unavailable

    // Minimum increment: original fee + (tx_size * min_relay_fee_rate)
    uint64_t min_increment = tx_size * min_relay_fee_rate_;
    uint64_t min_fee = original_fee + min_increment;

    return min_fee;
}

// ============================================================================
// CPFP Detection
// ============================================================================

CpfpCapability RbfCpfpDetector::CheckCpfpCapability(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool
) const {
    CpfpCapability capability;
    capability.max_ancestor_count = max_ancestor_count_;

    // Check if transaction is in mempool
    if (!mempool) {
        capability.viable = false;
        capability.within_package_limits = false;
        capability.cpfp_status = "Mempool unavailable";
        return capability;
    }

    // Check if transaction exists in mempool
    auto entry_opt = mempool->getMempoolEntry(txid);
    bool tx_in_mempool = entry_opt.has_value();

    if (!tx_in_mempool) {
        capability.viable = false;
        capability.within_package_limits = false;
        capability.cpfp_status = "Transaction not in mempool (may be confirmed)";
        return capability;
    }

    // Check package limits
    capability.within_package_limits = IsWithinPackageLimits(txid, mempool);
    if (!capability.within_package_limits) {
        capability.viable = false;
        capability.cpfp_status = "Exceeds ancestor package limits (max: " +
                                std::to_string(max_ancestor_count_) + ")";
        return capability;
    }

    // Find spendable outputs
    capability.outputs = FindSpendableOutputs(txid, wallet);

    if (capability.outputs.empty()) {
        capability.viable = false;
        capability.cpfp_status = "No spendable outputs available for CPFP";
        return capability;
    }

    // CPFP is viable
    capability.viable = true;
    capability.cpfp_status = "CPFP viable (" +
                            std::to_string(capability.outputs.size()) +
                            " spendable output(s))";

    dinero::g_logger.debug("CPFP capability check: " + capability.cpfp_status);

    return capability;
}

std::vector<SpendableOutput> RbfCpfpDetector::FindSpendableOutputs(
    const uint256& txid,
    const WalletManager* wallet
) const {
    std::vector<SpendableOutput> spendable_outputs;

    if (!wallet) {
        return spendable_outputs;  // No wallet, no spendable outputs
    }

    // TODO: Query transaction outputs
    // For now, use placeholder logic

    // Placeholder: Assume transaction has 2 outputs, one is change (wallet-controlled)
    // In real implementation, this would query:
    // 1. Transaction outputs from mempool/chainstate
    // 2. Check which outputs wallet controls
    // 3. Check if outputs are already spent

    SpendableOutput output;
    output.txid = txid;
    output.vout = 1;  // Assume output 1 is change
    output.amount = 50000;  // 0.0005 BTC
    output.is_confirmed = false;
    output.is_wallet_controlled = true;

    spendable_outputs.push_back(output);

    return spendable_outputs;
}

bool RbfCpfpDetector::IsWithinPackageLimits(
    const uint256& txid,
    const Mempool* mempool
) const {
    if (!mempool) {
        return false;
    }

    // Query mempool for transaction entry
    auto entry_opt = mempool->getMempoolEntry(txid);
    if (!entry_opt) {
        return false;  // Not in mempool
    }

    const auto& entry = *entry_opt;

    // Ancestor count = depends.size() + 1 (tx itself)
    // Note: This counts direct parents only. Full ancestor chain traversal
    // would require recursive lookup or adding ancestor_count to MempoolEntry.
    // This is conservative - allows some edge cases that full counting would reject.
    uint32_t current_ancestors = static_cast<uint32_t>(entry.depends.size()) + 1;

    // Bitcoin Core limits: max 25 ancestors, max 101 KB ancestor size
    if (current_ancestors >= max_ancestor_count_) {
        dinero::g_logger.debug("Transaction exceeds ancestor limit: " +
                              std::to_string(current_ancestors) + " >= " +
                              std::to_string(max_ancestor_count_));
        return false;
    }

    return true;
}

// ============================================================================
// Rescue Strategy
// ============================================================================

RescueStrategy RbfCpfpDetector::DetermineRescueStrategy(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool
) const {
    RescueStrategy strategy;

    // Check RBF capability
    strategy.rbf_details = CheckRbfCapability(txid, mempool);
    strategy.rbf_available = strategy.rbf_details.can_be_replaced;

    // Check CPFP capability
    strategy.cpfp_details = CheckCpfpCapability(txid, wallet, mempool);
    strategy.cpfp_available = strategy.cpfp_details.viable;

    // Determine recommended action
    if (strategy.rbf_available) {
        strategy.recommended_action = "rbf";
        strategy.explanation = "RBF (Replace-By-Fee) is recommended. " +
                              strategy.rbf_details.rbf_status;
    } else if (strategy.cpfp_available) {
        strategy.recommended_action = "cpfp";
        strategy.explanation = "CPFP (Child-Pays-For-Parent) is recommended. " +
                              strategy.cpfp_details.cpfp_status;
    } else {
        strategy.recommended_action = "wait";
        strategy.explanation = "No fee bump options available. ";

        // Provide reason
        if (!strategy.rbf_details.signals_rbf) {
            strategy.explanation += "Transaction does not signal RBF. ";
        }
        if (strategy.cpfp_details.outputs.empty()) {
            strategy.explanation += "No spendable outputs for CPFP. ";
        }
        if (!strategy.cpfp_details.within_package_limits) {
            strategy.explanation += "Exceeds ancestor limits. ";
        }

        strategy.explanation += "Wait for confirmation or submit new transaction.";
    }

    dinero::g_logger.debug("Rescue strategy: " + strategy.recommended_action +
                          " - " + strategy.explanation);

    return strategy;
}

// ============================================================================
// Configuration
// ============================================================================

void RbfCpfpDetector::SetMaxAncestorCount(uint32_t max_ancestors) {
    max_ancestor_count_ = max_ancestors;
}

uint32_t RbfCpfpDetector::GetMaxAncestorCount() const {
    return max_ancestor_count_;
}

} // namespace dinero
