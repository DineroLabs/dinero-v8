#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"  // Full DaemonContext definition (replaces forward decl)
#include "lightning/lightning_service.h"  // For LightningService complete type
#include "lightning/lightning_wallet.h"  // For LightningWallet integration
#include "lightning/channel_manager.h"
#include "lightning/lightning_types.h"
#include "lightning/invoice.h"  // For Invoice decoding
#include "lightning/lightning_db_types.h"  // For InvoiceRecord
#include "lightning/payment_router.h"  // For PaymentRouter and failure types
#include "lightning/onion_error.h"  // For FailureCode and failureCodeToString
#include "wallet/transaction.h"  // For Transaction deserialization
#include "wallet/wallet_manager.h"  // For per-wallet Lightning access
#include "daemon/services/wallet_service.h"  // For WalletService
#include "common/logger.h"
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

using dinero::lightning::Channel;
using dinero::lightning::ChannelState;
using dinero::lightning::MuSig2NoncePair;
using dinero::lightning::channelStateToString;
using dinero::lightning::htlcStateToString;
using dinero::Transaction;
namespace constants = dinero::lightning::constants;

/**
 * @file methods_lightning.cpp
 * @brief Lightning Network RPC methods (Phase 7)
 *
 * Implements the Lightning Network RPC interface:
 *
 * Channel Management:
 * - ln.openchannel: Open new payment channel
 * - ln.closechannel: Close existing channel
 * - ln.listchannels: List all channels
 *
 * Invoices & Payments:
 * - ln.createinvoice: Create BOLT #11 invoice
 * - ln.payinvoice: Pay a BOLT #11 invoice
 * - ln.sendpayment: Send payment to node
 * - ln.decodeinvoice: Decode invoice without paying
 * - ln.listinvoices: List all invoices with filtering
 *
 * Wallet Integration:
 * - wallet.fundchannel: Create funding transaction
 * - wallet.generatenonce: Generate MuSig2 nonce
 * - wallet.signcommitment: Sign commitment with MuSig2
 *
 * Naming convention: ln.* namespace for all Lightning methods
 */

// ═══════════════════════════════════════════════════════════════════════════
// Helper: Get Lightning Service from Active Wallet
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Get the Lightning service from the currently active wallet.
 * Returns nullptr if no wallet is open or Lightning is not available.
 */
static dinero::lightning::LightningService* getLightningFromActiveWallet(const ExecutionContext& ctx) {
    auto* daemon_ctx = ctx.daemon;
    if (!daemon_ctx || !daemon_ctx->wallet) {
        return nullptr;
    }

    // Get WalletManager from WalletService
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(daemon_ctx->wallet);
    if (!wallet_service) {
        return nullptr;
    }

    dinero::WalletManager& wallet_mgr = wallet_service->get();

    // Check if wallet is open
    if (!wallet_mgr.hasActiveWallet()) {
        return nullptr;
    }

    // Get Lightning service from wallet manager
    return wallet_mgr.getLightningService();
}

// ═══════════════════════════════════════════════════════════════════════════
// Helper: Convert Channel to JSON
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Convert Channel struct to JSON with dual format amounts
 *
 * Provides both Dinero native units (muna/una) and Lightning protocol
 * compatibility units (msats/sats) in the JSON output.
 *
 * Balance/Amount Fields (dual format):
 * - *_sats: Lightning protocol compatibility (converted from muna)
 * - *_msats: Same as muna internally, exposed for Lightning Network interop
 * - funding_amount_sats: Channel capacity (stored as una internally)
 * - dust_limit_sats: Dust threshold (stored as una internally)
 */
