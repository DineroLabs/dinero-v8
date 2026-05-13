/**
 * Unified Balance Service Implementation
 *
 * Provides combined view of transparent and confidential balances.
 *
 * NOTE: This implementation provides compilable stubs. Full integration with
 * existing UTXOIndex and balance calculation APIs is TODO.
 */

#include "wallet/unified_balance_service.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"

#include <algorithm>

namespace dinero {
namespace wallet {

// ============================================================================
// Constructor / Destructor
// ============================================================================

UnifiedBalanceService::UnifiedBalanceService(WalletManager* wallet_manager)
    : wallet_manager_(wallet_manager)
{
}

UnifiedBalanceService::~UnifiedBalanceService() = default;

// ============================================================================
// Balance Queries
// ============================================================================

UnifiedBalance UnifiedBalanceService::GetBalance(int min_confirmations) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    if (cache_valid_.load() && min_confirmations == min_confirmations_) {
        return cached_balance_;
    }

    UnifiedBalance balance;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return balance;
    }

    // TODO: Integrate with UTXOIndex for actual balance calculation
    // For now, return empty balance - the abstraction is ready
    // but full integration with existing balance APIs is needed.
    //
    // The existing balance calculation uses:
    // - UTXOIndex::GetBalance() for transparent
    // - UTXOIndex::GetConfidentialUTXOs() for CT
    // - Coinbase maturity checks (100 confirmations)

    g_logger.debug("UnifiedBalanceService::GetBalance called with min_conf=" +
                   std::to_string(min_confirmations));

    // Update cache
    cached_balance_ = balance;
    cache_valid_.store(true);

    return balance;
}

AddressBalance UnifiedBalanceService::GetAddressBalance(const std::string& address) const {
    AddressBalance balance;
    balance.address = address;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return balance;
    }

    // TODO: Integrate with UTXOIndex for address-specific balance
    g_logger.debug("UnifiedBalanceService::GetAddressBalance called for " + address);

    return balance;
}

std::vector<AddressBalance> UnifiedBalanceService::GetAllAddressBalances() const {
    std::vector<AddressBalance> balances;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return balances;
    }

    // TODO: Integrate with UTXOIndex for all address balances
    g_logger.debug("UnifiedBalanceService::GetAllAddressBalances called");

    return balances;
}

uint64_t UnifiedBalanceService::GetTransparentBalance(int min_confirmations) const {
    auto balance = GetBalance(min_confirmations);
    return balance.transparent_confirmed;
}

uint64_t UnifiedBalanceService::GetConfidentialBalance(int min_confirmations) const {
    auto balance = GetBalance(min_confirmations);
    return balance.confidential_confirmed;
}

uint64_t UnifiedBalanceService::GetSpendableBalance() const {
    auto balance = GetBalance(min_confirmations_);
    return balance.total_spendable - balance.locked_balance;
}

// ============================================================================
// UTXO Information
// ============================================================================

std::vector<UnifiedBalanceService::UTXOInfo> UnifiedBalanceService::GetSpendableUTXOs(
    bool include_confidential,
    int min_confirmations
) const {
    std::vector<UTXOInfo> utxos;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return utxos;
    }

    // TODO: Integrate with UTXOIndex for UTXO enumeration
    g_logger.debug("UnifiedBalanceService::GetSpendableUTXOs called ct=" +
                   std::to_string(include_confidential) +
                   " min_conf=" + std::to_string(min_confirmations));

    return utxos;
}

UnifiedBalanceService::UTXOCounts UnifiedBalanceService::GetUTXOCounts() const {
    UTXOCounts counts;

    // TODO: Integrate with UTXOIndex for UTXO counts
    g_logger.debug("UnifiedBalanceService::GetUTXOCounts called");

    return counts;
}

// ============================================================================
// Balance Notifications
// ============================================================================

uint64_t UnifiedBalanceService::OnBalanceChange(BalanceChangeCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    uint64_t id = next_subscription_id_.fetch_add(1);
    subscribers_.emplace_back(id, std::move(callback));
    return id;
}

void UnifiedBalanceService::Unsubscribe(uint64_t subscription_id) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
            [subscription_id](const auto& pair) {
                return pair.first == subscription_id;
            }),
        subscribers_.end()
    );
}

void UnifiedBalanceService::Refresh() {
    cache_valid_.store(false);
    NotifyBalanceChange("refresh");
}

// ============================================================================
// Chain State Integration
// ============================================================================

void UnifiedBalanceService::OnNewBlock(uint32_t block_height, const std::string& /*block_hash*/) {
    cache_valid_.store(false);
    NotifyBalanceChange("new_block", block_height);
}

void UnifiedBalanceService::OnMempoolTx(const std::string& txid, bool is_incoming) {
    cache_valid_.store(false);
    NotifyBalanceChange(is_incoming ? "incoming_tx" : "outgoing_tx", 0, txid);
}

void UnifiedBalanceService::OnReorg(uint32_t /*old_height*/, uint32_t new_height) {
    cache_valid_.store(false);
    NotifyBalanceChange("reorg", new_height);
}

// ============================================================================
// Private Methods
// ============================================================================

void UnifiedBalanceService::NotifyBalanceChange(
    const std::string& trigger,
    uint32_t height,
    const std::string& txid
) {
    // Get new balance
    UnifiedBalance new_balance = GetBalance(min_confirmations_);

    // Create event
    BalanceChangeEvent event;
    event.previous = cached_balance_;
    event.current = new_balance;
    event.transparent_change = static_cast<int64_t>(new_balance.transparent_confirmed) -
                                static_cast<int64_t>(event.previous.transparent_confirmed);
    event.confidential_change = static_cast<int64_t>(new_balance.confidential_confirmed) -
                                 static_cast<int64_t>(event.previous.confidential_confirmed);
    event.trigger = trigger;
    event.block_height = height;
    event.txid = txid;

    // Notify subscribers
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for (const auto& [id, callback] : subscribers_) {
        try {
            callback(event);
        } catch (const std::exception& e) {
            g_logger.error("Balance change callback failed: " + std::string(e.what()));
        }
    }
}

void UnifiedBalanceService::UpdateCache() {
    cache_valid_.store(false);
    GetBalance(min_confirmations_);
}

} // namespace wallet
} // namespace dinero
