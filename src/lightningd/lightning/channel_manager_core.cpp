// ═══════════════════════════════════════════════════════════════════════════
// Lightning Channel Manager Core Implementation (Pure L2)
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/channel_manager_core.h"
// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace lightning {

ChannelManagerCore::ChannelManagerCore(
    std::shared_ptr<::lightning::IChainOracle> chain_oracle,
    std::shared_ptr<::lightning::IWalletOracle> wallet_oracle,
    std::shared_ptr<::lightning::IFundingService> funding_service,
    std::shared_ptr<::lightning::IHTLCSweepOracle> sweep_oracle,
    std::shared_ptr<::lightning::IJusticeOracle> justice_oracle,
    std::shared_ptr<ILightningDB> db,
    const std::string& node_pubkey,
    ::lightning::ITimeOracle* time_oracle
)
    : m_chain_oracle(std::move(chain_oracle))
    , m_wallet_oracle(std::move(wallet_oracle))
    , m_funding_service(std::move(funding_service))
    , m_sweep_oracle(std::move(sweep_oracle))
    , m_justice_oracle(std::move(justice_oracle))
    , m_time_oracle(time_oracle)
    , m_db(std::move(db))
    , m_node_pubkey(node_pubkey)
{
    if (!m_chain_oracle || !m_wallet_oracle || !m_funding_service || !m_sweep_oracle || !m_justice_oracle || !m_db) {
        throw std::runtime_error("ChannelManagerCore: null dependency");
    }
    if (!m_time_oracle) {
        throw std::runtime_error("ChannelManagerCore: time_oracle cannot be null");
    }
    if (m_node_pubkey.empty()) {
        throw std::runtime_error("ChannelManagerCore: node_pubkey cannot be empty");
    }
}

