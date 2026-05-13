#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Event Sink Interface (Phase 8.3: Event Injection Boundary)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the ONLY way L1 can influence Lightning state.
//
// ARCHITECTURAL CONTRACT:
// - Lightning never queries L1
// - L1 pushes facts, Lightning reacts
// - No daemon/chainstate/wallet headers allowed
// - Strings + primitives only
//
// EVENT ORDERING INVARIANT:
// Events must be delivered in strict height order.
// Violation = undefined behavior (caller bug).
//
// DETERMINISM GUARANTEE:
// For the same event sequence + initial DB state, Lightning always reaches
// the same final state. No timers, no randomness, no wall clocks.
//
// IDEMPOTENCY GUARANTEE:
// Repeated events must not duplicate sweeps, justice, or corrupt state.
// All transitions guarded by current state + recorded heights + unique IDs.
//
// Phase 8.3: Interface definition only (NO IPC/RPC implementation)
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace dinero {
namespace lightning {

/**
 * @struct BlockConnectedEvent
 * @brief Notification that a new block was connected to the chain
 *
 * Used for:
 * - CSV countdowns (commitment maturity, sweep eligibility)
 * - CLTV expiry checks
 * - Justice maturity tracking
 * - Channel confirmation counting
 */
struct BlockConnectedEvent {
    uint64_t height;           // Block height
    std::string block_hash;    // Block hash (hex, 64 chars)
};

/**
 * @struct BlockDisconnectedEvent
 * @brief Notification that a block was disconnected (reorg)
 *
 * Used for:
 * - Rolling back time-based state
 * - Reverting confirmation counts
 * - Invalidating time-locked sweeps/justice
 */
struct BlockDisconnectedEvent {
    uint64_t height;           // Block height that was disconnected
    std::string block_hash;    // Block hash (hex, 64 chars)
};

/**
 * @struct TransactionConfirmedEvent
 * @brief Notification that a transaction was confirmed in a block
 *
 * Used for:
 * - Funding transaction confirmation
 * - Commitment transaction confirmation (force-close detection)
 * - Sweep transaction confirmation
 * - Justice transaction confirmation
 *
 * Lightning matches txid against:
 * - channel.funding_txid
 * - channel.local_commitment_txid / remote_commitment_txid
 * - sweep.sweep_txid
 * - justice.justice_txid
 */
struct TransactionConfirmedEvent {
    std::string txid;          // Transaction ID (hex, 64 chars)
    uint64_t height;           // Block height where confirmed
};

/**
 * @class ILightningEventSink
 * @brief Pure abstract interface for L1 → Lightning event injection
 *
 * This is the ONLY entrypoint for external state changes to Lightning.
 *
 * Implementer: LightningApp (in lightningd process)
 * Caller: dinerod, watchtowers, test harnesses, mobile clients
 *
 * Phase 8.3: Contract definition only (NO IPC implementation)
 * Phase 8.4: IPC transport implementation
 *
 * RULES:
 * - Must be callable from any external process
 * - Must not block indefinitely
 * - Must not throw exceptions
 * - Must be idempotent (safe to retry)
 * - Must accept events in strict height order
 */
class ILightningEventSink {
public:
    virtual ~ILightningEventSink() = default;

    /**
     * @brief Notify Lightning that a new block was connected
     *
     * Lightning will:
     * - Advance internal time tracking
     * - Unlock CSV/CLTV-locked actions (sweeps, justice)
     * - Update confirmation counts for pending channels
     *
     * @param event Block connection event
     */
    virtual void onBlockConnected(const BlockConnectedEvent& event) = 0;

    /**
     * @brief Notify Lightning that a block was disconnected (reorg)
     *
     * Lightning will:
     * - Roll back time-based state
     * - Revert confirmation counts
     * - Invalidate time-locked actions
     *
     * @param event Block disconnection event
     */
    virtual void onBlockDisconnected(const BlockDisconnectedEvent& event) = 0;

    /**
     * @brief Notify Lightning that a transaction was confirmed
     *
     * Lightning will:
     * - Match txid against known transactions
     * - Update channel state (funding confirmed, force-close detected)
     * - Update sweep/justice status (confirmed)
     *
     * @param event Transaction confirmation event
     */
    virtual void onTransactionConfirmed(const TransactionConfirmedEvent& event) = 0;
};

} // namespace lightning
} // namespace dinero
