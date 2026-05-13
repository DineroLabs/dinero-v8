#pragma once

/**
 * @file wallet_utxo_state.h
 * @brief Formal UTXO state model for wallet (Milestone 12.2)
 *
 * Every wallet UTXO must have exactly one state.
 * State transitions are explicit and validated.
 */

#include <string>
#include <optional>
#include <cstdint>

namespace dinero {
namespace wallet {

/**
 * @brief Wallet UTXO lifecycle states
 *
 * A UTXO progresses through these states as it moves through
 * the blockchain and mempool.
 */
enum class WalletUTXOState {
    /**
     * CONFIRMED: UTXO has >= 1 confirmation
     * - Included in a block
     * - Safe to spend (meets standard confirmation requirements)
     * - Not spent by any known transaction
     */
    CONFIRMED,

    /**
     * UNCONFIRMED: UTXO exists but has 0 confirmations
     * - Transaction is in mempool (not mined yet)
     * - May or may not be safe to spend (policy decision)
     * - Subject to RBF replacement
     */
    UNCONFIRMED,

    /**
     * SPENT_LOCAL: UTXO spent by our own transaction
     * - Used as input in a transaction we created
     * - Spending tx may be unconfirmed (allows RBF tracking)
     * - Spending tx may be confirmed (final spend)
     */
    SPENT_LOCAL,

    /**
     * CONFLICTED: UTXO invalidated by double-spend
     * - Another transaction spent the same input
     * - Our transaction was replaced (RBF)
     * - Chain reorg invalidated our transaction
     * - Cannot be spent (terminal state until cleanup)
     */
    CONFLICTED,

    /**
     * LOCKED: UTXO manually locked by user
     * - Reserved for specific purpose (e.g., CoinJoin)
     * - Coin selection must skip this UTXO
     * - Can be unlocked explicitly
     * - Still confirmed/unconfirmed underneath
     */
    LOCKED
};

/**
 * @brief Convert state to string for logging/debugging
 */
inline const char* WalletUTXOStateToString(WalletUTXOState state) {
    switch (state) {
        case WalletUTXOState::CONFIRMED:    return "CONFIRMED";
        case WalletUTXOState::UNCONFIRMED:  return "UNCONFIRMED";
        case WalletUTXOState::SPENT_LOCAL:  return "SPENT_LOCAL";
        case WalletUTXOState::CONFLICTED:   return "CONFLICTED";
        case WalletUTXOState::LOCKED:       return "LOCKED";
        default:                            return "UNKNOWN";
    }
}

/**
 * @brief UTXO state machine - validates state transitions
 *
 * Prevents invalid state changes and ensures wallet consistency.
 */
class WalletUTXOStateMachine {
public:
    /**
     * @brief Check if state transition is valid
     * @param from Current state
     * @param to Desired state
     * @return true if transition is allowed
     */
    static bool canTransition(WalletUTXOState from, WalletUTXOState to);

    /**
     * @brief Get human-readable reason for invalid transition
     * @param from Current state
     * @param to Desired state
     * @return Error message if invalid, empty if valid
     */
    static std::string getTransitionError(WalletUTXOState from, WalletUTXOState to);
};

/**
 * @brief Extended UTXO metadata for state tracking
 *
 * Stores information needed to manage UTXO lifecycle.
 */
struct WalletUTXOMetadata {
    WalletUTXOState state;                      // Current state
    std::optional<std::string> spending_txid;   // TXID that spent this (if SPENT_LOCAL)
    uint32_t confirmations;                     // Confirmation depth (0 = unconfirmed)
    bool is_coinbase;                           // Maturity rules apply
    uint32_t ancestor_count;                    // Cached from mempool (if unconfirmed)

    WalletUTXOMetadata()
        : state(WalletUTXOState::UNCONFIRMED)
        , confirmations(0)
        , is_coinbase(false)
        , ancestor_count(0)
    {}
};

} // namespace wallet
} // namespace dinero
