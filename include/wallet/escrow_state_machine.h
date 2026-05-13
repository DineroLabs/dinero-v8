#pragma once

#include "wallet/escrow_descriptor.h"
#include "wallet/receipt_bundle.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dinero {

/**
 * Escrow state machine.
 *
 * State transitions:
 *   NEGOTIATING -> FUNDED -> ATTESTED -> RELEASED
 *                         -> REFUNDED
 *                -> CANCELLED
 *
 * NEGOTIATING: pending_trade_id computed, awaiting funding tx
 * FUNDED: funding tx confirmed, finalized_trade_id available
 * ATTESTED: receipt bundle collected (threshold met)
 * RELEASED: seller claimed funds via release leaf
 * REFUNDED: buyer reclaimed funds via timeout leaf
 * CANCELLED: escrow cancelled before funding
 */
enum class EscrowState : uint8_t {
    NEGOTIATING = 0,
    FUNDED      = 1,
    ATTESTED    = 2,
    RELEASED    = 3,
    REFUNDED    = 4,
    CANCELLED   = 5
};

struct EscrowSession {
    EscrowDescriptor descriptor;
    EscrowState state = EscrowState::NEGOTIATING;

    /// Pending trade ID (set at creation)
    std::array<uint8_t, 32> pending_trade_id;

    /// Finalized trade ID (set when FUNDED)
    std::optional<std::array<uint8_t, 32>> finalized_trade_id;

    /// Funding outpoint (set when FUNDED)
    std::optional<std::array<uint8_t, 32>> funding_txid;
    std::optional<uint32_t> funding_vout;

    /// Receipt bundle (set when ATTESTED)
    std::optional<ReceiptBundle> receipt_bundle;

    /// Result txid (set when RELEASED or REFUNDED)
    std::optional<std::array<uint8_t, 32>> result_txid;

    // --- State transitions ---

    /**
     * Transition NEGOTIATING -> FUNDED.
     * Records the funding outpoint and computes finalized_trade_id.
     */
    bool Fund(const std::array<uint8_t, 32>& txid, uint32_t vout);

    /**
     * Transition FUNDED -> ATTESTED.
     * Records the receipt bundle after threshold verification.
     */
    bool Attest(const ReceiptBundle& bundle);

    /**
     * Transition ATTESTED -> RELEASED.
     * Records the release tx ID.
     */
    bool Release(const std::array<uint8_t, 32>& release_txid);

    /**
     * Transition FUNDED -> REFUNDED.
     * Records the refund tx ID.
     */
    bool Refund(const std::array<uint8_t, 32>& refund_txid);

    /**
     * Transition NEGOTIATING -> CANCELLED.
     */
    bool Cancel();

    static std::string StateName(EscrowState s);
};

} // namespace dinero