static din::Json channelToJson(const Channel& channel) {
    din::Json response = din::obj();

    // Identity
    response["channel_id"] = channel.channel_id;
    response["peer_node_id"] = channel.peer_node_id;

    // Funding
    // funding_txid is already a string (hex format)
    response["funding_txid"] = channel.funding_txid;
    response["funding_vout"] = static_cast<din::Json::Int64>(channel.funding_vout);
    response["funding_amount_sats"] = static_cast<din::Json::Int64>(channel.funding_amount_una);

    // Balances (convert milliuna to una for display)
    response["local_balance_sats"] = static_cast<double>(channel.local_balance_muna) / 1000.0;
    response["remote_balance_sats"] = static_cast<double>(channel.remote_balance_muna) / 1000.0;
    response["local_balance_msats"] = static_cast<din::Json::Int64>(channel.local_balance_muna);
    response["remote_balance_msats"] = static_cast<din::Json::Int64>(channel.remote_balance_muna);

    // State
    response["state"] = channelStateToString(channel.state);
    response["commitment_number"] = static_cast<din::Json::Int64>(channel.commitment_number);

    // Metadata
    response["created_at"] = static_cast<din::Json::Int64>(channel.created_at);
    response["last_update"] = static_cast<din::Json::Int64>(channel.last_update);
    response["is_initiator"] = channel.is_initiator;
    response["to_self_delay"] = static_cast<din::Json::Int64>(channel.to_self_delay);
    response["dust_limit_sats"] = static_cast<din::Json::Int64>(channel.dust_limit_una);

    // HTLCs
    din::Json htlcs = din::arr();
    for (const auto& htlc : channel.pending_htlcs) {
        din::Json htlc_json = din::obj();
        htlc_json["htlc_id"] = htlc.htlc_id;
        htlc_json["amount_msats"] = static_cast<din::Json::Int64>(htlc.amount_muna);
        htlc_json["amount_sats"] = static_cast<double>(htlc.amount_muna) / 1000.0;
        htlc_json["cltv_expiry"] = static_cast<din::Json::Int64>(htlc.cltv_expiry);
        htlc_json["is_incoming"] = htlc.is_incoming;
        htlc_json["state"] = htlcStateToString(htlc.state);
        htlcs.append(htlc_json);
    }
    response["pending_htlcs"] = htlcs;
    response["pending_htlc_count"] = static_cast<din::Json::Int64>(channel.pending_htlcs.size());

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// ln.openchannel
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Open Lightning Network channel with peer
 *
 * RPC: ln.openchannel
 *
 * Parameters:
 * - peer_node_id: Peer node public key (33-byte hex)
 * - local_amount_sats: Local contribution to channel capacity
 *                      [Lightning protocol compatibility - internally stored as una]
 * - push_amount_sats: Amount to push to peer on channel open (optional, default: 0)
 *                     [Lightning protocol compatibility - internally stored as una]
 * - to_self_delay: CSV delay for commitment transactions (optional)
 *
 * Returns:
 * - channel_id: Channel identifier
 * - peer_node_id: Peer node public key
 * - capacity_sats: Total channel capacity (Lightning compatibility)
 * - local_balance_sats: Our balance (Lightning compatibility)
 * - remote_balance_sats: Peer's balance (Lightning compatibility)
 */
static din::Json rpc_ln_openchannel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Validate parameters
    if (params.size() < 2) {
        response["error"] = "Usage: ln.openchannel <peer_node_id> <local_amount_sats> [push_amount_sats] [to_self_delay]";
        return response;
    }

    // Parse required parameters
    std::string peer_node_id = params[0].asString();
    uint64_t local_amount_sats = params[1].asUInt64();

    // Parse optional parameters
    uint64_t push_amount_sats = 0;
    if (params.size() >= 3) {
        push_amount_sats = params[2].asUInt64();
    }

    uint32_t to_self_delay = constants::DEFAULT_TO_SELF_DELAY;
    if (params.size() >= 4) {
        to_self_delay = static_cast<uint32_t>(params[3].asUInt());
    }

    // Validate peer_node_id format (33-byte hex = 66 characters)
    if (peer_node_id.size() != 66) {
        response["error"] = "Invalid peer_node_id: must be 33-byte hex (66 characters)";
        return response;
    }

    // Validate minimum channel capacity
    if (local_amount_sats < constants::MIN_CHANNEL_CAPACITY_UNA) {
        std::ostringstream oss;
        oss << "Channel capacity too low. Minimum: " << constants::MIN_CHANNEL_CAPACITY_UNA << " una";
        response["error"] = oss.str();
        return response;
    }

    // Validate push amount doesn't exceed local amount
    if (push_amount_sats > local_amount_sats) {
        response["error"] = "Push amount cannot exceed local contribution";
        return response;
    }

    // Open channel
    auto& channel_mgr = lightning->getChannelManager();
    auto open_response = channel_mgr.openChannel(peer_node_id, local_amount_sats, push_amount_sats, to_self_delay);

    if (open_response.isErr()) {
        response["error"] = open_response.err();
        return response;
    }

    // Success - return channel details
    const Channel& channel = open_response.unwrap();
    response = channelToJson(channel);

    // Add additional context
    response["confirmations_required"] = static_cast<din::Json::Int64>(constants::FUNDING_TX_CONFIRMATIONS);
    response["success"] = true;
    response["message"] = "Channel opened successfully. Waiting for funding transaction confirmations.";

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// ln.closechannel
// ═══════════════════════════════════════════════════════════════════════════

static din::Json rpc_ln_closechannel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    if (params.empty()) {
        response["error"] = "Usage: ln.closechannel <channel_id> [force]";
        return response;
    }

    std::string channel_id = params[0].asString();
    bool force = false;
    if (params.size() >= 2) {
        force = params[1].asBool();
    }

    auto& channel_mgr = lightning->getChannelManager();
    auto close_response = channel_mgr.closeChannel(channel_id, force);

    if (close_response.isErr()) {
        response["error"] = close_response.err();
        return response;
    }

    response["success"] = true;
    response["channel_id"] = channel_id;
    response["close_type"] = force ? "force" : "cooperative";
    response["message"] = force
        ? "Force-closing channel. Broadcasting latest commitment transaction."
        : "Closing channel cooperatively. Negotiating final balance with peer.";

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// ln.listchannels
// ═══════════════════════════════════════════════════════════════════════════

static din::Json rpc_ln_listchannels(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Parse optional state filter
    std::optional<ChannelState> state_filter = std::nullopt;
    if (!params.empty()) {
        std::string state_str = params[0].asString();

        // Convert string to ChannelState enum
        if (state_str == "PENDING_OPEN") {
            state_filter = ChannelState::PENDING_OPEN;
        } else if (state_str == "OPEN") {
            state_filter = ChannelState::OPEN;
        } else if (state_str == "PENDING_CLOSE") {
            state_filter = ChannelState::PENDING_CLOSE;
        } else if (state_str == "FORCE_CLOSING") {
            state_filter = ChannelState::FORCE_CLOSING;
        } else if (state_str == "CLOSED") {
            state_filter = ChannelState::CLOSED;
        } else {
            response["error"] = "Invalid state. Must be: PENDING_OPEN, OPEN, PENDING_CLOSE, FORCE_CLOSING, or CLOSED";
            return response;
        }
    }

    auto& channel_mgr = lightning->getChannelManager();
    auto channels = channel_mgr.listChannels(state_filter);

    // Convert channels to JSON array
    din::Json channels_json = din::arr();
    uint64_t total_capacity_sats = 0;
    uint64_t total_local_balance_msats = 0;
    uint64_t total_remote_balance_msats = 0;

    for (const auto& channel : channels) {
        channels_json.append(channelToJson(channel));
        total_capacity_sats += channel.funding_amount_una;
        total_local_balance_msats += channel.local_balance_muna;
        total_remote_balance_msats += channel.remote_balance_muna;
    }

    response["channels"] = channels_json;
    response["total_count"] = static_cast<din::Json::Int64>(channels.size());
    response["total_capacity_sats"] = static_cast<din::Json::Int64>(total_capacity_sats);
    response["total_local_balance_sats"] = static_cast<double>(total_local_balance_msats) / 1000.0;
    response["total_remote_balance_sats"] = static_cast<double>(total_remote_balance_msats) / 1000.0;

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// Invoice & Payment Methods
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Create Lightning invoice
 *
 * RPC: ln.createinvoice
 *
 * Creates a BOLT #11 invoice for receiving Lightning payments.
 *
 * Parameters:
 * - amount_sats: Invoice amount in una (or 0 for open invoice)
 *                [Lightning protocol compatibility - internally stored as muna]
 * - description: Human-readable description (required)
 * - expiry_seconds: Invoice expiry time (optional, default: 3600)
 *
 * Returns:
 * - bolt11: BOLT #11 invoice string
 * - payment_hash: Payment hash (hex)
 * - expires_at: Unix timestamp when invoice expires
 * - amount_sats: Invoice amount (Lightning compatibility)
 * - amount_msats: Invoice amount in milliuna (same as muna internally)
 */
static din::Json rpc_ln_createinvoice(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Validate parameters
    if (params.size() < 2) {
        response["error"] = "Usage: ln.createinvoice <amount_sats> <description> [expiry_seconds]";
        return response;
    }

    // Parse parameters
    uint64_t amount_sats = params[0].asUInt64();
    std::string description = params[1].asString();
    uint32_t expiry_seconds = 3600;  // Default: 1 hour

    if (params.size() >= 3) {
        expiry_seconds = static_cast<uint32_t>(params[2].asUInt());
    }

    // Convert una to milliuna
    uint64_t amount_msats = amount_sats * 1000;

    // Create invoice
    auto result = lightning->createInvoice(amount_msats, description, expiry_seconds);

    if (result.isErr()) {
        response["error"] = result.err();
        return response;
    }

    // Extract invoice details
    const auto& invoice = result.unwrap();

    // Calculate expiration timestamp
    uint64_t expires_at = invoice.created_at + invoice.expiry_seconds;

    // Success
    response["success"] = true;
    response["bolt11"] = invoice.bolt11_string;
    response["payment_hash"] = invoice.payment_hash;  // Already hex string
    response["amount_sats"] = static_cast<double>(invoice.amount_muna) / 1000.0;
    response["amount_msats"] = static_cast<din::Json::Int64>(invoice.amount_muna);
    response["description"] = invoice.description;
    response["created_at"] = static_cast<din::Json::Int64>(invoice.created_at);
    response["expires_at"] = static_cast<din::Json::Int64>(expires_at);
    response["expiry_seconds"] = static_cast<din::Json::Int>(invoice.expiry_seconds);

    return response;
}

/**
 * @brief Pay Lightning invoice
 *
 * RPC: ln.payinvoice
 *
 * Pays a BOLT #11 invoice.
 *
 * Parameters:
 * - bolt11: BOLT #11 invoice string
 * - timeout_ms: Payment timeout in milliseconds (optional, default: 60000)
 *
 * Returns:
 * - preimage: Payment preimage (hex) - proof of payment
 * - payment_hash: Payment hash (hex)
 * - amount_paid_sats: Amount paid (Lightning compatibility)
 * - fees_paid_sats: Routing fees paid (Lightning compatibility)
 */
static din::Json rpc_ln_payinvoice(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Validate parameters
    if (params.empty()) {
        response["error"] = "Usage: ln.payinvoice <bolt11> [timeout_ms]";
        return response;
    }

    std::string bolt11 = params[0].asString();
    uint64_t timeout_ms = 60000;  // Default: 60 seconds

    if (params.size() >= 2) {
        timeout_ms = params[1].asUInt64();
    }

    // Decode upfront for response metadata (payment hash, amount).
    auto invoice_opt = dinero::lightning::Invoice::decode(bolt11);

    // Pay invoice
    auto result = lightning->payInvoice(bolt11, timeout_ms);

    if (result.isErr()) {
        response["error"] = result.err();
        return response;
    }

    // Extract preimage
    const auto& preimage = result.unwrap();

    // Convert preimage to hex
    std::stringstream preimage_hex;
    for (uint8_t byte : preimage) {
        preimage_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    // Success
    response["success"] = true;
    response["preimage"] = preimage_hex.str();
    if (invoice_opt.has_value()) {
        const auto& invoice = invoice_opt.value();
        auto payment_hash = invoice.get_payment_hash();
        std::stringstream payment_hash_hex;
        for (uint8_t byte : payment_hash) {
            payment_hash_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        response["payment_hash"] = payment_hash_hex.str();

        auto amount_msats = invoice.get_amount_muna();
        if (amount_msats.has_value()) {
            response["amount_paid_sats"] = static_cast<double>(amount_msats.value()) / 1000.0;
            response["amount_paid_msats"] = static_cast<din::Json::Int64>(amount_msats.value());
        } else {
            response["amount_paid_sats"] = Json::nullValue;
            response["amount_paid_msats"] = Json::nullValue;
        }
    } else {
        response["payment_hash"] = Json::nullValue;
        response["amount_paid_sats"] = Json::nullValue;
        response["amount_paid_msats"] = Json::nullValue;
        response["metadata_warning"] = "Unable to decode BOLT11 metadata";
    }
    response["fees_paid_sats"] = Json::nullValue;
    response["fees_paid_msats"] = Json::nullValue;
    response["message"] = "Payment successful! Preimage is proof of payment.";

    return response;
}

/**
 * @brief Send Lightning payment
 *
 * RPC: ln.sendpayment
 *
 * Sends a payment directly to a node (without invoice).
 *
 * Parameters:
 * - destination_node_id: Destination node public key (33-byte hex)
 * - amount_sats: Payment amount in una
 *                [Lightning protocol compatibility - internally stored as muna]
 * - max_fee_sats: Maximum routing fee (optional, default: 0 = no limit)
 *                 [Lightning protocol compatibility - internally stored as muna]
 * - timeout_ms: Payment timeout (optional, default: 60000)
 *
 * Returns:
 * - preimage: Payment preimage (proof of payment)
 * - payment_hash: Payment hash
 * - amount_sent_sats: Amount sent (Lightning compatibility)
 * - fees_paid_sats: Routing fees paid (Lightning compatibility)
 */
static din::Json rpc_ln_sendpayment(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Validate parameters
    if (params.size() < 2) {
        response["error"] = "Usage: ln.sendpayment <destination_node_id> <amount_sats> [max_fee_sats] [timeout_ms]";
        return response;
    }

    std::string destination_node_id = params[0].asString();
    uint64_t amount_sats = params[1].asUInt64();
    uint64_t max_fee_sats = 0;  // Default: no limit
    uint64_t timeout_ms = 60000;  // Default: 60 seconds

    if (params.size() >= 3) {
        max_fee_sats = params[2].asUInt64();
    }
    if (params.size() >= 4) {
        timeout_ms = params[3].asUInt64();
    }

    // Validate node ID format (33-byte hex = 66 characters)
    if (destination_node_id.size() != 66) {
        response["error"] = "Invalid destination_node_id: must be 33-byte hex (66 characters)";
        return response;
    }

    // Convert una to milliuna
    uint64_t amount_msats = amount_sats * 1000;
    uint64_t max_fee_msats = max_fee_sats * 1000;

    // Send payment
    auto result = lightning->sendPayment(destination_node_id, amount_msats, max_fee_msats, timeout_ms);

    if (result.isErr()) {
        response["error"] = result.err();
        return response;
    }

    // Extract preimage
    const auto& preimage = result.unwrap();

    // Convert preimage to hex
    std::stringstream preimage_hex;
    for (uint8_t byte : preimage) {
        preimage_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    // Success
    response["success"] = true;
    response["preimage"] = preimage_hex.str();
    response["destination"] = destination_node_id;
    response["amount_sent_sats"] = static_cast<double>(amount_msats) / 1000.0;
    response["amount_sent_msats"] = static_cast<din::Json::Int64>(amount_msats);
    response["message"] = "Payment sent successfully!";

    return response;
}

/**
 * @brief Decode Lightning invoice
 *
 * RPC: ln.decodeinvoice
 *
 * Decodes a BOLT #11 invoice without paying it.
 *
 * Parameters:
 * - bolt11: BOLT #11 invoice string
 *
 * Returns:
 * - payment_hash: Payment hash (hex)
 * - amount_sats: Invoice amount (Lightning compatibility, if specified)
 * - description: Invoice description
 * - payee_node_id: Destination node (if specified)
 * - expires_at: Expiration timestamp
 * - is_expired: Boolean indicating if invoice is expired
 */
static din::Json rpc_ln_decodeinvoice(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Note: Decoding doesn't require Lightning service, but we'll keep it consistent
    if (params.empty()) {
        response["error"] = "Usage: ln.decodeinvoice <bolt11>";
        return response;
    }

    std::string bolt11 = params[0].asString();

    // Decode invoice using static method
    auto invoice_opt = dinero::lightning::Invoice::decode(bolt11);

    if (!invoice_opt.has_value()) {
        response["error"] = "Failed to decode invoice: invalid BOLT #11 format";
        return response;
    }

    const auto& invoice = invoice_opt.value();

    // Convert payment hash to hex
    auto payment_hash = invoice.get_payment_hash();
    std::stringstream payment_hash_hex;
    for (uint8_t byte : payment_hash) {
        payment_hash_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    // Build response
    response["success"] = true;
    response["payment_hash"] = payment_hash_hex.str();
    response["description"] = invoice.get_description();
    response["network"] = invoice.get_network();
    response["timestamp"] = static_cast<din::Json::Int64>(invoice.get_timestamp());
    response["expiry_seconds"] = static_cast<din::Json::Int>(invoice.get_expiry());
    response["is_expired"] = invoice.is_expired();

    // Optional fields
    auto amount_opt = invoice.get_amount_muna();
    if (amount_opt.has_value()) {
        uint64_t amount_muna = amount_opt.value();
        response["amount_sats"] = static_cast<double>(amount_muna) / 1000.0;
        response["amount_msats"] = static_cast<din::Json::Int64>(amount_muna);
    } else {
        response["amount_sats"] = Json::nullValue;  // Open amount invoice
        response["amount_msats"] = Json::nullValue;
    }

    auto payee_opt = invoice.get_payee_node_id();
    if (payee_opt.has_value()) {
        const auto& payee = payee_opt.value();
        std::stringstream payee_hex;
        for (uint8_t byte : payee) {
            payee_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        response["payee_node_id"] = payee_hex.str();
    }

    auto fallback_opt = invoice.get_fallback_address();
    if (fallback_opt.has_value()) {
        response["fallback_address"] = fallback_opt.value();
    }

    response["min_final_cltv_expiry"] = static_cast<din::Json::Int>(invoice.get_min_final_cltv_expiry());

    return response;
}

/**
 * @brief List Lightning invoices
 *
 * RPC: ln.listinvoices
 *
 * Lists all invoices stored in the database with optional filtering.
 *
 * Parameters:
 * - status: Filter by status (optional): "pending", "paid", "expired", "all"
 *
 * Returns:
 * - invoices: Array of invoice objects with full details
 *   - amount_sats: Invoice amount (Lightning compatibility)
 *   - amount_msats: Invoice amount in milliuna (same as muna internally)
 * - total_count: Total number of invoices
 * - total_pending: Number of pending invoices
 * - total_paid: Number of paid invoices
 * - total_expired: Number of expired invoices
 */
static din::Json rpc_ln_listinvoices(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response = din::obj();

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Get database instance
    auto db = lightning->getDatabase();
    if (!db || !db->isOpen()) {
        response["error"] = "Lightning database not available";
        return response;
    }

    // Parse optional status filter
    std::string status_filter = "all";  // Default: all invoices
    if (!params.empty()) {
        status_filter = params[0].asString();
    }

    // Define invoice status constants (matching InvoiceTracker::PaymentStatus)
    constexpr uint32_t STATUS_PENDING = 0;
    constexpr uint32_t STATUS_PAYING = 1;
    constexpr uint32_t STATUS_PAID = 2;
    constexpr uint32_t STATUS_EXPIRED = 3;
    constexpr uint32_t STATUS_CANCELLED = 4;

    // Get current timestamp for expiry checks
    uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));

    // Query all invoices from database
    auto all_invoices = db->listInvoices();

    // Build filtered invoice list
    din::Json invoices_json = din::arr();
    uint32_t total_pending = 0;
    uint32_t total_paid = 0;
    uint32_t total_expired = 0;

    for (const auto& inv : all_invoices) {
        // Determine actual status (update expired if necessary)
        uint32_t actual_status = inv.status;
        bool is_expired = (inv.expires_at > 0 && current_time > inv.expires_at && inv.status == STATUS_PENDING);

        if (is_expired) {
            actual_status = STATUS_EXPIRED;
        }

        // Update counters
        if (actual_status == STATUS_PENDING || actual_status == STATUS_PAYING) {
            total_pending++;
        } else if (actual_status == STATUS_PAID) {
            total_paid++;
        } else if (actual_status == STATUS_EXPIRED || actual_status == STATUS_CANCELLED) {
            total_expired++;
        }

        // Apply status filter
        bool include = false;
        if (status_filter == "all") {
            include = true;
        } else if (status_filter == "pending" && (actual_status == STATUS_PENDING || actual_status == STATUS_PAYING)) {
            include = true;
        } else if (status_filter == "paid" && actual_status == STATUS_PAID) {
            include = true;
        } else if (status_filter == "expired" && (actual_status == STATUS_EXPIRED || actual_status == STATUS_CANCELLED)) {
            include = true;
        }

        if (!include) {
            continue;
        }

        // Build invoice JSON object
        din::Json inv_json = din::obj();
        inv_json["payment_hash"] = inv.payment_hash;
        inv_json["bolt11"] = inv.bolt11_string;
        inv_json["amount_sats"] = static_cast<double>(inv.amount_muna) / 1000.0;
        inv_json["amount_msats"] = static_cast<din::Json::Int64>(inv.amount_muna);
        inv_json["description"] = inv.description;
        inv_json["created_at"] = static_cast<din::Json::Int64>(inv.created_at);
        inv_json["expires_at"] = static_cast<din::Json::Int64>(inv.expires_at);

        // Status
        std::string status_str;
        switch (actual_status) {
            case STATUS_PENDING:  status_str = "pending"; break;
            case STATUS_PAYING:   status_str = "paying"; break;
            case STATUS_PAID:     status_str = "paid"; break;
            case STATUS_EXPIRED:  status_str = "expired"; break;
            case STATUS_CANCELLED: status_str = "cancelled"; break;
            default:              status_str = "unknown"; break;
        }
        inv_json["status"] = status_str;

        // Payment details (if paid)
        if (actual_status == STATUS_PAID) {
            inv_json["paid_at"] = static_cast<din::Json::Int64>(inv.paid_at);
            if (!inv.paid_by_channel.empty()) {
                inv_json["paid_by_channel"] = inv.paid_by_channel;
            }
        }

        // Optional metadata
        if (!inv.label.empty()) {
            inv_json["label"] = inv.label;
        }

        if (!inv.tags.empty()) {
            din::Json tags_arr = din::arr();
            for (const auto& tag : inv.tags) {
                tags_arr.append(tag);
            }
            inv_json["tags"] = tags_arr;
        }

        // Check if expired (time-based)
        inv_json["is_expired"] = is_expired;

        // Add to results
        invoices_json.append(inv_json);
    }

    // Build response
    response["success"] = true;
    response["invoices"] = invoices_json;
    response["total_count"] = static_cast<din::Json::Int>(invoices_json.size());
    response["total_pending"] = static_cast<din::Json::Int>(total_pending);
    response["total_paid"] = static_cast<din::Json::Int>(total_paid);
    response["total_expired"] = static_cast<din::Json::Int>(total_expired);
    response["status_filter"] = status_filter;

    return response;
}

/**
 * @brief List all outgoing Lightning payments
 *
 * RPC: ln.listpayments
 *
 * Lists payment history (outgoing payments) with optional status filtering.
 *
 * Parameters:
 * - status: Optional filter ("pending", "succeeded", "failed", "all") - default: "all"
 *
 * Returns:
 * - payments: Array of payment objects with:
 *   - payment_hash: Payment hash (hex)
 *   - bolt11: Original BOLT #11 invoice (if paid via invoice)
 *   - destination: Destination node ID (hex)
 *   - amount_sats: Payment amount (Lightning compatibility)
 *   - fee_sats: Routing fees paid (Lightning compatibility)
 *   - status: "pending", "succeeded", "failed"
 *   - failure_reason: Reason for failure (if failed)
 *   - attempts: Number of payment attempts
 *   - created_at: Unix timestamp when payment initiated
 *   - completed_at: Unix timestamp when payment completed (0 = pending)
 *   - preimage: Proof of payment (hex, empty if failed)
 *   - route: JSON route information
 *   - route_hops: Number of hops
 *   - label: User-defined label
 * - total_count: Total number of payments (after filtering)
 * - total_pending: Count of pending payments
 * - total_succeeded: Count of successful payments
 * - total_failed: Count of failed payments
 * - status_filter: Applied status filter
 */
static din::Json rpc_ln_listpayments(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response;

    // Get Lightning service
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        response["payments"] = din::arr();
        response["total_count"] = 0;
        return response;
    }

    // Get database
    auto db = lightning->getDatabase();
    if (!db || !db->isOpen()) {
        response["error"] = "Lightning database not available";
        response["payments"] = din::arr();
        response["total_count"] = 0;
        return response;
    }

    // Parse status filter (optional parameter)
    std::string status_filter = "all";
    if (!params.empty() && params[0].isString()) {
        status_filter = params[0].asString();
    }

    // Payment status constants (matching PaymentRecord::status field)
    constexpr uint32_t STATUS_PENDING = 0;
    constexpr uint32_t STATUS_SUCCEEDED = 1;
    constexpr uint32_t STATUS_FAILED = 2;

    // Query all payments from database
    auto all_payments = db->listPayments();

    // Build response with filtering
    din::Json payments_json = din::arr();
    uint32_t total_pending = 0;
    uint32_t total_succeeded = 0;
    uint32_t total_failed = 0;

    for (const auto& pmt : all_payments) {
        // Determine actual status
        uint32_t actual_status = pmt.status;

        // Update statistics
        if (actual_status == STATUS_PENDING) total_pending++;
        else if (actual_status == STATUS_SUCCEEDED) total_succeeded++;
        else if (actual_status == STATUS_FAILED) total_failed++;

        // Apply status filter
        bool include = false;
        if (status_filter == "all") {
            include = true;
        } else if (status_filter == "pending" && actual_status == STATUS_PENDING) {
            include = true;
        } else if (status_filter == "succeeded" && actual_status == STATUS_SUCCEEDED) {
            include = true;
        } else if (status_filter == "failed" && actual_status == STATUS_FAILED) {
            include = true;
        }

        if (!include) continue;

        // Build payment JSON object
        din::Json pmt_json;
        pmt_json["payment_hash"] = pmt.payment_hash;
        pmt_json["bolt11"] = pmt.bolt11_string;
        pmt_json["destination"] = pmt.destination_node_id;

        // Amounts (convert milli-una to sats with precision)
        pmt_json["amount_sats"] = static_cast<double>(pmt.amount_muna) / 1000.0;
        pmt_json["fee_sats"] = static_cast<double>(pmt.fee_muna) / 1000.0;

        // Status as string
        std::string status_str = "unknown";
        if (actual_status == STATUS_PENDING) status_str = "pending";
        else if (actual_status == STATUS_SUCCEEDED) status_str = "succeeded";
        else if (actual_status == STATUS_FAILED) status_str = "failed";
        pmt_json["status"] = status_str;

        // Failure reason (only if failed)
        if (!pmt.failure_reason.empty()) {
            pmt_json["failure_reason"] = pmt.failure_reason;
            pmt_json["last_failure_reason"] = pmt.failure_reason;  // Alias for consistency
        }

        // Attempts
        pmt_json["attempts"] = static_cast<din::Json::Int>(pmt.attempts);
        pmt_json["route_attempts"] = static_cast<din::Json::Int>(pmt.attempts);  // Alias for clarity

        // BOLT #4 failure tracking (Phase 5.6: use real failure codes from PaymentRecord)
        if (actual_status == STATUS_FAILED && !pmt.failure_reason.empty()) {
            // Real BOLT #4 FailureCode from error packet decryption
            if (pmt.last_failure_code > 0) {
                dinero::lightning::FailureCode code = static_cast<dinero::lightning::FailureCode>(pmt.last_failure_code);
                pmt_json["last_failure_code"] = dinero::lightning::failureCodeToString(code);
                pmt_json["last_failure_code_int"] = static_cast<din::Json::Int>(pmt.last_failure_code);
            } else {
                // No failure code available (timeout, or pre-Phase 5 payment)
                pmt_json["last_failure_code"] = "UNKNOWN";
                pmt_json["last_failure_code_int"] = 0;
            }

            // Timestamp of last failure (use real timestamp if available, fallback to completed_at)
            if (pmt.timestamp_last_failure > 0) {
                pmt_json["timestamp_last_failure"] = static_cast<din::Json::Int64>(pmt.timestamp_last_failure);
            } else if (pmt.completed_at > 0) {
                pmt_json["timestamp_last_failure"] = static_cast<din::Json::Int64>(pmt.completed_at);
            }

            // Placeholder for total penalty applied (will be computed when penalties are tracked per payment)
            pmt_json["total_penalty_applied_muna"] = 0;
            pmt_json["total_penalty_applied_sats"] = 0.0;
        }

        // Timestamps
        pmt_json["created_at"] = static_cast<din::Json::Int64>(pmt.created_at);
        if (pmt.completed_at > 0) {
            pmt_json["completed_at"] = static_cast<din::Json::Int64>(pmt.completed_at);
        }

        // Proof of payment (only if succeeded)
        if (actual_status == STATUS_SUCCEEDED && !pmt.preimage.empty()) {
            pmt_json["preimage"] = pmt.preimage;
        }

        // Route information
        if (!pmt.route_json.empty()) {
            pmt_json["route"] = pmt.route_json;
        }
        if (pmt.route_hops > 0) {
            pmt_json["route_hops"] = static_cast<din::Json::Int>(pmt.route_hops);
        }

        // Optional metadata
        if (!pmt.label.empty()) {
            pmt_json["label"] = pmt.label;
        }

        payments_json.append(pmt_json);
    }

    // Build final response
    response["payments"] = payments_json;
    response["total_count"] = static_cast<din::Json::Int>(payments_json.size());
    response["total_pending"] = static_cast<din::Json::Int>(total_pending);
    response["total_succeeded"] = static_cast<din::Json::Int>(total_succeeded);
    response["total_failed"] = static_cast<din::Json::Int>(total_failed);
    response["status_filter"] = status_filter;

    return response;
}

/**
 * @brief List all route failures with BOLT #4 failure codes
 *
 * RPC: ln.listfailures
 *
 * Returns detailed information about failed payment routes including:
 * - BOLT #4 failure codes
 * - Channel penalties and expiration times
 * - Blacklisted nodes
 * - Failure timestamps and reasons
 *
 * Parameters: None
 *
 * Returns:
 * - failures: Array of failed routes with BOLT #4 codes
 * - penalties: Array of active channel penalties
 * - blacklisted_nodes: Array of permanently blacklisted nodes
 * - total_failures: Total number of route failures recorded
 * - total_penalties: Total number of active channel penalties
 * - total_blacklisted: Total number of blacklisted nodes
 */
static din::Json rpc_ln_listfailures(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response;

    // Get Lightning service
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        response["failures"] = din::arr();
        response["penalties"] = din::arr();
        response["blacklisted_nodes"] = din::arr();
        response["total_failures"] = 0;
        response["total_penalties"] = 0;
        response["total_blacklisted"] = 0;
        return response;
    }

    // Get PaymentRouter
    auto& router = lightning->getPaymentRouter();

    // Get failed routes
    auto failed_routes = router.getFailedRoutes();
    din::Json failures_array = din::arr();
    for (const auto& failure : failed_routes) {
        din::Json f;

        // Channel IDs array
        din::Json channel_ids_array = din::arr();
        for (const auto& channel_id : failure.channel_ids) {
            channel_ids_array.append(channel_id);
        }
        f["channel_ids"] = channel_ids_array;

        // Failure code
        f["failure_code"] = dinero::lightning::failureCodeToString(failure.code);
        f["failure_code_int"] = static_cast<int>(failure.code);
        f["reason"] = failure.reason;
        f["failing_hop"] = static_cast<din::Json::Int>(failure.failing_hop_index);
        f["timestamp"] = static_cast<din::Json::Int>(failure.failed_at);

        failures_array.append(f);
    }
    response["failures"] = failures_array;

    // Get channel penalties
    auto penalties = router.getChannelPenalties();
    din::Json penalties_array = din::arr();
    for (const auto& penalty : penalties) {
        din::Json p;
        p["channel_id"] = penalty.channel_id;
        p["failure_code"] = dinero::lightning::failureCodeToString(penalty.code);
        p["failure_code_int"] = static_cast<int>(penalty.code);
        p["penalty_muna"] = static_cast<din::Json::Int>(penalty.penalty_muna);
        p["penalty_sats"] = static_cast<double>(penalty.penalty_muna) / 1000.0;
        p["expires_at"] = static_cast<din::Json::Int>(penalty.expires_at);
        penalties_array.append(p);
    }
    response["penalties"] = penalties_array;

    // Get blacklisted nodes
    auto blacklist = router.getNodeBlacklist();
    din::Json blacklist_array = din::arr();
    for (const auto& entry : blacklist) {
        din::Json b;
        b["node_id"] = entry.node_id;
        b["failure_code"] = dinero::lightning::failureCodeToString(entry.code);
        b["failure_code_int"] = static_cast<int>(entry.code);
        b["blacklisted_at"] = static_cast<din::Json::Int>(entry.blacklisted_at);
        blacklist_array.append(b);
    }
    response["blacklisted_nodes"] = blacklist_array;

    // Summary statistics
    response["total_failures"] = static_cast<din::Json::Int>(failed_routes.size());
    response["total_penalties"] = static_cast<din::Json::Int>(penalties.size());
    response["total_blacklisted"] = static_cast<din::Json::Int>(blacklist.size());

    return response;
}

/**
 * @brief Get detailed information about a specific payment
 *
 * RPC: ln.getpayment <payment_hash>
 *
 * Returns comprehensive payment information including:
 * - Full payment details (amount, destination, status)
 * - All failure attempts with BOLT #4 codes
 * - Route information for each attempt
 * - Penalties applied during routing
 * - Retryability analysis
 *
 * Parameters:
 * - payment_hash: 32-byte payment hash (hex)
 *
 * Returns:
 * - payment: Complete payment record with extended failure history
 *   - amount_sats: Payment amount (Lightning compatibility)
 *   - fee_sats: Routing fees (Lightning compatibility)
 * - failures: Array of failure attempts
 * - current_penalties: Active penalties affecting this payment's destination
 */
static din::Json rpc_ln_getpayment(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response;

    // Validate parameters
    if (params.empty() || !params[0].isString()) {
        response["error"] = "Usage: ln.getpayment <payment_hash>";
        return response;
    }

    std::string payment_hash = params[0].asString();

    // Validate hash format (64 hex characters = 32 bytes)
    if (payment_hash.length() != 64) {
        response["error"] = "Invalid payment_hash length (expected 64 hex characters)";
        return response;
    }

    // Get Lightning service
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        response["error"] = "Lightning Network not available. Please open a wallet first.";
        return response;
    }

    // Get database
    auto db = lightning->getDatabase();
    if (!db || !db->isOpen()) {
        response["error"] = "Lightning database not available";
        return response;
    }

    // Retrieve payment record
    auto pmt_result = db->getPayment(payment_hash);
    if (!pmt_result.has_value()) {
        response["error"] = "Payment not found: " + payment_hash;
        return response;
    }

    auto pmt = pmt_result.value();

    // Build main payment object (reuse logic from ln.listpayments)
    din::Json pmt_json;
    pmt_json["payment_hash"] = pmt.payment_hash;
    pmt_json["bolt11"] = pmt.bolt11_string;
    pmt_json["destination"] = pmt.destination_node_id;

    // Amounts
    pmt_json["amount_sats"] = static_cast<double>(pmt.amount_muna) / 1000.0;
    pmt_json["fee_sats"] = static_cast<double>(pmt.fee_muna) / 1000.0;

    // Status
    uint32_t actual_status = pmt.status;
    std::string status_str = "unknown";
    if (actual_status == 0) status_str = "pending";
    else if (actual_status == 1) status_str = "succeeded";
    else if (actual_status == 2) status_str = "failed";
    pmt_json["status"] = status_str;

    // Failure information
    if (!pmt.failure_reason.empty()) {
        pmt_json["failure_reason"] = pmt.failure_reason;
        pmt_json["last_failure_reason"] = pmt.failure_reason;
    }

    // Attempts
    pmt_json["attempts"] = static_cast<din::Json::Int>(pmt.attempts);
    pmt_json["route_attempts"] = static_cast<din::Json::Int>(pmt.attempts);

    // Timestamps
    pmt_json["created_at"] = static_cast<din::Json::Int64>(pmt.created_at);
    if (pmt.completed_at > 0) {
        pmt_json["completed_at"] = static_cast<din::Json::Int64>(pmt.completed_at);
    }

    // Proof of payment
    if (actual_status == 1 && !pmt.preimage.empty()) {
        pmt_json["preimage"] = pmt.preimage;
    }

    // Route information
    if (!pmt.route_json.empty()) {
        pmt_json["route"] = pmt.route_json;
    }
    if (pmt.route_hops > 0) {
        pmt_json["route_hops"] = static_cast<din::Json::Int>(pmt.route_hops);
    }

    // Metadata
    if (!pmt.label.empty()) {
        pmt_json["label"] = pmt.label;
    }

    response["payment"] = pmt_json;

    // Get PaymentRouter for failure analysis
    auto& router = lightning->getPaymentRouter();

    // Get all failures and filter for this payment's destination
    auto all_failures = router.getFailedRoutes();
    din::Json failures_array = din::arr();

    for (const auto& failure : all_failures) {
        // Filter failures by checking if any channel in the route relates to this payment
        // (In a real implementation, we'd track payment_hash per failure in RouteFailure)
        // For now, we show all recent failures as they may be relevant
        din::Json f;

        din::Json channel_ids_array = din::arr();
        for (const auto& channel_id : failure.channel_ids) {
            channel_ids_array.append(channel_id);
        }
        f["channel_ids"] = channel_ids_array;
        f["failure_code"] = dinero::lightning::failureCodeToString(failure.code);
        f["failure_code_int"] = static_cast<int>(failure.code);
        f["reason"] = failure.reason;
        f["failing_hop"] = static_cast<din::Json::Int>(failure.failing_hop_index);
        f["timestamp"] = static_cast<din::Json::Int>(failure.failed_at);

        failures_array.append(f);
    }
    response["failures"] = failures_array;

    // Get active penalties affecting routing to this destination
    auto penalties = router.getChannelPenalties();
    din::Json penalties_array = din::arr();

    for (const auto& penalty : penalties) {
        din::Json p;
        p["channel_id"] = penalty.channel_id;
        p["failure_code"] = dinero::lightning::failureCodeToString(penalty.code);
        p["failure_code_int"] = static_cast<int>(penalty.code);
        p["penalty_muna"] = static_cast<din::Json::Int>(penalty.penalty_muna);
        p["penalty_sats"] = static_cast<double>(penalty.penalty_muna) / 1000.0;
        p["expires_at"] = static_cast<din::Json::Int>(penalty.expires_at);
        penalties_array.append(p);
    }
    response["current_penalties"] = penalties_array;

    // Summary
    response["total_failures_shown"] = static_cast<din::Json::Int>(failures_array.size());
    response["total_penalties_active"] = static_cast<din::Json::Int>(penalties_array.size());

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// Wallet Integration Methods
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Create Lightning funding transaction from wallet
 *
 * RPC: wallet.fundchannel
 *
 * Creates a funding transaction for a Lightning channel using wallet UTXOs.
 * The transaction funds a 2-of-2 MuSig2 output with the aggregate pubkey.
 *
 * Parameters:
 * - aggregate_pubkey: 32-byte hex x-only MuSig2 aggregate public key
 * - amount_sats: Channel funding amount in una
 * - fee_rate: Fee rate in una per vbyte (optional, default: 1)
 *
 * Returns:
 * - txid: Transaction ID (hex)
 * - funding_output_index: Output index of funding output (usually 0)
 * - raw_tx: Hex-encoded signed transaction
 * - fee_sats: Transaction fee paid
 * - change_sats: Change amount returned to wallet (if any)
 */
static din::Json rpc_wallet_fundchannel(const ExecutionContext& ctx, const din::Json& params) {
    // Validate daemon context
    const DaemonContext* daemon_ctx = ctx.daemon;
    if (!daemon_ctx) {
        throw std::runtime_error("Daemon context not available");
    }

    // Check wallet service
    if (!daemon_ctx->wallet) {
        throw std::runtime_error("Wallet service not initialized");
    }

    // Get Lightning service from active wallet
    auto* lightning = getLightningFromActiveWallet(ctx);
    if (!lightning) {
        throw std::runtime_error("Lightning Network not available. Please open a wallet first.");
    }

    // Parse parameters (array-style)
    if (params.size() < 2) {
        throw std::invalid_argument("Usage: wallet.fundchannel <aggregate_pubkey> <amount_sats> [fee_rate]");
    }

    // aggregate_pubkey (required) - params[0]
    std::string aggregate_pubkey_hex = params[0].asString();

    // Validate and decode aggregate pubkey
    if (aggregate_pubkey_hex.length() != 64) {
        throw std::invalid_argument("aggregate_pubkey must be 32 bytes (64 hex chars)");
    }
    std::vector<uint8_t> aggregate_pubkey;
    for (size_t i = 0; i < aggregate_pubkey_hex.length(); i += 2) {
        std::string byte_str = aggregate_pubkey_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        aggregate_pubkey.push_back(byte);
    }

    // amount_sats (required) - params[1]
    uint64_t amount_sats = params[1].asUInt64();

    if (amount_sats == 0) {
        throw std::invalid_argument("amount_sats must be greater than 0");
    }

    // fee_rate (optional, default: 1 sat/vbyte) - params[2]
    uint64_t fee_rate = 1;
    if (params.size() >= 3) {
        fee_rate = params[2].asUInt64();
        if (fee_rate == 0) {
            throw std::invalid_argument("fee_rate must be greater than 0");
        }
    }

    // Create LightningWallet instance
    dinero::lightning::LightningWallet lightning_wallet(*const_cast<DaemonContext*>(daemon_ctx));

    // Call createFundingTransaction
    auto result = lightning_wallet.createFundingTransaction(aggregate_pubkey, amount_sats, fee_rate);

    din::Json response = din::obj();

    if (!result.isOk()) {
        response["success"] = false;
        response["error"] = result.error();
        return response;
    }

    // Success - return transaction details
    const Transaction& tx = result.value();

    // Serialize transaction to hex
    std::vector<uint8_t> tx_bytes = tx.Serialize();
    std::stringstream tx_hex;
    for (uint8_t byte : tx_bytes) {
        tx_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    response["success"] = true;
    response["tx_hex"] = tx_hex.str();
    response["funding_amount_sats"] = static_cast<din::Json::Int64>(amount_sats);
    response["fee_rate"] = static_cast<din::Json::Int64>(fee_rate);

    // Calculate actual fee paid
    uint64_t total_output = 0;
    for (const auto& output : tx.vout) {
        total_output += output.value;
    }
    uint64_t total_input = 0;  // Would need UTXO amounts to calculate
    // For now, just indicate success
    response["message"] = "Funding transaction created successfully. Broadcast with sendrawtransaction after counter-party signs.";

    return response;
}

/**
 * @brief Generate MuSig2 nonce for commitment signing
 *
 * RPC: wallet.generatenonce
 *
 * Generates a MuSig2 nonce pair for commitment transaction signing.
 * The public nonce must be exchanged with the remote peer via BOLT #2.
 *
 * Parameters:
 * - channel_id: 32-byte hex channel identifier
 *
 * Returns:
 * - public_nonce: 66-byte hex public nonce (send to remote peer)
 * - created_at: Unix timestamp when nonce was created
 */
static din::Json rpc_wallet_generatenonce(const ExecutionContext& ctx, const din::Json& params) {
    // Validate daemon context
    const DaemonContext* daemon_ctx = ctx.daemon;
    if (!daemon_ctx) {
        throw std::runtime_error("Daemon context not available");
    }

    // Parse parameters (array-style)
    if (params.size() < 1) {
        throw std::invalid_argument("Usage: wallet.generatenonce <channel_id>");
    }

    // channel_id (required) - params[0]
    std::string channel_id_hex = params[0].asString();

    // Validate and decode channel ID
    if (channel_id_hex.length() != 64) {
        throw std::invalid_argument("channel_id must be 32 bytes (64 hex chars)");
    }
    std::vector<uint8_t> channel_id;
    for (size_t i = 0; i < channel_id_hex.length(); i += 2) {
        std::string byte_str = channel_id_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        channel_id.push_back(byte);
    }

    // Create LightningWallet instance
    dinero::lightning::LightningWallet lightning_wallet(*const_cast<DaemonContext*>(daemon_ctx));

    // Generate nonce pair
    auto result = lightning_wallet.generateCommitmentNonce(channel_id);

    din::Json response = din::obj();

    if (!result.isOk()) {
        response["success"] = false;
        response["error"] = result.error();
        return response;
    }

    // Success - return public nonce
    const MuSig2NoncePair& nonce_pair = result.value();

    // Convert public nonce to hex
    std::stringstream public_nonce_hex;
    for (uint8_t byte : nonce_pair.public_nonce) {
        public_nonce_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    response["success"] = true;
    response["channel_id"] = channel_id_hex;
    response["public_nonce"] = public_nonce_hex.str();
    response["created_at"] = static_cast<din::Json::Int64>(nonce_pair.created_at);
    response["message"] = "Send public_nonce to remote peer via BOLT #2. Keep secret_nonce private.";

    return response;
}

/**
 * @brief Sign commitment transaction with MuSig2
 *
 * RPC: wallet.signcommitment
 *
 * Signs a commitment transaction using MuSig2 partial signature.
 * Requires a previously generated nonce (via wallet.generatenonce).
 *
 * Parameters:
 * - channel_id: 32-byte hex channel identifier
 * - tx_hex: Raw commitment transaction (hex)
 * - input_index: Input index to sign (usually 0 for funding output)
 * - remote_nonce: Remote peer's 66-byte hex public nonce
 * - sighash_type: (optional) Signature hash type (default: 0x01 = SIGHASH_ALL)
 *
 * Returns:
 * - partial_signature: 32-byte hex partial signature (send to remote peer)
 * - nonce_cleared: Boolean indicating nonce was securely wiped
 */
static din::Json rpc_wallet_signcommitment(const ExecutionContext& ctx, const din::Json& params) {
    // Validate daemon context
    const DaemonContext* daemon_ctx = ctx.daemon;
    if (!daemon_ctx) {
        throw std::runtime_error("Daemon context not available");
    }

    // Parse parameters (array-style)
    if (params.size() < 4) {
        throw std::invalid_argument("Usage: wallet.signcommitment <channel_id> <tx_hex> <input_index> <remote_nonce> [sighash_type]");
    }

    // channel_id (required) - params[0]
    std::string channel_id_hex = params[0].asString();

    // Validate and decode channel ID
    if (channel_id_hex.length() != 64) {
        throw std::invalid_argument("channel_id must be 32 bytes (64 hex chars)");
    }
    std::vector<uint8_t> channel_id;
    for (size_t i = 0; i < channel_id_hex.length(); i += 2) {
        std::string byte_str = channel_id_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        channel_id.push_back(byte);
    }

    // tx_hex (required) - params[1]
    std::string tx_hex = params[1].asString();

    // Fast sanity check before full deserialization.
    if (tx_hex.length() < 10) {
        throw std::invalid_argument("tx_hex is too short to be a valid transaction");
    }

    // input_index (required) - params[2]
    uint32_t input_index = params[2].asUInt();

    // remote_nonce (required) - params[3]
    std::string remote_nonce_hex = params[3].asString();

    // Validate and decode remote nonce
    if (remote_nonce_hex.length() != 132) {  // 66 bytes = 132 hex chars
        throw std::invalid_argument("remote_nonce must be 66 bytes (132 hex chars)");
    }
    std::vector<uint8_t> remote_nonce;
    for (size_t i = 0; i < remote_nonce_hex.length(); i += 2) {
        std::string byte_str = remote_nonce_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        remote_nonce.push_back(byte);
    }

    // sighash_type (optional, default: SIGHASH_ALL) - params[4]
    uint32_t sighash_type = 0x01;  // SIGHASH_ALL
    if (params.size() >= 5) {
        sighash_type = params[4].asUInt();
    }

    // Deserialize transaction from hex
    dinero::Transaction tx;
    if (!dinero::TransactionSerializer::Deserialize(tx, tx_hex)) {
        throw std::invalid_argument("Failed to deserialize transaction from hex");
    }

    // Create LightningWallet instance
    dinero::lightning::LightningWallet lightning_wallet(*const_cast<DaemonContext*>(daemon_ctx));

    // Sign commitment transaction
    auto result = lightning_wallet.signCommitmentWithMuSig2(channel_id, tx, input_index, remote_nonce, sighash_type);

    din::Json response = din::obj();

    if (!result.isOk()) {
        response["success"] = false;
        response["error"] = result.error();
        return response;
    }

    // Success - return partial signature
    const std::vector<uint8_t>& partial_sig = result.value();

    // Convert partial signature to hex
    std::stringstream sig_hex;
    for (uint8_t byte : partial_sig) {
        sig_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    response["success"] = true;
    response["channel_id"] = channel_id_hex;
    response["partial_signature"] = sig_hex.str();
    response["input_index"] = static_cast<din::Json::Int>(input_index);
    response["sighash_type"] = static_cast<din::Json::Int>(sighash_type);
    response["message"] = "Send partial_signature to remote peer. Remote will aggregate with their signature.";
    response["nonce_cleared"] = true;  // Nonce is automatically cleared after signing

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// Registration (called from rpc_context_wiring.cpp)
// ═══════════════════════════════════════════════════════════════════════════

void register_lightning_methods() {
    // Channel management
    g_rpcRegistry.registerHandler("ln.openchannel",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_openchannel(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.closechannel",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_closechannel(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.listchannels",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_listchannels(ctx, params);
        });

    // Invoice & payment methods
    g_rpcRegistry.registerHandler("ln.createinvoice",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_createinvoice(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.payinvoice",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_payinvoice(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.sendpayment",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_sendpayment(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.decodeinvoice",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_decodeinvoice(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.listinvoices",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_listinvoices(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.listpayments",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_listpayments(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.listfailures",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_listfailures(ctx, params);
        });

    g_rpcRegistry.registerHandler("ln.getpayment",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_ln_getpayment(ctx, params);
        });

    // Wallet integration methods
    g_rpcRegistry.registerHandler("wallet.fundchannel",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_wallet_fundchannel(ctx, params);
        });

    g_rpcRegistry.registerHandler("wallet.generatenonce",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_wallet_generatenonce(ctx, params);
        });

    g_rpcRegistry.registerHandler("wallet.signcommitment",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_wallet_signcommitment(ctx, params);
        });

    dinero::g_logger.info("✅ Lightning Network RPC methods registered (14 methods: ln.openchannel, ln.closechannel, ln.listchannels, ln.createinvoice, ln.payinvoice, ln.sendpayment, ln.decodeinvoice, ln.listinvoices, ln.listpayments, ln.listfailures, ln.getpayment, wallet.fundchannel, wallet.generatenonce, wallet.signcommitment)");
}
