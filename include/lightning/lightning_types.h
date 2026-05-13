#pragma once

#include "result.h"  // Global Result<T> template (used throughout Lightning code)
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace dinero {
namespace lightning {

/**
 * @file lightning_types.h
 * @brief Shared data structures for Dinero Lightning Network
 *
 * Phase 7.0: Core type definitions used across all Lightning components
 */

// ═══════════════════════════════════════════════════════════════════════════
// Channel State Machine
// ═══════════════════════════════════════════════════════════════════════════

enum class ChannelState {
    PENDING_OPEN,     // Funding tx broadcast, awaiting confirmations
    OPEN,             // Channel active, can route payments
    PENDING_CLOSE,    // Cooperative close initiated
    FORCE_CLOSING,    // Unilateral close (breach or timeout)
    CLOSED            // On-chain settled
};

inline std::string channelStateToString(ChannelState state) {
    switch (state) {
        case ChannelState::PENDING_OPEN: return "PENDING_OPEN";
        case ChannelState::OPEN: return "OPEN";
        case ChannelState::PENDING_CLOSE: return "PENDING_CLOSE";
        case ChannelState::FORCE_CLOSING: return "FORCE_CLOSING";
        case ChannelState::CLOSED: return "CLOSED";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Channel terminal state (Phase 7A: Force-Close & On-Chain Recovery)
 *
 * Describes how a channel was closed on-chain (if at all).
 * Used for deterministic recovery after crash/restart.
 *
 * Invariants:
 * - Terminal states are immutable once set
 * - NONE means channel is still open or hasn't closed yet
 * - UNKNOWN is temporary until close type is classified
 */
enum class ChannelTerminalState {
    NONE,                  // Channel not closed (still OPEN/PENDING_OPEN)
    FORCE_CLOSED_LOCAL,    // Our commitment transaction confirmed on-chain
    FORCE_CLOSED_REMOTE,   // Peer's commitment transaction confirmed on-chain
    FORCE_CLOSED_UNKNOWN   // Some close detected, type not yet classified
};

inline std::string terminalStateToString(ChannelTerminalState state) {
    switch (state) {
        case ChannelTerminalState::NONE: return "NONE";
        case ChannelTerminalState::FORCE_CLOSED_LOCAL: return "FORCE_CLOSED_LOCAL";
        case ChannelTerminalState::FORCE_CLOSED_REMOTE: return "FORCE_CLOSED_REMOTE";
        case ChannelTerminalState::FORCE_CLOSED_UNKNOWN: return "FORCE_CLOSED_UNKNOWN";
        default: return "INVALID";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// On-Chain Event Signals (Phase 7A: L1 → L2 Oracle Notifications)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief On-chain event notification from L1 to L2
 *
 * Lightning L2 does NOT scan chainstate. Instead, L1 delivers signals when:
 * - An outpoint is spent
 * - A transaction is confirmed
 *
 * L2 reacts to these signals to detect channel closes and trigger recovery.
 */
struct OnChainEvent {
    enum class Type {
        OUTPOINT_SPENT,    // A specific outpoint (txid:vout) was spent
        TX_CONFIRMED       // A transaction was confirmed in a block
    };

    Type type;                    // Event type
    std::string txid;             // Transaction ID (hex)
    uint32_t vout;                // Output index (for OUTPOINT_SPENT)
    uint64_t block_height;        // Block height where event occurred
    std::string spending_txid;    // TXID of transaction that spent the outpoint (for OUTPOINT_SPENT)

    OnChainEvent()
        : type(Type::TX_CONFIRMED), vout(0), block_height(0) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Phase 7B: HTLC Sweep Types (After Force-Close)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief HTLC sweep type (how we claim HTLC funds after force-close)
 *
 * After a force-close, HTLCs in the commitment transaction need to be swept:
 * - TIMEOUT: Outgoing HTLCs after CLTV expiry (we claim back our funds)
 * - SUCCESS: Incoming HTLCs with preimage (we claim their payment)
 */
enum class HTLCSweepType {
    TIMEOUT,    // Sweep outgoing HTLC via timeout path (after CLTV expiry)
    SUCCESS     // Sweep incoming HTLC via success path (with preimage)
};

inline std::string sweepTypeToString(HTLCSweepType type) {
    switch (type) {
        case HTLCSweepType::TIMEOUT: return "TIMEOUT";
        case HTLCSweepType::SUCCESS: return "SUCCESS";
        default: return "INVALID";
    }
}

/**
 * @brief HTLC sweep status (lifecycle of sweep attempt)
 *
 * Tracks the progress of sweeping an HTLC after force-close:
 * - PENDING: Sweep identified, waiting for height constraints
 * - READY: Height constraints satisfied, ready to build TX
 * - BROADCAST: Sweep transaction broadcast to mempool
 * - CONFIRMED: Sweep transaction confirmed on-chain
 * - FAILED: Sweep attempt failed permanently
 */
enum class HTLCSweepStatus {
    PENDING,     // Waiting for CSV/CLTV constraints
    READY,       // Ready to sweep (constraints satisfied)
    BROADCAST,   // Sweep TX broadcast, awaiting confirmation
    CONFIRMED,   // Sweep TX confirmed, funds recovered
    FAILED       // Sweep failed permanently (expired, double-spent, etc.)
};

inline std::string sweepStatusToString(HTLCSweepStatus status) {
    switch (status) {
        case HTLCSweepStatus::PENDING: return "PENDING";
        case HTLCSweepStatus::READY: return "READY";
        case HTLCSweepStatus::BROADCAST: return "BROADCAST";
        case HTLCSweepStatus::CONFIRMED: return "CONFIRMED";
        case HTLCSweepStatus::FAILED: return "FAILED";
        default: return "INVALID";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 7C: Justice Transaction Types (Breach Detection & Punishment)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Justice transaction status (lifecycle of breach punishment)
 *
 * Tracks the progress of punishing a counterparty who broadcast a revoked
 * commitment transaction:
 * - PENDING: Breach detected, waiting for CSV delay
 * - READY: CSV delay satisfied, ready to broadcast justice TX
 * - BROADCAST: Justice TX broadcast to mempool
 * - CONFIRMED: Justice TX confirmed, funds recovered
 * - FAILED: Justice attempt failed permanently
 */
enum class JusticeStatus {
    PENDING,     // Waiting for CSV delay after breach detection
    READY,       // Ready to broadcast justice transaction
    BROADCAST,   // Justice TX broadcast, awaiting confirmation
    CONFIRMED,   // Justice TX confirmed, breach punished
    FAILED       // Justice attempt failed permanently
};

inline std::string justiceStatusToString(JusticeStatus status) {
    switch (status) {
        case JusticeStatus::PENDING: return "PENDING";
        case JusticeStatus::READY: return "READY";
        case JusticeStatus::BROADCAST: return "BROADCAST";
        case JusticeStatus::CONFIRMED: return "CONFIRMED";
        case JusticeStatus::FAILED: return "FAILED";
        default: return "INVALID";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// HTLC (Hashed Time-Locked Contract)
// ═══════════════════════════════════════════════════════════════════════════

struct HTLC {
    std::string htlc_id;                 // Unique HTLC identifier (32-byte hex)
    std::string channel_id;              // Channel this HTLC belongs to
    uint64_t amount_muna;               // Amount in milli-una
    std::vector<uint8_t> payment_hash;   // SHA256(preimage) - 32 bytes
    uint32_t cltv_expiry;                // Absolute block height for timeout
    bool is_incoming;                    // true = receive, false = send

    // Routing information
    std::string next_hop;                // Next channel_id in payment route
    std::string prev_hop;                // Previous channel_id in payment route

    // State tracking
    enum class State {
        PENDING,      // HTLC offered, awaiting settlement
        SETTLED,      // Preimage revealed, payment complete
        FAILED,       // Payment failed (routing error, etc.)
        TIMED_OUT     // CLTV expiry reached without settlement
    } state;

    uint64_t created_at;                 // Unix timestamp
    uint64_t updated_at;                 // Last state change timestamp

    HTLC()
        : amount_muna(0),
          cltv_expiry(0),
          is_incoming(false),
          state(State::PENDING),
          created_at(0),
          updated_at(0) {}
};

inline std::string htlcStateToString(HTLC::State state) {
    switch (state) {
        case HTLC::State::PENDING: return "PENDING";
        case HTLC::State::SETTLED: return "SETTLED";
        case HTLC::State::FAILED: return "FAILED";
        case HTLC::State::TIMED_OUT: return "TIMED_OUT";
        default: return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Channel
// ═══════════════════════════════════════════════════════════════════════════

struct Channel {
    // Identity
    std::string channel_id;              // Unique channel ID (32-byte hex)
    std::string peer_node_id;            // Remote peer's node pubkey (33-byte hex)
    uint64_t short_channel_id;           // Short Channel ID (SCID): block(24) || tx_index(24) || vout(16)

    // Funding transaction (on-chain)
    std::string funding_txid;            // Funding transaction hash
    uint32_t funding_vout;               // Output index in funding tx
    uint64_t funding_amount_una;        // Total channel capacity in una

    // Balances (in milli-una for precision)
    uint64_t local_balance_muna;        // Our balance in muna
    uint64_t remote_balance_muna;       // Peer's balance in muna

    // Channel state
    ChannelState state;
    uint64_t commitment_number;          // Current commitment transaction version
    std::vector<uint8_t> revocation_secret;  // Current revocation secret

    // Taproot keys (BIP340 x-only pubkeys, 32 bytes each)
    std::vector<uint8_t> local_funding_key;      // Our funding pubkey
    std::vector<uint8_t> remote_funding_key;     // Peer's funding pubkey
    std::vector<uint8_t> revocation_basepoint;   // Revocation key derivation base

    // HD Wallet key derivation index (persisted to database)
    // Used to derive private key from seed: m/84'/1448'/0'/3/local_key_index
    uint32_t local_key_index;                    // Channel key derivation index

    // Pending HTLCs
    std::vector<HTLC> pending_htlcs;

    // Metadata
    uint64_t created_at;                 // Channel open timestamp
    uint64_t last_update;                // Last commitment update timestamp
    bool is_initiator;                   // true if we opened the channel
    uint32_t to_self_delay;              // CSV delay for our outputs (blocks)
    uint32_t dust_limit_una;            // Minimum output value

    // Phase 13.2: Cooperative close scriptpubkeys
    std::vector<uint8_t> local_shutdown_scriptpubkey;   // Our closing address scriptPubKey
    std::vector<uint8_t> remote_shutdown_scriptpubkey;  // Peer's closing address scriptPubKey

    Channel()
        : short_channel_id(0),
          funding_vout(0),
          funding_amount_una(0),
          local_balance_muna(0),
          remote_balance_muna(0),
          state(ChannelState::PENDING_OPEN),
          commitment_number(0),
          local_key_index(0),
          created_at(0),
          last_update(0),
          is_initiator(false),
          to_self_delay(144),              // Default: 1 day (144 blocks)
          dust_limit_una(546) {}          // Dinero-compatible dust limit
};

// ═══════════════════════════════════════════════════════════════════════════
// Payment Route
// ═══════════════════════════════════════════════════════════════════════════

struct Hop {
    std::string channel_id;              // Channel to route through
    std::string node_id;                 // Node at this hop
    uint64_t amount_muna;               // Amount to forward
    uint64_t fee_muna;                  // Routing fee for this hop
    uint32_t cltv_delta;                 // CLTV expiry delta
};

struct Route {
    std::vector<Hop> hops;               // Payment path
    uint64_t total_amount_muna;         // Total amount (including fees)
    uint64_t total_fee_muna;            // Sum of all routing fees
    uint32_t total_timelock;             // Total CLTV expiry
};

// ═══════════════════════════════════════════════════════════════════════════
// Lightning Invoice (BOLT #11)
// ═══════════════════════════════════════════════════════════════════════════

struct LightningInvoice {
    std::string payment_hash;            // SHA256 of preimage (32-byte hex)
    std::vector<uint8_t> preimage;       // Payment preimage (kept secret until settlement)
    uint64_t amount_muna;                // Invoice amount in muna (milli-una)
    std::string description;             // Human-readable description
    uint32_t expiry_seconds;             // Invoice expiry time (default: 3600)
    uint64_t created_at;                 // Unix timestamp
    std::string bolt11_string;           // Encoded BOLT #11 invoice (lndin1p...)
    std::vector<uint8_t> signature;      // ECDSA signature over invoice data

    LightningInvoice()
        : amount_muna(0),
          expiry_seconds(3600),
          created_at(0) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Result Types (Rust-style error handling)
// ═══════════════════════════════════════════════════════════════════════════

// NOTE: Lightning Network code uses the global ::Result<T> type from include/result.h
// The duplicate Result type that was previously defined here has been removed to avoid
// ambiguity and template instantiation conflicts.
//
// Usage:
//   ::Result<MyType> myFunction() {
//       if (error) return ::Result<MyType>::Err("error message");
//       return ::Result<MyType>::Ok(value);
//   }
//
// The global Result<T> provides:
//   - .isOk() / .isErr() - status checks
//   - .value() / .unwrap() - get success value
//   - .error() - get error message
//
// For backward compatibility, Lightning code can use either .value() or .unwrap().

// ═══════════════════════════════════════════════════════════════════════════
// Short Channel ID (SCID) Utilities (BOLT #7)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Generate Short Channel ID from funding transaction location
 *
 * BOLT #7: SCID = block_height(24 bits) || tx_index(24 bits) || output_index(16 bits)
 * Total: 64 bits
 *
 * @param block_height Block height where funding tx was confirmed
 * @param tx_index Transaction index within block (0-based)
 * @param output_index Output index (vout) in transaction
 * @return uint64_t Short Channel ID
 */
inline uint64_t generateSCID(uint32_t block_height, uint32_t tx_index, uint16_t output_index) {
    // Validate bounds
    if (block_height > 0xFFFFFF) {   // 24-bit max (16,777,215)
        throw std::out_of_range("Block height exceeds 24-bit maximum");
    }
    if (tx_index > 0xFFFFFF) {        // 24-bit max
        throw std::out_of_range("Transaction index exceeds 24-bit maximum");
    }
    // output_index is already uint16_t, no need to check

    // SCID encoding: block_height(bits 40-63) || tx_index(bits 16-39) || output_index(bits 0-15)
    return (static_cast<uint64_t>(block_height) << 40) |
           (static_cast<uint64_t>(tx_index) << 16) |
           static_cast<uint64_t>(output_index);
}

/**
 * @brief Decode Short Channel ID into components
 *
 * @param scid Short Channel ID to decode
 * @return std::tuple<uint32_t, uint32_t, uint16_t> (block_height, tx_index, output_index)
 */
inline std::tuple<uint32_t, uint32_t, uint16_t> decodeSCID(uint64_t scid) {
    uint32_t block_height = static_cast<uint32_t>((scid >> 40) & 0xFFFFFF);
    uint32_t tx_index = static_cast<uint32_t>((scid >> 16) & 0xFFFFFF);
    uint16_t output_index = static_cast<uint16_t>(scid & 0xFFFF);
    return std::make_tuple(block_height, tx_index, output_index);
}

/**
 * @brief Convert SCID to human-readable string format
 *
 * Format: "blockheight×txindex×vout" (e.g., "123456×78×0")
 *
 * @param scid Short Channel ID
 * @return std::string Human-readable SCID
 */
inline std::string scidToString(uint64_t scid) {
    auto [block, tx, vout] = decodeSCID(scid);
    return std::to_string(block) + "×" + std::to_string(tx) + "×" + std::to_string(vout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Network Constants
// ═══════════════════════════════════════════════════════════════════════════

namespace constants {
    // Protocol version
    constexpr uint32_t PROTOCOL_VERSION = 1;

    // Default channel parameters
    constexpr uint64_t DEFAULT_CHANNEL_CAPACITY_UNA = 1000000;  // 0.01 DIN
    constexpr uint32_t DEFAULT_TO_SELF_DELAY = 144;              // 1 day (144 blocks)
    constexpr uint64_t DEFAULT_DUST_LIMIT_UNA = 546;            // Dinero-compatible
    constexpr uint64_t MIN_CHANNEL_CAPACITY_UNA = 100000;       // 0.001 DIN minimum

    // Payment routing
    constexpr uint32_t MAX_PAYMENT_HOPS = 20;                    // Max route length
    constexpr uint64_t DEFAULT_BASE_FEE_MUNA = 1000;            // 1 una base fee
    constexpr uint64_t DEFAULT_FEE_RATE_PPM = 100;               // 0.01% proportional

    // Timelock parameters
    constexpr uint32_t MIN_CLTV_EXPIRY_DELTA = 9;                // Min blocks per hop
    constexpr uint32_t DEFAULT_CLTV_EXPIRY_DELTA = 40;           // Default CLTV delta

    // Confirmation requirements
    constexpr uint32_t FUNDING_TX_CONFIRMATIONS = 6;             // Confirmations to open
    constexpr uint32_t FORCE_CLOSE_CONFIRMATIONS = 1;            // Confirmations to force-close

    // Invoice defaults
    constexpr uint32_t DEFAULT_INVOICE_EXPIRY_SECONDS = 3600;    // 1 hour

    // ═══════════════════════════════════════════════════════════════════════
    // SAFETY PATTERN #4: Resource Limits
    // ═══════════════════════════════════════════════════════════════════════
    // These limits prevent Lightning from consuming unbounded resources,
    // protecting daemon memory and ensuring graceful degradation under attack.

    // Channel limits (prevent channel exhaustion attacks)
    constexpr uint32_t MAX_CHANNELS_PER_NODE = 1000;             // Maximum channels per node
    constexpr uint32_t WARN_CHANNELS_PER_NODE = 800;             // Warning threshold (80%)

    // HTLC limits (prevent HTLC spam attacks)
    constexpr uint32_t MAX_HTLCS_PER_CHANNEL = 483;              // BOLT #2 spec limit
    constexpr uint32_t MAX_PENDING_HTLCS_TOTAL = 10000;          // Total pending HTLCs across all channels
    constexpr uint32_t WARN_PENDING_HTLCS_TOTAL = 8000;          // Warning threshold (80%)

    // Event queue limits (prevent event queue overflow)
    constexpr uint32_t MAX_BLOCK_EVENT_QUEUE_SIZE = 100;         // Max queued block events
    constexpr uint32_t WARN_BLOCK_EVENT_QUEUE_SIZE = 80;         // Warning threshold

    // Thread pool limits (prevent task queue overflow)
    constexpr uint32_t MAX_THREADPOOL_QUEUE_SIZE = 1000;         // Max queued tasks
    constexpr uint32_t WARN_THREADPOOL_QUEUE_SIZE = 800;         // Warning threshold

    // Memory budget (prevent memory exhaustion)
    constexpr uint64_t MAX_LIGHTNING_MEMORY_MB = 512;            // 512 MB max for Lightning
    constexpr uint64_t WARN_LIGHTNING_MEMORY_MB = 409;           // Warning at 80%

    // ═══════════════════════════════════════════════════════════════════════
    // SAFETY PATTERN #5: Health Monitoring
    // ═══════════════════════════════════════════════════════════════════════
    // These constants define health check thresholds and heartbeat intervals.
    // If the service doesn't emit heartbeats within these timeframes, it's
    // considered unhealthy (possibly deadlocked or overloaded).

    // Heartbeat timeouts (detect deadlocks and hangs)
    constexpr uint32_t HEARTBEAT_TIMEOUT_SECONDS = 60;           // Max time without heartbeat before unhealthy
    constexpr uint32_t CRITICAL_HEARTBEAT_TIMEOUT_SECONDS = 300; // Critical timeout (5 minutes)

    // Exception rate thresholds (detect cascading failures)
    constexpr uint32_t WARN_EXCEPTION_RATE_PER_HOUR = 100;       // Warning at 100 exceptions/hour
    constexpr uint32_t CRITICAL_EXCEPTION_RATE_PER_HOUR = 1000;  // Critical at 1000 exceptions/hour

    // Health check intervals
    constexpr uint32_t HEALTH_CHECK_INTERVAL_SECONDS = 30;       // Run health checks every 30s
}

} // namespace lightning
} // namespace dinero
