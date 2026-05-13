#include "wallet/wallet_utxo_state.h"

namespace dinero {
namespace wallet {

bool WalletUTXOStateMachine::canTransition(WalletUTXOState from, WalletUTXOState to) {
    // Same state is always allowed (idempotent)
    if (from == to) {
        return true;
    }

    using State = WalletUTXOState;

    switch (from) {
        case State::UNCONFIRMED:
            // Unconfirmed can transition to:
            // - CONFIRMED (mined in block)
            // - SPENT_LOCAL (we spent it)
            // - CONFLICTED (replaced by RBF / reorg)
            // - LOCKED (user manually locks)
            return to == State::CONFIRMED ||
                   to == State::SPENT_LOCAL ||
                   to == State::CONFLICTED ||
                   to == State::LOCKED;

        case State::CONFIRMED:
            // Confirmed can transition to:
            // - SPENT_LOCAL (we spent it)
            // - CONFLICTED (chain reorg invalidated)
            // - LOCKED (user manually locks)
            // NOTE: Cannot go back to UNCONFIRMED (would indicate reorg bug)
            return to == State::SPENT_LOCAL ||
                   to == State::CONFLICTED ||
                   to == State::LOCKED;

        case State::SPENT_LOCAL:
            // Spent can transition to:
            // - CONFLICTED (our spend was replaced / reorg)
            // - UNCONFIRMED (spending tx evicted from mempool - rare but possible)
            // - CONFIRMED (if spending tx was unconfirmed, then confirmed)
            // NOTE: This handles RBF replacements
            return to == State::CONFLICTED ||
                   to == State::UNCONFIRMED ||
                   to == State::CONFIRMED;

        case State::CONFLICTED:
            // Conflicted is mostly terminal, but can transition to:
            // - UNCONFIRMED (deep reorg resurrects tx)
            // - CONFIRMED (deep reorg resurrects and confirms)
            // NOTE: Rare but must handle for reorg safety
            return to == State::UNCONFIRMED ||
                   to == State::CONFIRMED;

        case State::LOCKED:
            // Locked can transition to:
            // - Any state (user unlocks, then normal transitions apply)
            // NOTE: This is a user-controlled overlay state
            return true;

        default:
            return false;
    }
}

std::string WalletUTXOStateMachine::getTransitionError(WalletUTXOState from, WalletUTXOState to) {
    if (canTransition(from, to)) {
        return "";  // No error
    }

    // Build helpful error message
    std::string from_str = WalletUTXOStateToString(from);
    std::string to_str = WalletUTXOStateToString(to);

    return "Invalid UTXO state transition: " + from_str + " → " + to_str;
}

} // namespace wallet
} // namespace dinero
