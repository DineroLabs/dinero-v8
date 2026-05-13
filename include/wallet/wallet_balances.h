#pragma once

#include <sqlite3.h>
#include "compat/jsoncpp_compat.h"
#include <vector>
#include <optional>
#include <cstdint>

namespace dinero {

struct WalletBalances {
    long long confirmed = 0;
    long long immature  = 0;
    long long unconfirmed = 0;
    long long total() const { return confirmed + immature + unconfirmed; }
};

/**
 * @brief Compute wallet balances from the wallet database
 * @param db Opened wallet database connection
 * @param tipHeight Current blockchain tip height
 * @param out Output balances structure
 * @return true on success, false on error
 */
bool compute_wallet_balances(sqlite3* db, int tipHeight, WalletBalances& out);

/**
 * @brief Convert wallet balances to JSON format
 * @param b Wallet balances structure
 * @return JSON object with balance fields
 */
Json::Value balances_to_json(const WalletBalances& b);

/**
 * @brief Migrate wallet database to include balance tracking tables
 * @param db Opened wallet database connection
 * @return true on success, false on error
 */
bool migrate_wallet_balances(sqlite3* db);

/**
 * @brief Add a transaction to wallet with crash-safe atomic updates
 * @param db Opened wallet database connection
 * @param txid Transaction ID (32 bytes)
 * @param height Block height (NULL for mempool)
 * @param is_coinbase Whether this is a coinbase transaction
 * @param outputs List of outputs we own with values and scripts
 * @param coinbase_maturity_height Height when coinbase becomes spendable (0 if not coinbase)
 * @return true on success, false on error
 */
bool add_wallet_transaction(sqlite3* db, 
                           const std::vector<uint8_t>& txid,
                           std::optional<int> height,
                           bool is_coinbase,
                           const std::vector<std::pair<uint32_t, int64_t>>& outputs,
                           int coinbase_maturity_height = 0);

/**
 * @brief Remove transactions from wallet (for reorg handling)
 * @param db Opened wallet database connection  
 * @param min_height Remove all transactions at or above this height
 * @return true on success, false on error
 */
bool remove_wallet_transactions_from_height(sqlite3* db, int min_height);

// Coinbase maturity constant (100 blocks like Bitcoin)
constexpr int COINBASE_MATURITY = 100;

} // namespace dinero
