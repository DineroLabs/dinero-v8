/**
 * Wallet Routing Engine — see include/wallet/wallet_routing_engine.h.
 *
 * Routing logic:
 *
 *   destination   | mode        | decision
 *   ------------- | ----------- | --------
 *   din1p...      | transparent | TransparentTaproot
 *   din1r...      | transparent | TransparentP2MR
 *   din1p/r...    | private     | ShieldToCommitment (needs shielded payment code)
 *   (none)        | private→tap | Unshield (spend shielded → transparent output)
 */

#include "wallet/wallet_routing_engine.h"
#include "wallet/p2mr_address.h"
#include "consensus/shielded/shielded_tx.h"

namespace dinero::wallet {

namespace {

bool IsTaprootAddress(const std::string& addr) {
    return addr.size() > 5 &&
           (addr.rfind("din1p", 0) == 0 || addr.rfind("rdin1p", 0) == 0 ||
            addr.rfind("tdin1p", 0) == 0);
}

bool IsP2MRAddress(const std::string& addr) {
    return addr.size() > 5 &&
           (addr.rfind("din1r", 0) == 0 || addr.rfind("rdin1r", 0) == 0 ||
            addr.rfind("tdin1r", 0) == 0);
}

} // namespace

RouteResult Route(const SendRequest& req) {
    RouteResult r;

    if (req.destination.empty()) {
        r.error = "destination is required";
        return r;
    }

    if (req.amount_una == 0) {
        r.error = "amount must be positive";
        return r;
    }

    const bool is_taproot = IsTaprootAddress(req.destination);
    const bool is_p2mr    = IsP2MRAddress(req.destination);
    // ── Transparent mode ──
    // If the user picks "Normal" and the wallet only has shielded funds,
    // the engine implicitly unshields. The user never sees "unshield"
    // as a UI action — it's a routing decision.
    if (req.mode == SendMode::Transparent) {
        if (!is_taproot && !is_p2mr) {
            r.error = "unrecognized address format";
            return r;
        }
        if (req.has_shielded_balance) {
            // Source is shielded pool → implicit unshield to transparent output
            r.decision = RouteDecision::Unshield;
            r.estimated_vwu = consensus::shielded::SHIELDED_SPEND_VWU;
        } else if (is_taproot) {
            r.decision = RouteDecision::TransparentTaproot;
            r.estimated_vwu = 66;
        } else {
            r.decision = RouteDecision::TransparentP2MR;
            r.estimated_vwu = 5274;
        }
        return r;
    }

    // ── Private mode ──
    // If the user picks "Private", the engine:
    //   - From transparent source → implicit shield (burn UTXO → commitment)
    //   - From shielded source → private transfer (nullifier → new commitment)
    // The user never sees "shield" as a UI action.
    if (req.mode == SendMode::Private) {
        if (req.payment_code.empty()) {
            r.error = "private mode requires a shielded payment code";
            return r;
        }
        if (req.has_shielded_balance) {
            r.decision = RouteDecision::PrivateTransfer;
            r.estimated_vwu = consensus::shielded::SHIELDED_SPEND_VWU
                            + consensus::shielded::SHIELDED_OUTPUT_VWU;
        } else {
            r.decision = RouteDecision::ShieldToCommitment;
            r.estimated_vwu = consensus::shielded::SHIELDED_OUTPUT_VWU;
        }
        return r;
    }

    // ── Auto mode: transparent by default (fee-optimal) ──
    if (req.mode == SendMode::Auto) {
        if (is_taproot) {
            r.decision = RouteDecision::TransparentTaproot;
            r.estimated_vwu = 66;
        } else if (is_p2mr) {
            r.decision = RouteDecision::TransparentP2MR;
            r.estimated_vwu = 5274;
        } else {
            r.error = "unrecognized address format for auto mode";
        }
        return r;
    }

    r.error = "invalid send mode";
    return r;
}

} // namespace dinero::wallet
