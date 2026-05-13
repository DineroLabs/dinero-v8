/**
 * Phase 29: Covenant RPC Methods
 *
 * JSON-RPC methods for covenant creation, inspection, and spending:
 * - covenant.ctvcreate - Create CTV template
 * - covenant.ctvdecode - Decode CTV hash to template
 * - covenant.ctvspend - Create spending transaction
 * - covenant.analyze - Analyze script for covenants
 * - covenant.csfs.create - Create CSFS delegation
 * - covenant.csfs.sign - Sign delegation
 * - covenant.contract.create - Register contract
 * - covenant.contract.advance - Advance contract state
 * - covenant.list - List covenant UTXOs
 * - covenant.stats - Get covenant statistics
 */

#pragma once

#include "compat/jsoncpp_compat.h"
#include <functional>
#include <map>
#include <string>

namespace dinero {
namespace rpc {

// Forward declaration
struct DaemonContext;

/**
 * @brief Register all covenant RPC methods
 */
void registerCovenantMethods(
    std::map<std::string, std::function<Json::Value(const Json::Value&, DaemonContext&)>>& methods);

// ============================================================================
// CTV (CheckTemplateVerify) Methods
// ============================================================================

/**
 * @brief covenant.ctvcreate - Create a CTV template
 *
 * Creates a new CTV template with specified outputs.
 *
 * Parameters:
 * {
 *   "outputs": [
 *     {"address": "din1...", "amount": 1.0},
 *     {"script": "0014...", "amount": 0.5}
 *   ],
 *   "locktime": 0,
 *   "label": "My vault template"
 * }
 *
 * Returns:
 * {
 *   "template_id": "uuid",
 *   "template_hash": "hex",
 *   "script": "hex (P2WSH script with CTV)",
 *   "address": "din1... (bech32m address)"
 * }
 */
Json::Value covenant_ctvcreate(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.ctvdecode - Decode CTV hash
 *
 * Decodes a CTV hash to show committed template structure.
 *
 * Parameters:
 * {
 *   "hash": "32-byte hex hash"
 * }
 * OR
 * {
 *   "template_id": "uuid"
 * }
 *
 * Returns:
 * {
 *   "found": true/false,
 *   "template": { ... template details ... }
 * }
 */
Json::Value covenant_ctvdecode(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.ctvtemplate - Get template details
 *
 * Parameters:
 * {
 *   "template_id": "uuid"
 * }
 *
 * Returns: Full template information
 */
Json::Value covenant_ctvtemplate(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.ctvspend - Create CTV spending transaction
 *
 * Creates a transaction that spends a CTV-locked output.
 *
 * Parameters:
 * {
 *   "template_id": "uuid",
 *   "utxo": {"txid": "...", "vout": 0}
 * }
 *
 * Returns:
 * {
 *   "hex": "raw transaction hex",
 *   "txid": "transaction id",
 *   "complete": true/false
 * }
 */
Json::Value covenant_ctvspend(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.ctvlist - List CTV templates
 *
 * Parameters:
 * {
 *   "include_spent": false
 * }
 *
 * Returns: Array of templates
 */
Json::Value covenant_ctvlist(const Json::Value& params, DaemonContext& ctx);

// ============================================================================
// CSFS (CheckSigFromStack) Methods
// ============================================================================

/**
 * @brief covenant.csfs.create - Create CSFS delegation
 *
 * Creates a delegation request for off-chain signing.
 *
 * Parameters:
 * {
 *   "pubkey": "32-byte x-only pubkey hex",
 *   "message": "message to sign (hex or string)",
 *   "purpose": "oracle|delegation|channel",
 *   "expires_at": 0 (unix timestamp, 0 = no expiry)
 * }
 *
 * Returns:
 * {
 *   "delegation_id": "uuid",
 *   "pubkey": "hex",
 *   "message_hash": "hex"
 * }
 */
Json::Value covenant_csfs_create(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.csfs.sign - Add signature to delegation
 *
 * Parameters:
 * {
 *   "delegation_id": "uuid",
 *   "signature": "64-byte schnorr signature hex"
 * }
 *
 * Returns:
 * {
 *   "success": true/false,
 *   "verified": true/false
 * }
 */
Json::Value covenant_csfs_sign(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.csfs.verify - Verify a CSFS signature
 *
 * Parameters:
 * {
 *   "delegation_id": "uuid"
 * }
 * OR
 * {
 *   "pubkey": "hex",
 *   "message": "hex",
 *   "signature": "hex"
 * }
 *
 * Returns:
 * {
 *   "valid": true/false
 * }
 */
Json::Value covenant_csfs_verify(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.csfs.list - List CSFS delegations
 */
Json::Value covenant_csfs_list(const Json::Value& params, DaemonContext& ctx);

// ============================================================================
// Contract (CCV) Methods
// ============================================================================

/**
 * @brief covenant.contract.create - Register new contract
 *
 * Parameters:
 * {
 *   "code": "contract code hex",
 *   "initial_data": "initial state data hex",
 *   "type": "escrow|vault|auction|custom",
 *   "label": "My contract"
 * }
 *
 * Returns:
 * {
 *   "contract_id": "uuid",
 *   "code_hash": "hex",
 *   "state_hash": "hex"
 * }
 */
Json::Value covenant_contract_create(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.contract.get - Get contract details
 *
 * Parameters:
 * {
 *   "contract_id": "uuid"
 * }
 *
 * Returns: Contract state and history
 */
Json::Value covenant_contract_get(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.contract.advance - Create state transition
 *
 * Parameters:
 * {
 *   "contract_id": "uuid",
 *   "new_data": "new state data hex",
 *   "utxo": {"txid": "...", "vout": 0}
 * }
 *
 * Returns:
 * {
 *   "hex": "raw transaction hex",
 *   "new_state_hash": "hex",
 *   "new_counter": number
 * }
 */
Json::Value covenant_contract_advance(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.contract.list - List contracts
 */
Json::Value covenant_contract_list(const Json::Value& params, DaemonContext& ctx);

// ============================================================================
// General Covenant Methods
// ============================================================================

/**
 * @brief covenant.analyze - Analyze script for covenant constraints
 *
 * Parameters:
 * {
 *   "script": "scriptPubKey hex"
 * }
 * OR
 * {
 *   "address": "din1..."
 * }
 *
 * Returns:
 * {
 *   "type": "ctv|csfs|txhash|ccv|composite|none",
 *   "has_ctv": true/false,
 *   "has_csfs": true/false,
 *   "has_txhash": true/false,
 *   "has_ccv": true/false,
 *   "ctv_hash": "hex (if found)",
 *   "description": "Human-readable description"
 * }
 */
Json::Value covenant_analyze(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.list - List covenant UTXOs
 *
 * Parameters:
 * {
 *   "type": "ctv|csfs|ccv|all",
 *   "include_spent": false
 * }
 *
 * Returns: Array of covenant UTXOs
 */
Json::Value covenant_list(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.spendinfo - Get spending requirements for covenant UTXO
 *
 * Parameters:
 * {
 *   "txid": "...",
 *   "vout": 0
 * }
 *
 * Returns:
 * {
 *   "type": "ctv|csfs|ccv",
 *   "can_spend": true/false,
 *   "missing": "what's needed to spend",
 *   "requirements": { ... }
 * }
 */
Json::Value covenant_spendinfo(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.estimatefee - Estimate fee for covenant spend
 *
 * Parameters:
 * {
 *   "txid": "...",
 *   "vout": 0,
 *   "fee_rate": 1 (sat/vB)
 * }
 *
 * Returns:
 * {
 *   "total_fee": una,
 *   "vsize": bytes,
 *   "weight": weight units
 * }
 */
Json::Value covenant_estimatefee(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.stats - Get covenant statistics
 *
 * Returns:
 * {
 *   "ctv_templates": { "total": N, "active": N },
 *   "delegations": { "total": N, "signed": N },
 *   "contracts": { "total": N, "active": N },
 *   "utxos": { "count": N, "value_locked": una }
 * }
 */
Json::Value covenant_stats(const Json::Value& params, DaemonContext& ctx);

/**
 * @brief covenant.buildtx - Build covenant transaction
 *
 * Generic transaction builder for covenant operations.
 *
 * Parameters:
 * {
 *   "inputs": [
 *     {"txid": "...", "vout": 0, "covenant_type": "ctv", "template_id": "..."}
 *   ],
 *   "outputs": [
 *     {"address": "din1...", "amount": 1.0}
 *   ],
 *   "locktime": 0
 * }
 *
 * Returns:
 * {
 *   "hex": "raw transaction",
 *   "complete": true/false,
 *   "errors": []
 * }
 */
Json::Value covenant_buildtx(const Json::Value& params, DaemonContext& ctx);

} // namespace rpc
} // namespace dinero
