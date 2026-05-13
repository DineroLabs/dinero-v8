#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Database Record Types (L1/L2 Boundary - Pure Data)
// ═══════════════════════════════════════════════════════════════════════════
// Plain data structures for Lightning state persistence.
//
// ARCHITECTURE:
// - NO L1 dependencies (daemon, chainstate, wallet, RocksDB, etc.)
// - NO serialization concerns (msgpack, protobuf, etc.)
// - Pure POD (Plain Old Data) with STL types only
// - Used by both:
//   * L2 pure core (channel_manager_core) via ILightningDB interface
//   * L1 concrete implementations (LightningDB with RocksDB)
//
// Serialization is handled in L1 adapter layer, NOT here.
// This enables compile-time enforcement: L2 core compiles WITHOUT external deps.
// ═══════════════════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace lightning {

/**
 * @struct ChannelRecord
 * @brief Lightning channel data persisted in LightningDB
 *
 * NOTE: This is the minimal set of fields needed for persistence.
 * Runtime state (pending HTLCs, etc.) is stored separately.
 */
struct ChannelRecord {
    // Identity
    std::string channel_id;              // 32-byte hex channel ID
    std::string peer_node_id;            // Remote peer's node pubkey (33-byte hex)
    uint64_t short_channel_id = 0;       // Short Channel ID (SCID): block(24) || tx_index(24) || vout(16)

    // Funding
    std::string funding_txid;            // Funding transaction hash
    uint32_t funding_vout = 0;           // Output index in funding tx
    uint64_t funding_amount_una = 0;    // Total channel capacity

    // Balances (in milli-una)
    uint64_t local_balance_muna = 0;    // Our balance
    uint64_t remote_balance_muna = 0;   // Peer's balance

    // Channel state
    uint32_t state = 0;                  // ChannelState enum value
    uint64_t commitment_number = 0;      // Current commitment version
    std::string revocation_secret;       // Current revocation secret (hex)

    // Taproot keys (hex-encoded)
    std::string local_funding_key;       // Our funding pubkey
    std::string remote_funding_key;      // Peer's funding pubkey
    std::string revocation_basepoint;    // Revocation key base

    // HD wallet key derivation
    uint32_t local_key_index = 0;        // HD wallet derivation index for local funding key

    // Metadata
    uint64_t created_at = 0;             // Channel open timestamp
    uint64_t last_update = 0;            // Last update timestamp
    bool is_initiator = false;           // true if we opened
    uint32_t to_self_delay = 144;        // CSV delay (blocks)
    uint32_t dust_limit_una = 546;      // Minimum output value

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 7A: Force-Close & On-Chain Recovery
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Terminal state tracking for force-close detection and recovery.
     *
     * Invariants:
     * - terminal_state is immutable once set to non-NONE
     * - commitment txids must be persisted before channel opens
     * - recovery_blocked = true means channel is corrupted
     */
    uint32_t terminal_state = 0;             // ChannelTerminalState enum value
    std::string local_commitment_txid;       // Our latest commitment tx ID (hex)
    std::string remote_commitment_txid;      // Peer's latest commitment tx ID (hex)
    uint64_t close_detected_height = 0;      // Block height where close was detected
    std::string closing_txid;                // TX that closed the channel (if known)
    bool recovery_blocked = false;           // true if channel is corrupted/unrecoverable

    // Note: Serialization (msgpack, protobuf, etc.) is handled in L1 adapter layer
};

/**
 * @struct HTLCRecord
 * @brief Hash Time-Locked Contract record
 */
struct HTLCRecord {
    std::string htlc_id;           // Unique HTLC identifier
    std::string channel_id;        // Channel this HTLC belongs to
    uint64_t amount_muna = 0;     // HTLC amount in milli-una
    std::string payment_hash;      // 32-byte payment hash (hex)
    uint32_t cltv_expiry = 0;      // CheckLockTimeVerify expiry height
    bool is_incoming = false;      // true = incoming, false = outgoing

    // Routing information
    std::string next_hop;          // Next channel_id in payment route
    std::string prev_hop;          // Previous channel_id in payment route

    // State tracking
    uint32_t state = 0;            // HTLC::State enum value
    uint64_t created_at = 0;       // Unix timestamp
    uint64_t updated_at = 0;       // Last state change timestamp
};

/**
 * @struct CommitmentRecord
 * @brief Commitment transaction data
 */
struct CommitmentRecord {
    std::string commitment_id;     // Unique commitment identifier
    std::string channel_id;        // Channel this commitment belongs to
    uint64_t commitment_num = 0;   // Commitment transaction number
    std::string local_sig;         // Local signature (hex)
    std::string remote_sig;        // Remote signature (hex)
    std::string tx_data;           // Serialized transaction (hex)
    uint64_t created_at = 0;       // Unix timestamp
};

/**
 * @struct PeerRecord
 * @brief Lightning peer information
 */