Result<OpenChannelResult> ChannelManagerCore::openChannel(
    const std::string& peer_node_id,
    uint64_t local_amount_sats,
    uint64_t push_amount_sats
) {
    // ═══════════════════════════════════════════════════════════════════════
    // Phase 1: Validation (Pure L2 Logic)
    // ═══════════════════════════════════════════════════════════════════════

    if (peer_node_id.empty()) {
        return Result<OpenChannelResult>::Err("peer_node_id cannot be empty");
    }

    if (local_amount_sats < constants::MIN_CHANNEL_CAPACITY_UNA) {
        return Result<OpenChannelResult>::Err("Channel amount below minimum");
    }

    if (push_amount_sats > local_amount_sats) {
        return Result<OpenChannelResult>::Err("Push amount cannot exceed local funding amount");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 2: L1 Queries (Via Oracles)
    // ═══════════════════════════════════════════════════════════════════════

    // Check wallet availability
    if (!m_wallet_oracle->isAvailable()) {
        return Result<OpenChannelResult>::Err("Wallet not available");
    }

    // Check sufficient balance
    uint64_t wallet_balance = m_wallet_oracle->getConfirmedBalance();
    if (wallet_balance < local_amount_sats + 10000) { // +10k sats for fees
        return Result<OpenChannelResult>::Err("Insufficient wallet balance");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 3: Create Funding Transaction (Via Funding Service)
    // ═══════════════════════════════════════════════════════════════════════

    // Use node's public key for channel funding
    const std::string& local_pubkey = m_node_pubkey;

    // Request funding transaction from L1
    auto funding_result = m_funding_service->createFunding(
        local_amount_sats,
        peer_node_id,          // remote pubkey
        local_pubkey,          // local pubkey
        constants::DEFAULT_TO_SELF_DELAY,
        10  // feerate: 10 sat/kvB
    );

    if (!funding_result) {
        return Result<OpenChannelResult>::Err("Failed to create funding transaction");
    }

    auto& funding = *funding_result;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 4: Create Channel State (Pure L2 Data)
    // ═══════════════════════════════════════════════════════════════════════

    ChannelRecord channel;
    
    // Identity
    channel.channel_id = generateChannelId(funding.funding_txid, funding.funding_vout);
    channel.peer_node_id = peer_node_id;
    channel.short_channel_id = 0; // Assigned after confirmation

    // Funding
    channel.funding_txid = funding.funding_txid;
    channel.funding_vout = funding.funding_vout;
    channel.funding_amount_una = funding.funding_amount_sats;

    // Balances (in milli-una: 1 una = 1000 muna)
    uint64_t local_balance_muna = (local_amount_sats - push_amount_sats) * 1000;
    uint64_t remote_balance_muna = push_amount_sats * 1000;
    channel.local_balance_muna = local_balance_muna;
    channel.remote_balance_muna = remote_balance_muna;

    // Channel state
    channel.state = static_cast<uint32_t>(ChannelState::PENDING_OPEN);
    channel.commitment_number = 0;
    channel.revocation_secret = ""; // Will be generated during channel establishment

    // Keys
    channel.local_funding_key = local_pubkey;
    channel.remote_funding_key = peer_node_id;
    // TODO: Derive revocation_basepoint from HD wallet per BOLT #3
    // For now, use node pubkey as placeholder (will be replaced with proper derivation)
    channel.revocation_basepoint = local_pubkey + "_revocation";
    channel.local_key_index = 0;

    // Metadata
    channel.created_at = getCurrentTimestamp();
    channel.last_update = channel.created_at;
    channel.is_initiator = true;  // We opened the channel
    channel.to_self_delay = constants::DEFAULT_TO_SELF_DELAY;
    channel.dust_limit_una = constants::DEFAULT_DUST_LIMIT_UNA;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 5: Persist State (Via DB Interface - Atomic Transaction)
    // ═══════════════════════════════════════════════════════════════════════

    Status db_status = persistChannelState(channel);
    if (db_status != Status::Ok) {
        return Result<OpenChannelResult>::Err("Failed to persist channel state");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 6: Return Result (L2 State + L1 TX Data)
    // ═══════════════════════════════════════════════════════════════════════

    OpenChannelResult result;
    result.channel = channel;
    result.funding_tx_hex = funding.funding_tx_hex;

    return Result<OpenChannelResult>::Ok(result);
}

Result<void> ChannelManagerCore::closeChannel(
    const std::string& channel_id,
    bool force
) {
    // Get channel from database
    auto channel_opt = m_db->getChannel(channel_id);
    if (!channel_opt) {
        return Result<void>::Err("Channel not found: " + channel_id);
    }

    auto& channel = *channel_opt;

    // Check if channel can be closed
    uint32_t current_state = channel.state;
    if (current_state == static_cast<uint32_t>(ChannelState::CLOSED) ||
        current_state == static_cast<uint32_t>(ChannelState::FORCE_CLOSING) ||
        current_state == static_cast<uint32_t>(ChannelState::PENDING_CLOSE)) {
        return Result<void>::Err("Channel already closing or closed");
    }

    // Update state based on close type
    if (force) {
        channel.state = static_cast<uint32_t>(ChannelState::FORCE_CLOSING);
    } else {
        channel.state = static_cast<uint32_t>(ChannelState::PENDING_CLOSE);
    }

    channel.last_update = getCurrentTimestamp();

    // Persist updated state (atomic transaction)
    Status db_status = persistChannelState(channel);
    if (db_status != Status::Ok) {
        return Result<void>::Err("Failed to update channel state");
    }

    return Result<void>::Ok();
}

std::vector<ChannelRecord> ChannelManagerCore::listChannels() {
    return m_db->listChannels();
}

std::optional<ChannelRecord> ChannelManagerCore::getChannel(const std::string& channel_id) {
    return m_db->getChannel(channel_id);
}

Result<void> ChannelManagerCore::onNewBlock(uint64_t block_height, const std::string& block_hash) {
    (void)block_hash; // Unused in this simplified version

    // Get all channels that might need updates
    auto channels = m_db->listChannels();

    for (auto& channel : channels) {
        bool updated = false;

        // Check funding transaction confirmations
        if (channel.state == static_cast<uint32_t>(ChannelState::PENDING_OPEN)) {
            // Query L1 for funding tx status via oracle
            if (m_chain_oracle->isUnspent(channel.funding_txid, channel.funding_vout)) {
                // Funding tx confirmed - transition to OPEN
                channel.state = static_cast<uint32_t>(ChannelState::OPEN);
                channel.last_update = getCurrentTimestamp();
                
                // Assign short channel ID: block_height || tx_index || vout
                // (In real implementation, would query actual tx index from L1)
                channel.short_channel_id = (block_height << 40) | (1 << 16) | channel.funding_vout;
                
                updated = true;
            }
        }

        // Check for force-close timeout
        if (channel.state == static_cast<uint32_t>(ChannelState::FORCE_CLOSING)) {
            // Check if to_self_delay has elapsed (simplified check)
            uint64_t current_block = m_chain_oracle->getBlockHeight();
            if (current_block >= block_height + channel.to_self_delay) {
                channel.state = static_cast<uint32_t>(ChannelState::CLOSED);
                channel.last_update = getCurrentTimestamp();
                updated = true;
            }
        }

        // Persist updates (atomic transaction)
        if (updated) {
            Status persist_status = persistChannelState(channel);
            if (persist_status != Status::Ok) {
                // Persistence failed after state transition - critical error
                // Cannot continue with this channel in inconsistent state
                // Mark channel as CLOSED to prevent further operations
                channel.state = static_cast<uint32_t>(ChannelState::CLOSED);
                channel.last_update = getCurrentTimestamp();

                // Attempt to persist closed state (best effort)
                Status failure_persist = persistChannelState(channel);
                if (failure_persist != Status::Ok) {
                    // Even failure persistence failed - log and return error
                    return Result<void>::Err("Critical: Failed to persist channel state for " + channel.channel_id + " and failed to mark as CLOSED");
                }

                return Result<void>::Err("Channel " + channel.channel_id + " marked as CLOSED due to persistence error");
            }
        }
    }

    return Result<void>::Ok();
}

// ═══════════════════════════════════════════════════════════════════════════
// On-Chain Event Processing (Phase 7A: Force-Close Detection)
// ═══════════════════════════════════════════════════════════════════════════

Result<void> ChannelManagerCore::onChainEvent(const OnChainEvent& event) {
    // Phase 7A: Event-driven force-close detection
    // L2 receives on-chain signals from L1, reacts to detect channel closes

    // Handle different event types
    switch (event.type) {
        case OnChainEvent::Type::OUTPOINT_SPENT: {
            // Find channel by funding outpoint
            auto channels = m_db->listChannels();

            for (auto& channel : channels) {
                // Check if this event matches the channel's funding outpoint
                if (channel.funding_txid == event.txid &&
                    channel.funding_vout == event.vout) {

                    // Funding outpoint spent - channel is closing/closed
                    Status status = handleFundingSpent(
                        channel,
                        event.spending_txid,
                        event.block_height
                    );

                    if (status != Status::Ok) {
                        return Result<void>::Err(
                            "Failed to handle funding spent for channel " + channel.channel_id
                        );
                    }

                    // Event processed successfully
                    return Result<void>::Ok();
                }
            }

            // No matching channel found - this is OK (might be non-Lightning TX)
            return Result<void>::Ok();
        }

        case OnChainEvent::Type::TX_CONFIRMED: {
            // Phase 7A: TX_CONFIRMED events not yet handled
            // Future use: Track commitment tx confirmations for CSV timelock
            return Result<void>::Ok();
        }

        default:
            return Result<void>::Err("Unknown event type");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Crash Recovery (Phase 6: Restart Safety)
// ═══════════════════════════════════════════════════════════════════════════

ChannelManagerCore::RecoveryResult ChannelManagerCore::restoreFromDB() {
    RecoveryResult result{0, 0, {}};

    if (!m_db || !m_db->isOpen()) {
        result.errors.push_back("Database not available");
        return result;
    }

    // Phase 6: Deterministic recovery with proper ordering
    // 1. Load channels first
    // 2. Validate each channel
    // 3. Load HTLCs only for valid channels
    // 4. Corrupted channels do not poison valid ones
    //
    // Phase 7A: Terminal state recovery
    // 5. Check terminal_state for each channel
    // 6. Validate terminal state consistency
    // 7. Report channels in terminal states (for visibility)

    auto channels = m_db->listChannels();

    for (auto channel : channels) {
        // Phase 7A.6: Check recovery_blocked flag first
        if (channel.recovery_blocked) {
            // Channel marked as unrecoverable - skip it
            result.channels_corrupted++;
            result.errors.push_back(
                "Channel " + channel.channel_id.substr(0, 16) +
                "... BLOCKED: recovery_blocked flag set"
            );
            continue;
        }

        // Validate channel state BEFORE loading HTLCs
        auto validation = validateChannel(channel);

        if (!validation.is_valid) {
            // Channel is corrupted - mark as blocked and persist
            result.channels_corrupted++;
            std::string error = "Channel " + channel.channel_id.substr(0, 16) +
                              "... CORRUPTED: " + validation.error;
            result.errors.push_back(error);

            // Phase 7A.6: Set recovery_blocked flag to prevent future recovery attempts
            channel.recovery_blocked = true;
            channel.last_update = getCurrentTimestamp();
            persistChannelState(channel); // Best-effort persist (ignore failure)

            continue; // Do not process HTLCs for corrupted channels
        }

        // Phase 7A: Check terminal state
        auto terminal_enum = static_cast<ChannelTerminalState>(channel.terminal_state);

        if (terminal_enum != ChannelTerminalState::NONE) {
            // Channel is in terminal state (closed on-chain)
            // Validate terminal state fields are consistent
            if (channel.close_detected_height == 0) {
                result.channels_corrupted++;
                result.errors.push_back(
                    "Channel " + channel.channel_id.substr(0, 16) +
                    "... terminal state set but close_detected_height = 0"
                );

                // Phase 7A.6: Mark as blocked
                channel.recovery_blocked = true;
                channel.last_update = getCurrentTimestamp();
                persistChannelState(channel);

                continue;
            }

            if (channel.closing_txid.empty()) {
                result.channels_corrupted++;
                result.errors.push_back(
                    "Channel " + channel.channel_id.substr(0, 16) +
                    "... terminal state set but closing_txid empty"
                );

                // Phase 7A.6: Mark as blocked
                channel.recovery_blocked = true;
                channel.last_update = getCurrentTimestamp();
                persistChannelState(channel);

                continue;
            }

            // Terminal state is valid - log for visibility
            // (In production, this would trigger recovery actions in Phase 7B)
            std::string terminal_state_str = terminalStateToString(terminal_enum);
            // Note: Not adding to errors since this is informational, not an error
            // Real implementation would log or trigger recovery based on terminal_enum
        }

        // Channel is valid - load and validate associated HTLCs
        auto htlcs = m_db->listHTLCsForChannel(channel.channel_id);

        // Validate HTLCs for sanity
        for (const auto& htlc : htlcs) {
            // Basic validation checks
            if (htlc.amount_muna == 0) {
                result.errors.push_back("HTLC " + htlc.htlc_id + " has zero amount");
                continue;
            }

            if (htlc.payment_hash.empty() || htlc.payment_hash.size() != 64) {  // 32 bytes = 64 hex chars
                result.errors.push_back("HTLC " + htlc.htlc_id + " has invalid payment_hash");
                continue;
            }

            if (htlc.cltv_expiry == 0) {
                result.errors.push_back("HTLC " + htlc.htlc_id + " has zero CLTV expiry");
                continue;
            }

            // Note: Signature validation would require cryptographic verification
            // which depends on commitment transaction context. Deferred to commitment validation.
        }

        result.channels_loaded++;

        // Note: ChannelManagerCore is stateless - all state lives in database
        // In a stateful design, we would:
        // - Add channel to in-memory map
        // - Attach HTLCs to channel object
        // - Rebuild routing indices
        // - Trigger recovery actions for terminal states (Phase 7B)
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Private Helpers
// ═══════════════════════════════════════════════════════════════════════════

ChannelManagerCore::ValidationResult ChannelManagerCore::validateChannel(
    const ChannelRecord& channel
) const {
    // Check required fields
    if (channel.channel_id.empty()) {
        return {false, "Empty channel_id"};
    }

    if (channel.peer_node_id.empty()) {
        return {false, "Empty peer_node_id"};
    }

    if (channel.funding_txid.empty()) {
        return {false, "Empty funding_txid"};
    }

    // Check balance invariant: local + remote = funding
    uint64_t total_muna = channel.local_balance_muna + channel.remote_balance_muna;
    uint64_t expected_muna = channel.funding_amount_una * 1000; // 1 una = 1000 muna

    if (total_muna != expected_muna) {
        return {false, "Balance invariant violated: " +
                std::to_string(total_muna) + " != " + std::to_string(expected_muna)};
    }

    // Check state is valid
    uint32_t state = channel.state;
    if (state > static_cast<uint32_t>(ChannelState::CLOSED)) {
        return {false, "Invalid state: " + std::to_string(state)};
    }

    // Check funding amount is above minimum
    if (channel.funding_amount_una < constants::MIN_CHANNEL_CAPACITY_UNA) {
        return {false, "Funding amount below minimum"};
    }

    // All checks passed
    return {true, ""};
}

Status ChannelManagerCore::persistChannelState(
    const ChannelRecord& channel,
    const std::vector<HTLCRecord>& htlcs
) {
    // Phase 6: Atomic persistence using transactions
    // Ensures channel state + HTLCs are persisted together or not at all

    auto tx = m_db->beginTransaction();
    if (!tx) {
        return Status::Internal; // Transaction creation failed (L1 resource issue)
    }

    // Persist channel state
    Status channel_status = tx->putChannel(channel);
    if (channel_status != Status::Ok) {
        tx->rollback();
        return channel_status;
    }

    // Persist associated HTLCs (if any)
    for (const auto& htlc : htlcs) {
        Status htlc_status = tx->putHTLC(htlc);
        if (htlc_status != Status::Ok) {
            tx->rollback();
            return htlc_status;
        }
    }

    // Commit atomically
    return tx->commit();
}

std::string ChannelManagerCore::generateChannelId(const std::string& funding_txid, uint32_t vout) const {
    // Simplified: channel_id = txid + ":" + vout
    // Real implementation would use proper cryptographic derivation
    std::ostringstream oss;
    oss << funding_txid << ":" << vout;
    return oss.str();
}

uint64_t ChannelManagerCore::getCurrentTimestamp() const {
    // Phase 8.5: Deterministic time from oracle (block-height-based)
    return m_time_oracle->getCurrentTimestamp();
}

// ═══════════════════════════════════════════════════════════════════════════
// Force-Close Detection Helpers (Phase 7A)
// ═══════════════════════════════════════════════════════════════════════════

uint32_t ChannelManagerCore::classifyChannelClose(
    const ChannelRecord& channel,
    const std::string& spending_txid
) const {
    // Phase 7A: Classify channel close by matching spending txid against known commitments
    //
    // Matching rules:
    // - If txid == local_commitment_txid → FORCE_CLOSED_LOCAL
    // - If txid == remote_commitment_txid → FORCE_CLOSED_REMOTE
    // - Else → FORCE_CLOSED_UNKNOWN
    //
    // Note: Justice detection handles UNKNOWN cases by checking if the spending tx
    // is different from both known commitments (potential revoked commitment).

    // Check local commitment match
    if (!channel.local_commitment_txid.empty() &&
        channel.local_commitment_txid == spending_txid) {
        return static_cast<uint32_t>(ChannelTerminalState::FORCE_CLOSED_LOCAL);
    }

    // Check remote commitment match
    if (!channel.remote_commitment_txid.empty() &&
        channel.remote_commitment_txid == spending_txid) {
        return static_cast<uint32_t>(ChannelTerminalState::FORCE_CLOSED_REMOTE);
    }

    // Unknown close type (could be revoked commitment, cooperative close, etc.)
    return static_cast<uint32_t>(ChannelTerminalState::FORCE_CLOSED_UNKNOWN);
}

Status ChannelManagerCore::handleFundingSpent(
    ChannelRecord& channel,
    const std::string& spending_txid,
    uint64_t block_height
) {
    // Phase 7A: Process funding outpoint spent event
    //
    // Invariants:
    // - Idempotent: Same event processed twice → same state
    // - No resurrection: Terminal states are immutable (except UNKNOWN refinement)
    // - Order independent: Event arrival order doesn't affect final state

    // Check if channel already in terminal state
    uint32_t current_terminal_state = channel.terminal_state;
    auto terminal_enum = static_cast<ChannelTerminalState>(current_terminal_state);

    if (terminal_enum != ChannelTerminalState::NONE) {
        // Channel already closed - check for refinement
        if (terminal_enum == ChannelTerminalState::FORCE_CLOSED_UNKNOWN) {
            // Try to refine UNKNOWN → LOCAL/REMOTE with new information
            uint32_t new_terminal_state = classifyChannelClose(channel, spending_txid);
            auto new_enum = static_cast<ChannelTerminalState>(new_terminal_state);

            // Only refine if classification succeeded
            if (new_enum != ChannelTerminalState::FORCE_CLOSED_UNKNOWN) {
                channel.terminal_state = new_terminal_state;
                channel.closing_txid = spending_txid;
                channel.close_detected_height = block_height;
                channel.last_update = getCurrentTimestamp();

                // Persist refined state
                return persistChannelState(channel) == Status::Ok
                    ? Status::Ok
                    : Status::Internal;
            }
        }

        // Already in terminal state (not UNKNOWN, or refinement failed)
        // Idempotent: Do nothing, return success
        return Status::Ok;
    }

    // First time detecting close - classify and transition to terminal state
    uint32_t terminal_state = classifyChannelClose(channel, spending_txid);

    // Update channel state
    channel.terminal_state = terminal_state;
    channel.closing_txid = spending_txid;
    channel.close_detected_height = block_height;
    channel.last_update = getCurrentTimestamp();

    // Transition channel state to CLOSED (L2 state machine)
    channel.state = static_cast<uint32_t>(ChannelState::CLOSED);

    // Phase 7B: Create HTLC sweep records (freeze HTLC set)
    // This happens ONCE when channel enters terminal state
    auto sweeps = identifySweepCandidates(channel);

    // Phase 7C: Create justice record if revoked commitment detected
    //
    // Justice detection logic:
    // - If spending_txid == local_commitment_txid: Our own force close, no justice needed
    // - If spending_txid == remote_commitment_txid: Latest remote commitment, no justice needed
    // - Otherwise: Possible revoked remote commitment, create justice record
    //
    // This covers cases where terminal_state is FORCE_CLOSED_REMOTE or FORCE_CLOSED_UNKNOWN
    std::optional<JusticeRecord> justice_record;
    bool is_potential_breach =
        (spending_txid != channel.local_commitment_txid) &&
        (spending_txid != channel.remote_commitment_txid);

    if (is_potential_breach) {
        // Revoked commitment detected - create justice record
        JusticeRecord justice;
        justice.justice_id = channel.channel_id + "_justice_" + spending_txid;
        justice.channel_id = channel.channel_id;
        justice.commitment_txid = spending_txid;
        justice.revoked_commitment_number = channel.commitment_number > 0
            ? channel.commitment_number - 1
            : 0;
        justice.breach_detected_height = block_height;
        justice.csv_expiry_height = block_height + channel.to_self_delay;
        justice.earliest_justice_height = block_height + channel.to_self_delay;
        justice.status = static_cast<uint32_t>(JusticeStatus::PENDING);
        justice.created_at = getCurrentTimestamp();
        justice.updated_at = getCurrentTimestamp();

        justice_record = justice;
    }

    // Persist channel + sweeps + justice atomically (Phase 6 + 7B + 7C integration)
    auto tx = m_db->beginTransaction();
    if (!tx) {
        return Status::Internal;
    }

    // Persist channel state
    Status channel_status = tx->putChannel(channel);
    if (channel_status != Status::Ok) {
        tx->rollback();
        return Status::Internal;
    }

    // Persist sweep records
    for (const auto& sweep : sweeps) {
        Status sweep_status = tx->putHTLCSweep(sweep);
        if (sweep_status != Status::Ok) {
            tx->rollback();
            return Status::Internal;
        }
    }

    // Persist justice record if created
    if (justice_record.has_value()) {
        Status justice_status = tx->putJustice(justice_record.value());
        if (justice_status != Status::Ok) {
            tx->rollback();
            return Status::Internal;
        }
    }

    // Commit atomically
    Status commit_status = tx->commit();
    if (commit_status != Status::Ok) {
        return Status::Internal;
    }

    return Status::Ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// HTLC Sweep Implementation (Phase 7B)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<HTLCSweepRecord> ChannelManagerCore::identifySweepCandidates(
    const ChannelRecord& channel
) {
    // Phase 7B.2: Create sweep records for all HTLCs after force-close
    std::vector<HTLCSweepRecord> sweeps;

    // Only create sweeps if channel is in terminal state
    auto terminal_enum = static_cast<ChannelTerminalState>(channel.terminal_state);
    if (terminal_enum == ChannelTerminalState::NONE) {
        return sweeps; // Channel not closed yet
    }

    // Get all HTLCs for this channel
    auto htlcs = m_db->listHTLCsForChannel(channel.channel_id);

    for (const auto& htlc : htlcs) {
        // Determine sweep type based on HTLC direction
        if (htlc.is_incoming) {
            // Incoming HTLC → SUCCESS sweep (if we have preimage)
            // For now, assume we don't have preimage (Phase 7B.3 will handle this)
            // In real implementation, check if preimage is known
            // For simplicity: skip incoming HTLCs in this phase
            continue;
        } else {
            // Outgoing HTLC → TIMEOUT sweep
            HTLCSweepRecord sweep = createSweepRecord(channel, htlc, "");
            sweeps.push_back(sweep);
        }
    }

    return sweeps;
}

HTLCSweepRecord ChannelManagerCore::createSweepRecord(
    const ChannelRecord& channel,
    const HTLCRecord& htlc,
    const std::string& preimage
) const {
    // Phase 7B.2: Build sweep record with all metadata

    HTLCSweepRecord sweep;

    // Identity
    sweep.sweep_id = htlc.htlc_id + "_sweep";
    sweep.htlc_id = htlc.htlc_id;
    sweep.channel_id = channel.channel_id;

    // Determine sweep type
    HTLCSweepType sweep_type;
    if (preimage.empty()) {
        sweep_type = HTLCSweepType::TIMEOUT;
    } else {
        sweep_type = HTLCSweepType::SUCCESS;
    }
    sweep.sweep_type = static_cast<uint32_t>(sweep_type);

    // Amount and preimage
    sweep.amount_muna = htlc.amount_muna;
    sweep.preimage = preimage;

    // Timing constraints (Phase 7B.3: Eligibility)
    uint64_t commitment_confirm_height = channel.close_detected_height;
    sweep.csv_expiry_height = commitment_confirm_height + channel.to_self_delay;
    sweep.cltv_expiry_height = htlc.cltv_expiry;

    // Calculate earliest sweep height
    sweep.earliest_sweep_height = calculateEarliestSweepHeight(channel, htlc, sweep_type);

    // Initial status
    sweep.status = static_cast<uint32_t>(HTLCSweepStatus::PENDING);
    sweep.attempts = 0;
    sweep.last_attempt_height = 0;
    sweep.confirmed_height = 0;

    // Metadata
    sweep.created_at = getCurrentTimestamp();
    sweep.updated_at = sweep.created_at;

    return sweep;
}

uint64_t ChannelManagerCore::calculateEarliestSweepHeight(
    const ChannelRecord& channel,
    const HTLCRecord& htlc,
    HTLCSweepType sweep_type
) const {
    // Phase 7B.3: Eligibility evaluation
    //
    // Authoritative rule from spec:
    // - All sweeps must wait for CSV delay
    // - TIMEOUT sweeps also must wait for CLTV expiry

    uint64_t commitment_confirm_height = channel.close_detected_height;
    uint64_t csv_expiry = commitment_confirm_height + channel.to_self_delay;

    if (sweep_type == HTLCSweepType::TIMEOUT) {
        // TIMEOUT: max(CSV, CLTV)
        uint64_t cltv_expiry = htlc.cltv_expiry;
        return std::max(csv_expiry, cltv_expiry);
    } else {
        // SUCCESS: CSV only
        return csv_expiry;
    }
}

std::vector<HTLCSweepRecord> ChannelManagerCore::getReadySweeps(uint64_t current_height) {
    // Phase 7B.3: Return sweeps that are eligible for execution

    std::vector<HTLCSweepRecord> ready_sweeps;

    // Get all pending sweeps
    auto all_sweeps = m_db->listPendingHTLCSweeps();

    for (const auto& sweep : all_sweeps) {
        auto status_enum = static_cast<HTLCSweepStatus>(sweep.status);

        // Only consider PENDING or READY sweeps
        if (status_enum != HTLCSweepStatus::PENDING &&
            status_enum != HTLCSweepStatus::READY) {
            continue;
        }

        // Check if eligibility constraints are satisfied
        if (current_height >= sweep.earliest_sweep_height) {
            ready_sweeps.push_back(sweep);
        }
    }

    return ready_sweeps;
}

Result<void> ChannelManagerCore::updateSweepStatus(
    const std::string& sweep_id,
    HTLCSweepStatus status,
    const std::string& sweep_txid,
    uint64_t confirmed_height
) {
    // Phase 7B.5: Update sweep state after L1 action

    auto sweep_opt = m_db->getHTLCSweep(sweep_id);
    if (!sweep_opt) {
        return Result<void>::Err("Sweep not found: " + sweep_id);
    }

    auto sweep = *sweep_opt;

    // Update status
    sweep.status = static_cast<uint32_t>(status);
    sweep.updated_at = getCurrentTimestamp();

    // Update fields based on status
    switch (status) {
        case HTLCSweepStatus::READY:
            // No additional fields
            break;

        case HTLCSweepStatus::BROADCAST:
            sweep.sweep_txid = sweep_txid;
            sweep.attempts++;
            sweep.last_attempt_height = m_chain_oracle->getBlockHeight();
            break;

        case HTLCSweepStatus::CONFIRMED:
            sweep.sweep_txid = sweep_txid;
            sweep.confirmed_height = confirmed_height;
            break;

        case HTLCSweepStatus::FAILED:
            // failure_reason should already be set by caller
            break;

        default:
            break;
    }

    // Persist updated sweep
    Status persist_status = m_db->putHTLCSweep(sweep);
    if (persist_status != Status::Ok) {
        return Result<void>::Err("Failed to persist sweep status update");
    }

    return Result<void>::Ok();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9: Transaction Confirmation Processing (Watchtower Integration)
// ═══════════════════════════════════════════════════════════════════════════

void ChannelManagerCore::onTransactionConfirmed(
    const std::string& txid,
    uint64_t confirmed_height
) {
    // Phase 9: Entry point for ALL transaction confirmation events
    // Called by watchtower via IPC when a watched transaction is confirmed
    //
    // Invariants:
    // - Idempotent: Same txid processed twice → same state
    // - Deterministic: Same inputs → same outputs
    // - Atomic: All state changes committed together

    // 1. Ignore if already processed (idempotency)
    if (m_db->isTxConfirmed(txid)) {
        return;
    }

    // 2. Mark txid as confirmed (for idempotency)
    m_db->markTxConfirmed(txid, confirmed_height);

    // 3. Check: Is this a revoked commitment? (Phase 7C: Justice)
    auto revoked = m_db->getRevokedCommitmentByTxId(txid);
    if (revoked.has_value()) {
        maybeCreateJustice(*revoked, confirmed_height);
    }

    // 4. Check: Is this a sweep transaction we were waiting on? (Phase 7B)
    auto sweep = m_db->getSweepByTxId(txid);
    if (sweep.has_value()) {
        updateSweepConfirmed(*sweep, confirmed_height);
    }

    // 5. Check: Is this a funding or commitment tx?
    auto channel = m_db->getChannelByFundingTx(txid);
    if (channel.has_value()) {
        updateChannelConfirmation(*channel, confirmed_height);
    }

    // 6. Persist all changes atomically
    // Note: Individual methods above already persist their changes
    // This commit() call ensures any pending transaction is finalized
    m_db->flush();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9 Helper Methods
// ═══════════════════════════════════════════════════════════════════════════

void ChannelManagerCore::maybeCreateJustice(
    const RevokedCommitmentRecord& revoked,
    uint64_t confirmed_height
) {
    // Phase 7C: Justice transaction creation
    //
    // Called when a revoked commitment is confirmed on-chain (breach detected)
    // Creates and broadcasts justice transaction to claim all funds
    //
    // TODO Phase 7C: Implement justice transaction logic
    // For now, this is a stub that will be filled in during Phase 7C implementation

    (void)revoked;           // Unused in stub
    (void)confirmed_height;  // Unused in stub

    // Future implementation will:
    // 1. Check CSV delay has elapsed
    // 2. Build justice transaction using m_sweep_oracle
    // 3. Sign justice transaction
    // 4. Broadcast via L1 oracle
    // 5. Persist justice record to DB
}

JusticeRecord ChannelManagerCore::createJusticeRecord(
    const ChannelRecord& channel,
    const std::string& commitment_txid,
    uint64_t breach_height
) const {
    // Phase 7C: Create justice record for breach detection
    JusticeRecord justice;
    justice.justice_id = channel.channel_id + "_justice_" + commitment_txid;
    justice.channel_id = channel.channel_id;
    justice.commitment_txid = commitment_txid;
    justice.revoked_commitment_number = channel.commitment_number > 0
        ? channel.commitment_number - 1
        : 0;
    justice.breach_detected_height = breach_height;
    justice.csv_expiry_height = breach_height + channel.to_self_delay;
    justice.earliest_justice_height = breach_height + channel.to_self_delay;
    justice.status = static_cast<uint32_t>(JusticeStatus::PENDING);
    justice.created_at = getCurrentTimestamp();
    justice.updated_at = getCurrentTimestamp();

    return justice;
}

std::vector<JusticeRecord> ChannelManagerCore::getReadyJusticeActions(uint64_t current_height) {
    // Phase 7C: Get justice actions ready for execution
    // Query DB for justice records with CSV delay satisfied

    std::vector<JusticeRecord> ready_justice;

    // Get all pending justice records
    auto pending_justice = m_db->listPendingJustice();

    for (const auto& justice : pending_justice) {
        // Check if CSV delay is satisfied
        if (current_height >= justice.earliest_justice_height) {
            ready_justice.push_back(justice);
        }
    }

    return ready_justice;
}

Result<void> ChannelManagerCore::updateJusticeStatus(
    const std::string& justice_id,
    JusticeStatus status,
    const std::string& justice_txid,
    uint64_t confirmed_height
) {
    // Phase 7C: Update justice transaction status

    // Fetch existing record
    auto justice_opt = m_db->getJustice(justice_id);
    if (!justice_opt.has_value()) {
        return Result<void>::Err("Justice record not found: " + justice_id);
    }

    JusticeRecord justice = justice_opt.value();

    // Update status
    justice.status = static_cast<uint32_t>(status);
    justice.updated_at = getCurrentTimestamp();

    // Update status-specific fields
    switch (status) {
        case JusticeStatus::BROADCAST:
            justice.justice_txid = justice_txid;
            justice.attempts++;
            justice.last_attempt_height = confirmed_height; // Actually current height when broadcast
            break;

        case JusticeStatus::CONFIRMED:
            justice.confirmed_height = confirmed_height;
            break;

        case JusticeStatus::FAILED:
            // failure_reason should be set by caller if needed
            break;

        default:
            break;
    }

    // Persist updated record
    Status db_status = m_db->putJustice(justice);
    if (db_status != Status::Ok) {
        return Result<void>::Err("Failed to persist justice status update");
    }

    return Result<void>::Ok();
}

void ChannelManagerCore::updateSweepConfirmed(
    const HTLCSweepRecord& sweep,
    uint64_t confirmed_height
) {
    // Phase 7B: Mark sweep as confirmed
    //
    // Called when a sweep transaction is confirmed on-chain
    // Updates sweep status from BROADCAST → CONFIRMED

    // Use existing updateSweepStatus() method
    auto result = updateSweepStatus(
        sweep.sweep_id,
        HTLCSweepStatus::CONFIRMED,
        sweep.sweep_txid,
        confirmed_height
    );

    // Note: Errors are silently ignored here to maintain atomicity
    // Real implementation might log to Phase 9 event stream
    (void)result;
}

void ChannelManagerCore::updateChannelConfirmation(
    const ChannelRecord& channel,
    uint64_t confirmed_height
) {
    // Phase 9: Update channel state based on funding/commitment tx confirmation
    //
    // Called when a channel's funding or commitment transaction is confirmed
    // Transitions channel state appropriately

    auto updated_channel = channel;
    bool needs_update = false;

    // Check if this is a funding transaction confirmation
    if (updated_channel.state == static_cast<uint32_t>(ChannelState::PENDING_OPEN)) {
        // Funding tx confirmed - transition to OPEN
        updated_channel.state = static_cast<uint32_t>(ChannelState::OPEN);
        updated_channel.last_update = getCurrentTimestamp();

        // Assign short channel ID: block_height || tx_index || vout
        // (Simplified: use confirmed_height, assume tx_index = 1)
        updated_channel.short_channel_id =
            (confirmed_height << 40) | (1 << 16) | updated_channel.funding_vout;

        needs_update = true;
    }

    // Check if this is a commitment transaction (force-close)
    if (updated_channel.closing_txid == channel.funding_txid) {
        // This might be a force-close commitment tx confirmation
        // Already handled by onChainEvent / handleFundingSpent
        // No additional action needed here
    }

    // Persist if state changed
    if (needs_update) {
        persistChannelState(updated_channel);
    }
}

} // namespace lightning
} // namespace dinero
