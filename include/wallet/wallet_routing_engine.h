#pragma once
/**
 * Wallet Routing Engine — the control center for all sends.
 *
 * INVARIANT: Privacy mode is a transaction policy, not a property of
 * the destination address. All destinations (din1p, din1r) may be
 * satisfied via either transparent or shielded execution depending on
 * wallet policy and routing rules. Address type determines the
 * recipient's cryptographic identity. Mode determines the execution path.
 *
 * Three layers:
 *   1. Address layer (UX):     din1p / din1r
 *   2. Intent layer (engine):  transparent / private / auto
 *   3. Consensus layer:        UTXO output or commitment leaf
 *
 * The engine maps (intent, destination) → execution path. The intent
 * comes from the user, wallet policy, fee constraints, or privacy
 * budget — never from the address type alone.
 *
 *   wallet.send <address> <amount> [mode=transparent|private|auto]
 */

#include <cstdint>
#include <string>

namespace dinero::wallet {

enum class SendMode : uint8_t {
    Transparent = 0,   ///< Default. Builds a transparent output (UTXO in Utreexo).
    Private     = 1,   ///< Shield. Burns transparent UTXO → commitment in tree.
    Auto        = 2,   ///< Engine decides based on wallet policy + fee comparison.
};

struct SendRequest {
    std::string  destination;     ///< din1p... or din1r...
    uint64_t     amount_una = 0;
    SendMode     mode = SendMode::Transparent;
    bool         has_shielded_balance = false;  ///< wallet has funds in shielded pool
    std::string  payment_code;    ///< Future shielded recipient code
};

enum class RouteDecision : uint8_t {
    TransparentTaproot   = 0,  ///< Build P2TR output
    TransparentP2MR      = 1,  ///< Build P2MR output
    ShieldToCommitment   = 2,  ///< Burn transparent → shielded commitment
    PrivateTransfer      = 3,  ///< Shielded → shielded (nullifier + new commitment)
    Unshield             = 4,  ///< Shielded → transparent output
};

struct RouteResult {
    RouteDecision  decision;
    std::string    error;         ///< Non-empty on failure
    uint64_t       estimated_fee_una = 0;
    uint64_t       estimated_vwu = 0;

    bool ok() const { return error.empty(); }
};

/**
 * Determine the routing path for a send request.
 *
 * Pure function — reads wallet policy but does not mutate state.
 * The caller executes the decision via the appropriate builder
 * (UnsignedTxBuilder for transparent, Shield/Unshield for private).
 */
RouteResult Route(const SendRequest& req);

} // namespace dinero::wallet
