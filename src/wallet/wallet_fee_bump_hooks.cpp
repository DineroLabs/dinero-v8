// SPDX-License-Identifier: MIT
// Phase W.4.5: Wallet Fee Bump UX Hooks Implementation

#include "wallet/wallet_fee_bump_hooks.h"
#include "wallet/wallet_manager.h"
#include "wallet/rbf_cpfp_detector.h"
#include "mining/tx_inclusion_analyzer.h"
#include "wallet/fee_bump_engine.h"
#include "mempool/mempool.h"
#include "mining/block_assembler.h"
#include "rpc/ergonomics_rpc_handlers.h"  // For NodeHealth
#include "common/logger.h"
#include <sstream>

using namespace dinero;

// ============================================================================
// Global Component Access (Singleton Pattern)
// ============================================================================

namespace {

/**
 * @brief Get global TxInclusionAnalyzer instance
 */
TxInclusionAnalyzer& GetTxInclusionAnalyzer() {
    static TxInclusionAnalyzer instance;
    return instance;
}

/**
 * @brief Get global RbfCpfpDetector instance
 */
RbfCpfpDetector& GetRbfCpfpDetector() {
    static RbfCpfpDetector instance;
    return instance;
}

/**
 * @brief Get global FeeBumpEngine instance
 */
FeeBumpEngine& GetFeeBumpEngine() {
    static FeeBumpEngine instance;
    return instance;
}

/**
 * @brief Get unconfirmed transactions from wallet
 *
 * In a real implementation, this would query the wallet's transaction
 * history for transactions with confirmations == 0. For now, returns
 * empty placeholder list.
 *
 * @param wallet Wallet to query
 * @return Vector of unconfirmed transaction IDs
 */
std::vector<uint256> GetUnconfirmedTransactions(const WalletManager* wallet) {
    // TODO: Wire up actual wallet transaction query
    // For now, return empty list (placeholder)

    // Real implementation would do:
    // auto tx_history = wallet->getTransactionHistory();
    // for (auto& tx : tx_history) {
    //     if (tx.confirmations == 0) {
    //         result.push_back(uint256::FromHexUnsafe(tx.txid));
    //     }
    // }

    return std::vector<uint256>();
}

} // anonymous namespace

// ============================================================================
// WalletFeeBumpHooks Implementation
// ============================================================================

WalletFeeBumpHooks::WalletBumpSummary WalletFeeBumpHooks::GetWalletBumpSummary(
    const WalletManager* wallet,
    const Mempool* mempool
) const {
    dinero::g_logger.debug("WalletFeeBumpHooks: Getting wallet bump summary");

    WalletBumpSummary summary;

    if (!wallet) {
        dinero::g_logger.warn("WalletFeeBumpHooks: Null wallet pointer");
        return summary;
    }

    // Get all unconfirmed transactions from wallet
    std::vector<uint256> unconfirmed_txs = GetUnconfirmedTransactions(wallet);
    summary.total_unconfirmed = static_cast<uint32_t>(unconfirmed_txs.size());

    if (unconfirmed_txs.empty()) {
        dinero::g_logger.debug("WalletFeeBumpHooks: No unconfirmed transactions");
        return summary;
    }

    // Analyze each transaction
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    RbfCpfpDetector& detector = GetRbfCpfpDetector();

    for (const auto& txid : unconfirmed_txs) {
        // Get inclusion status
        TxInclusionStatus status = analyzer.Analyze(
            txid,
            mempool,
            nullptr,  // BlockAssembler
            nullptr   // NodeHealth
        );

        // Check RBF/CPFP capabilities
        RbfCapability rbf_cap = detector.CheckRbfCapability(txid, mempool);
        CpfpCapability cpfp_cap = detector.CheckCpfpCapability(txid, wallet, mempool);

        // Categorize by inclusion state
        if (status.state == InclusionState::LIKELY) {
            summary.likely_to_confirm++;
        } else if (status.state == InclusionState::STALLED) {
            summary.stalled++;
            summary.needs_attention.push_back(txid);
        } else if (status.state == InclusionState::BLOCKED) {
            summary.blocked++;
            summary.needs_attention.push_back(txid);
        }

        // Count bump capabilities
        if (rbf_cap.can_be_replaced) {
            summary.can_rbf++;
        }
        if (cpfp_cap.viable) {
            summary.can_cpfp++;
        }
    }

    dinero::g_logger.debug("WalletFeeBumpHooks: Summary complete - " +
                          std::to_string(summary.needs_attention.size()) +
                          " transactions need attention");

    return summary;
}

