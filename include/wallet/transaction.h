#pragma once

// Phase M.1: Wallet uses primitives, never redefines
// This header is now a thin extension layer over primitives/transaction.h
// NO duplicate Transaction/TxInput/TxOutput definitions allowed

#include "primitives/transaction.h"

namespace dinero {
namespace wallet {

// ============================================================================
// Wallet-Specific Transaction Extensions
// ============================================================================
// These types extend primitives::Transaction with wallet-only metadata.
// They do NOT redefine core consensus types.

/**
 * Wallet metadata for a transaction
 * Separated from Transaction to maintain primitives purity
 */
struct WalletTxMetadata {
    uint64_t time_received = 0;      // Unix timestamp when tx entered wallet
    uint64_t time_confirmed = 0;     // Unix timestamp when tx confirmed (0 = unconfirmed)
    int32_t block_height = -1;       // Block height (-1 = mempool)
    std::string label;               // User-assigned label
    bool is_from_me = false;         // Did we create this transaction?
    bool is_to_me = false;           // Does this transaction send to our addresses?
};

/**
 * Wallet view of a transaction
 * Combines primitives::Transaction with wallet metadata
 */
struct WalletTransaction {
    Transaction tx;                  // Primitive transaction (consensus data)
    WalletTxMetadata meta;           // Wallet-specific metadata

    WalletTransaction() = default;
    explicit WalletTransaction(const Transaction& tx_) : tx(tx_) {}
};

// ============================================================================
// Wallet-Only Helpers (DO NOT belong in primitives)
// ============================================================================

/**
 * Check if output belongs to wallet
 * (Requires wallet keychain access - not a primitive operation)
 */
bool IsOutputMine(const TxOutput& output, const class HDWallet& wallet);

/**
 * Compute fee from wallet perspective
 * (May use wallet UTXO index - not available in primitives layer)
 */
bool ComputeWalletFee(const Transaction& tx, const class UTXOIndex& utxo_index, uint64_t& fee_out);

} // namespace wallet
} // namespace dinero