struct PeerRecord {
    std::string node_id;           // 33-byte compressed pubkey (hex)
    std::string address;           // IP:port or onion address
    uint64_t last_seen = 0;        // Unix timestamp
    bool trusted = false;          // Manual trust flag
};

/**
 * @struct InvoiceRecord
 * @brief Lightning invoice data (BOLT #11)
 */
struct InvoiceRecord {
    std::string payment_hash;              // 32-byte payment hash (hex) - used as DB key
    std::string bolt11_string;             // Encoded BOLT #11 invoice
    std::string preimage;                  // 32-byte payment preimage (hex) - only for created invoices

    // Amount and description
    uint64_t amount_muna = 0;             // 0 = "any amount" invoice
    std::string description;               // Payment description

    // Timing
    uint64_t created_at = 0;               // Unix timestamp when invoice created
    uint64_t expires_at = 0;               // Unix timestamp when invoice expires

    // Status
    uint32_t status = 0;                   // InvoiceStatus enum value
    uint64_t paid_at = 0;                  // Unix timestamp when paid (0 = unpaid)
    std::string paid_by_channel;           // Channel ID used for payment

    // Metadata
    std::string label;                     // User-defined label
    std::vector<std::string> tags;         // User-defined tags
};

/**
 * @struct HTLCSweepRecord
 * @brief Phase 7B: HTLC sweep tracking after force-close
 *
 * After a channel is force-closed, HTLCs need to be swept from the commitment
 * transaction outputs. This record tracks sweep attempts for each HTLC.
 *
 * Sweep paths:
 * - TIMEOUT: Outgoing HTLCs after CLTV expiry (we claim back our funds)
 * - SUCCESS: Incoming HTLCs with preimage (we claim their funds)
 *
 * Timing constraints:
 * - Must wait for CSV delay (to_self_delay blocks) after commitment confirmation
 * - TIMEOUT sweeps require CLTV expiry height to be reached
 * - SUCCESS sweeps can happen immediately after CSV delay
 */
struct HTLCSweepRecord {
    // Identity
    std::string sweep_id;              // Unique sweep identifier (htlc_id + "_sweep")
    std::string htlc_id;               // HTLC being swept
    std::string channel_id;            // Channel this sweep belongs to

    // Sweep parameters
    uint32_t sweep_type = 0;           // HTLCSweepType enum value (TIMEOUT/SUCCESS)
    uint64_t amount_muna = 0;         // Amount to sweep (may be less than HTLC amount due to fees)
    std::string preimage;              // Payment preimage (hex) - only for SUCCESS sweeps

    // Timing
    uint64_t csv_expiry_height = 0;    // Block height when CSV delay expires
    uint64_t cltv_expiry_height = 0;   // Block height when CLTV expires (for TIMEOUT)
    uint64_t earliest_sweep_height = 0; // Earliest block height when sweep is valid

    // Phase 7B: Commitment transaction metadata (required for sweep TX building)
    std::string commitment_txid;       // Commitment TX containing HTLC output (hex)
    uint32_t htlc_output_index = 0;    // Output index of HTLC in commitment TX
    uint32_t csv_delay = 144;          // CSV delay blocks (typically to_self_delay)
    std::string htlc_script_hex;       // Full HTLC script (hex) for witness construction
    std::string local_htlc_pubkey;     // Local HTLC pubkey (hex)
    std::string remote_htlc_pubkey;    // Remote HTLC pubkey (hex)

    // Sweep transaction
    std::string sweep_txid;            // Sweep transaction ID (hex) - empty until broadcast
    std::string sweep_tx_hex;          // Sweep transaction (hex) - built by L1

    // Status tracking
    uint32_t status = 0;               // HTLCSweepStatus enum value
    uint32_t attempts = 0;             // Number of broadcast attempts
    uint64_t last_attempt_height = 0;  // Block height of last attempt
    uint64_t confirmed_height = 0;     // Block height where sweep confirmed (0 = unconfirmed)

    // Metadata
    uint64_t created_at = 0;           // Unix timestamp when sweep was created
    uint64_t updated_at = 0;           // Last state change timestamp
    std::string failure_reason;        // Failure reason if sweep failed
};

/**
 * @struct JusticeRecord
 * @brief Phase 7C: Justice transaction tracking after breach detection
 *
 * When a counterparty broadcasts a revoked commitment transaction (breach),
 * we create a justice record to track punishment enforcement.
 *
 * Justice path:
 * 1. Detect revoked commitment on-chain
 * 2. Create JusticeRecord with CSV maturity constraints
 * 3. Wait for CSV delay (to_self_delay blocks)
 * 4. Broadcast justice transaction (claims all channel funds)
 * 5. Track confirmation
 *
 * Timing constraint:
 * - Must wait for CSV delay (to_self_delay blocks) after breach detection
 *
 * Invariants:
 * - One justice record per breach (idempotent on duplicate detection)
 * - Justice records are immutable once created
 * - Status transitions are monotonic (no rollbacks)
 */
