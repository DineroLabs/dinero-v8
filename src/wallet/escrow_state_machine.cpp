#include "wallet/escrow_state_machine.h"

namespace dinero {

bool EscrowSession::Fund(const std::array<uint8_t, 32>& txid, uint32_t vout) {
    if (state != EscrowState::NEGOTIATING) return false;

    funding_txid = txid;
    funding_vout = vout;
    finalized_trade_id = descriptor.ComputeFinalizedTradeId(txid, vout);
    state = EscrowState::FUNDED;
    return true;
}

bool EscrowSession::Attest(const ReceiptBundle& bundle) {
    if (state != EscrowState::FUNDED) return false;
    if (!bundle.HasThreshold(descriptor.attestor_threshold)) return false;

    receipt_bundle = bundle;
    state = EscrowState::ATTESTED;
    return true;
}

bool EscrowSession::Release(const std::array<uint8_t, 32>& release_txid) {
    if (state != EscrowState::ATTESTED) return false;

    result_txid = release_txid;
    state = EscrowState::RELEASED;
    return true;
}

bool EscrowSession::Refund(const std::array<uint8_t, 32>& refund_txid) {
    if (state != EscrowState::FUNDED) return false;

    result_txid = refund_txid;
    state = EscrowState::REFUNDED;
    return true;
}

bool EscrowSession::Cancel() {
    if (state != EscrowState::NEGOTIATING) return false;

    state = EscrowState::CANCELLED;
    return true;
}

std::string EscrowSession::StateName(EscrowState s) {
    switch (s) {
        case EscrowState::NEGOTIATING: return "Negotiating";
        case EscrowState::FUNDED:      return "Funded";
        case EscrowState::ATTESTED:    return "Attested";
        case EscrowState::RELEASED:    return "Released";
        case EscrowState::REFUNDED:    return "Refunded";
        case EscrowState::CANCELLED:   return "Cancelled";
        default:                       return "Unknown";
    }
}

} // namespace dinero