std::vector<uint256> WalletFeeBumpHooks::GetTransactionsNeedingBump(
    const WalletManager* wallet,
    const Mempool* mempool,
    uint32_t target_blocks
) const {
    dinero::g_logger.debug("WalletFeeBumpHooks: Getting transactions needing bump (target: " +
                          std::to_string(target_blocks) + " blocks)");

    std::vector<uint256> needs_bump;

    if (!wallet) {
        dinero::g_logger.warn("WalletFeeBumpHooks: Null wallet pointer");
        return needs_bump;
    }

    // Get all unconfirmed transactions
    std::vector<uint256> unconfirmed_txs = GetUnconfirmedTransactions(wallet);

    if (unconfirmed_txs.empty()) {
        return needs_bump;
    }

    // Analyze each transaction
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    FeeBumpEngine& engine = GetFeeBumpEngine();

    for (const auto& txid : unconfirmed_txs) {
        // Get inclusion status
        TxInclusionStatus status = analyzer.Analyze(
            txid,
            mempool,
            nullptr,  // BlockAssembler
            nullptr   // NodeHealth
        );

        // Check if transaction would benefit from bump to reach target
        uint32_t estimated_blocks = engine.EstimateConfirmationBlocks(
            status.effective_feerate,
            mempool
        );

        // Transaction needs bump if:
        // 1. It's stalled or blocked, OR
        // 2. Estimated confirmation time exceeds target
        bool is_stuck = (status.state == InclusionState::STALLED ||
                        status.state == InclusionState::BLOCKED);
        bool too_slow = (estimated_blocks > target_blocks);

        if (is_stuck || too_slow) {
            needs_bump.push_back(txid);
        }
    }

    dinero::g_logger.debug("WalletFeeBumpHooks: " +
                          std::to_string(needs_bump.size()) +
                          " transactions need bump");

    return needs_bump;
}

TxInclusionStatus WalletFeeBumpHooks::CheckTransactionStatus(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool
) const {
    dinero::g_logger.debug("WalletFeeBumpHooks: Checking status for tx " +
                          txid.ToString().substr(0, 16) + "...");

    // Get inclusion analysis
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    TxInclusionStatus status = analyzer.Analyze(
        txid,
        mempool,
        nullptr,  // BlockAssembler
        nullptr   // NodeHealth
    );

    // Add wallet-specific RBF/CPFP availability
    RbfCpfpDetector& detector = GetRbfCpfpDetector();
    RbfCapability rbf_cap = detector.CheckRbfCapability(txid, mempool);
    CpfpCapability cpfp_cap = detector.CheckCpfpCapability(txid, wallet, mempool);

    status.rbf_available = rbf_cap.can_be_replaced;
    status.cpfp_available = cpfp_cap.viable;

    return status;
}

FeeBumpRecommendation WalletFeeBumpHooks::GetBumpRecommendation(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool,
    uint32_t target_blocks
) const {
    dinero::g_logger.debug("WalletFeeBumpHooks: Getting bump recommendation for tx " +
                          txid.ToString().substr(0, 16) + "... (target: " +
                          std::to_string(target_blocks) + " blocks)");

    // Step 1: Get inclusion status
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    TxInclusionStatus inclusion_status = analyzer.Analyze(
        txid,
        mempool,
        nullptr,  // BlockAssembler
        nullptr   // NodeHealth
    );

    // Step 2: Get rescue strategy (RBF/CPFP capabilities)
    RbfCpfpDetector& detector = GetRbfCpfpDetector();
    RescueStrategy rescue_strategy = detector.DetermineRescueStrategy(
        txid,
        wallet,
        mempool
    );

    // Step 3: Generate recommendation
    FeeBumpEngine& engine = GetFeeBumpEngine();
    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid,
        inclusion_status,
        rescue_strategy,
        mempool,
        wallet,
        target_blocks
    );

    return recommendation;
}

bool WalletFeeBumpHooks::CanBumpTransaction(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool
) const {
    if (!wallet) {
        return false;
    }

    RbfCpfpDetector& detector = GetRbfCpfpDetector();

    // Check RBF capability
    RbfCapability rbf_cap = detector.CheckRbfCapability(txid, mempool);
    if (rbf_cap.can_be_replaced) {
        return true;
    }

    // Check CPFP capability
    CpfpCapability cpfp_cap = detector.CheckCpfpCapability(txid, wallet, mempool);
    if (cpfp_cap.viable) {
        return true;
    }

    return false;
}

bool WalletFeeBumpHooks::NeedsAttention(
    const uint256& txid,
    const WalletManager* wallet,
    const Mempool* mempool,
    uint32_t target_blocks
) const {
    if (!wallet) {
        return false;
    }

    // Get inclusion status
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    TxInclusionStatus status = analyzer.Analyze(
        txid,
        mempool,
        nullptr,  // BlockAssembler
        nullptr   // NodeHealth
    );

    // Transaction needs attention if:
    // 1. It's stalled or blocked (unlikely to confirm)
    bool is_stuck = (status.state == InclusionState::STALLED ||
                    status.state == InclusionState::BLOCKED);

    if (is_stuck) {
        return true;
    }

    // 2. Estimated confirmation time exceeds target
    FeeBumpEngine& engine = GetFeeBumpEngine();
    uint32_t estimated_blocks = engine.EstimateConfirmationBlocks(
        status.effective_feerate,
        mempool
    );

    if (estimated_blocks > target_blocks) {
        return true;
    }

    return false;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string dinero::WalletBumpSummaryToString(
    const WalletFeeBumpHooks::WalletBumpSummary& summary
) {
    std::ostringstream oss;
    oss << "WalletBumpSummary{";
    oss << "total_unconfirmed=" << summary.total_unconfirmed;
    oss << ", likely=" << summary.likely_to_confirm;
    oss << ", stalled=" << summary.stalled;
    oss << ", blocked=" << summary.blocked;
    oss << ", can_rbf=" << summary.can_rbf;
    oss << ", can_cpfp=" << summary.can_cpfp;
    oss << ", needs_attention=" << summary.needs_attention.size();
    oss << "}";
    return oss.str();
}