struct JusticeRecord {
    // Identity
    std::string justice_id;            // Unique justice identifier (channel_id + "_justice_" + commitment_txid)
    std::string channel_id;            // Channel where breach occurred
    std::string commitment_txid;       // Revoked commitment transaction ID (hex)

    // Revocation proof
    uint64_t revoked_commitment_number; // Commitment number that was revoked
    std::string revocation_secret;     // Secret needed to claim revoked outputs (hex)

    // Timing constraints
    uint64_t breach_detected_height;   // Block height where breach was detected
    uint64_t csv_expiry_height;        // Block height when CSV delay expires
    uint64_t earliest_justice_height;  // Earliest block height when justice TX is valid

    // Justice transaction
    std::string justice_txid;          // Justice transaction ID (hex) - empty until broadcast
    std::string justice_tx_hex;        // Justice transaction (hex) - built by L1

    // Status tracking
    uint32_t status = 0;               // JusticeStatus enum value
    uint32_t attempts = 0;             // Number of broadcast attempts
    uint64_t last_attempt_height = 0;  // Block height of last attempt
    uint64_t confirmed_height = 0;     // Block height where justice confirmed (0 = unconfirmed)

    // Metadata
    uint64_t created_at = 0;           // Unix timestamp when justice was created
    uint64_t updated_at = 0;           // Last state change timestamp
    std::string failure_reason;        // Failure reason if justice failed
};

/**
 * @struct PaymentRecord
 * @brief Lightning payment attempt data (outgoing payments)
 */
struct PaymentRecord {
    std::string payment_hash;              // 32-byte payment hash (hex) - used as DB key
    std::string bolt11_string;             // Original BOLT #11 invoice (if paid via invoice)
    std::string destination_node_id;       // Destination node pubkey (33-byte hex)

    // Amount and fees
    uint64_t amount_muna = 0;             // Payment amount in milli-una
    uint64_t fee_muna = 0;                // Total routing fees paid

    // Status
    uint32_t status = 0;                   // PaymentStatus enum value (0=pending, 1=succeeded, 2=failed)
    std::string failure_reason;            // Failure reason (empty if succeeded)
    uint32_t attempts = 0;                 // Number of payment attempts

    // Timing
    uint64_t created_at = 0;               // Unix timestamp when payment initiated
    uint64_t completed_at = 0;             // Unix timestamp when payment completed (0 = pending)

    // Phase 5.4: BOLT #4 failure tracking
    uint16_t last_failure_code = 0;        // Last BOLT #4 FailureCode (0 = no failure yet, or succeeded)
    uint64_t timestamp_last_failure = 0;   // Unix timestamp of last failure (0 = no failure yet)

    // Phase 6.1: Retry history and adaptive routing
    std::string failure_history_json;      // JSON array of retry attempts with failure codes
    std::string last_successful_route;     // JSON-encoded last successful route for this dest

    // Proof of payment
    std::string preimage;                  // 32-byte payment preimage (hex) - empty if failed

    // Route information
    std::string route_json;                // JSON-encoded route taken (channel_ids, fees per hop)
    uint32_t route_hops = 0;               // Number of hops in route

    // Metadata
    std::string label;                     // User-defined label
    std::vector<std::string> tags;         // User-defined tags

    // Phase 7: Multi-path payment (MPP) tracking
    std::string mpp_set_id;                // UUID grouping MPP parts (empty for single-path)
    uint32_t mpp_total_parts = 0;          // Total parts in MPP set (0 for single-path)
    uint32_t mpp_parts_succeeded = 0;      // Parts that completed successfully
    std::string mpp_parts_json;            // JSON array of part details
};

/**
 * @struct RevokedCommitmentRecord
 * @brief Phase 7C/9: Revoked commitment transaction data for watchtower
 *
 * Stores information about revoked commitment transactions to detect breaches.
 * When a revoked commitment is confirmed on-chain, watchtower triggers justice.
 */
struct RevokedCommitmentRecord {
    std::string commitment_txid;           // Transaction ID of revoked commitment (hex) - DB key
    std::string channel_id;                // Channel this commitment belongs to (32-byte hex)

    // Revocation data
    std::string revocation_secret;         // Revocation secret for this commitment
    uint64_t commitment_number;            // Commitment number (for ordering)

    // Justice transaction data
    std::string to_local_script;           // Script for local output
    std::string to_remote_script;          // Script for remote output
    uint64_t to_local_delay;               // CSV delay for to_local output

    // Amounts (for justice transaction calculation)
    uint64_t local_balance_muna;          // Local balance in this commitment
    uint64_t remote_balance_muna;         // Remote balance in this commitment

    // Metadata
    uint64_t revoked_at;                   // Unix timestamp when commitment was revoked
    uint64_t detected_at;                  // Unix timestamp when breach was detected (0 = not detected)
    uint64_t justice_txid;                 // Justice transaction ID (empty = not yet created)
};

} // namespace lightning
} // namespace dinero
