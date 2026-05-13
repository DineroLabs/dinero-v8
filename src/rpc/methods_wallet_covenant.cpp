/**
 * Wallet Covenant RPC Methods - Phase C.4: Covenant Construction API
 *
 * This file implements RPC methods for covenant transaction construction.
 * These methods expose the covenant construction helpers from Phase C.3.
 *
 * Architecture:
 *   RPC Layer → Wallet Construction Helpers → (Consensus Validation - separate)
 *
 * CRITICAL BOUNDARY RULE:
 * - RPC methods expose CONSTRUCTION helpers only
 * - RPC methods NEVER validate covenant rules
 * - Validation happens in consensus::ScriptInterpreter
 *
 * New RPC Methods:
 *   - wallet.createctvtemplate     - Build CTV template from outputs
 *   - wallet.createctvscript        - Create CTV-locked scriptPubKey
 *   - wallet.buildctvspending       - Build CTV spending transaction
 *   - wallet.createcsfsdelegation   - Create unsigned CSFS delegation
 *   - wallet.signcsfs               - Sign CSFS delegation
 *   - wallet.createcsfsscript       - Create CSFS-locked scriptPubKey
 *   - wallet.estimatecovenantfee    - Estimate covenant witness fees
 *
 * Phase: C.4 - RPC Layer
 * Date: 2025-12-27
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "wallet/covenant_builders.h"
#include "wallet/canonical_wallet_utxo.h"
#include "primitives/uint256.h"
#include "external/bech32/bech32.hpp"
#include "common/logger.h"
#include "common/ilogger.h"
#include <memory>
#include <iomanip>
#include <sstream>
#include <vector>

// ═══════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════

/**
 * Convert hex string to byte vector
 */
std::vector<uint8_t> hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Hex string must have even length");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
        bytes.push_back(byte);
    }

    return bytes;
}

/**
 * Convert byte vector to hex string
 */
std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

/**
 * Convert array to hex string
 */
template<size_t N>
std::string arrayToHex(const std::array<uint8_t, N>& arr) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : arr) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

/**
 * Decode Bech32 address to scriptPubKey
 */
std::vector<uint8_t> decodeAddress(const std::string& address) {
    auto decoded = bech32::decode(address);
    if (decoded.hrp.empty()) {
        throw std::runtime_error("Invalid Bech32 address");
    }

    // Convert 5-bit to 8-bit
    std::vector<uint8_t> data;
    if (!bech32::convertbits(data, decoded.dp, 5, 8, false)) {
        throw std::runtime_error("Failed to convert address data");
    }

    // Construct scriptPubKey: OP_0 + data
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x00);  // OP_0 (witness v0)
    scriptPubKey.push_back(static_cast<uint8_t>(data.size()));
    scriptPubKey.insert(scriptPubKey.end(), data.begin(), data.end());

    return scriptPubKey;
}

// ═══════════════════════════════════════════════════════════════
// CTV RPC METHODS
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.createctvtemplate - Build CTV template from outputs
 *
 * Phase C.4: RPC exposure of buildCTVTemplate()
 *
 * @param params {
 *   "outputs": [{"address": "tb1q...", "value": 50000}, ...],
 *   "locktime": 0,
 *   "version": 2
 * }
 *
 * @returns {
 *   "template_hash": "abcd1234...",
 *   "outputs": [...],
 *   "locktime": 0,
 *   "version": 2,
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_createctvtemplate(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("outputs") || !params["outputs"].is_array()) {
            result["error"] = "outputs must be an array";
            return result;
        }

        // Parse outputs
        std::vector<dinero::wallet::CTVOutput> outputs;
        for (const auto& output_json : params["outputs"]) {
            if (!output_json.contains("address") || !output_json.contains("value")) {
                result["error"] = "Each output must have 'address' and 'value'";
                return result;
            }

            dinero::wallet::CTVOutput output;
            output.address = output_json["address"].get<std::string>();
            output.value = output_json["value"].get<uint64_t>();

            // Decode address to scriptPubKey
            try {
                output.scriptPubKey = decodeAddress(output.address);
            } catch (const std::exception& e) {
                result["error"] = std::string("Invalid address: ") + e.what();
                return result;
            }

            outputs.push_back(output);
        }

        // Parse optional parameters
        uint32_t locktime = params.value("locktime", 0);
        int32_t version = params.value("version", 2);

        // Build CTV template using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto ctv_template = dinero::wallet::buildCTVTemplate(outputs, locktime, version);

        // Prepare response
        result["template_hash"] = arrayToHex(ctv_template.template_hash);
        result["locktime"] = ctv_template.locktime;
        result["version"] = ctv_template.version;

        din::Json outputs_json = din::Json::array();
        for (const auto& output : ctv_template.outputs) {
            din::Json out;
            out["value"] = output.value;
            out["script_pubkey"] = bytesToHex(output.scriptPubKey);
            out["address"] = output.address;
            outputs_json.push_back(out);
        }
        result["outputs"] = outputs_json;

        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.createctvtemplate: " +
                std::to_string(outputs.size()) + " outputs");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create CTV template: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.createctvtemplate error: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.createctvscript - Create CTV-locked scriptPubKey
 *
 * Phase C.4: RPC exposure of createCTVScript()
 *
 * @param params {
 *   "template_hash": "abcd1234...",
 *   "use_taproot": false
 * }
 *
 * @returns {
 *   "script": "0020abcd...",
 *   "script_hex": "0020abcd1234...",
 *   "type": "p2wsh_ctv",
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_createctvscript(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("template_hash")) {
            result["error"] = "template_hash required";
            return result;
        }

        std::string template_hash_hex = params["template_hash"].get<std::string>();
        if (template_hash_hex.size() != 64) {
            result["error"] = "template_hash must be 64 hex characters (32 bytes)";
            return result;
        }

        // Parse template hash
        auto hash_bytes = hexToBytes(template_hash_hex);
        std::array<uint8_t, 32> template_hash;
        std::copy(hash_bytes.begin(), hash_bytes.end(), template_hash.begin());

        // Parse optional parameters
        bool use_taproot = params.value("use_taproot", false);

        // Create CTV script using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto script = dinero::wallet::createCTVScript(template_hash, use_taproot);

        // Prepare response
        result["script_hex"] = bytesToHex(script);
        result["type"] = use_taproot ? "taproot_ctv" : "p2wsh_ctv";
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.createctvscript: " +
                std::string(use_taproot ? "taproot" : "p2wsh"));
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create CTV script: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.createctvscript error: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.buildctvspending - Build CTV spending transaction
 *
 * Phase C.4: RPC exposure of buildCTVSpendingTx()
 *
 * @param params {
 *   "template_hash": "abcd1234...",
 *   "funding_utxo": {"txid": "1111...", "vout": 0, "value": 100000, "height": 100},
 *   "outputs": [...]
 * }
 *
 * @returns {
 *   "tx_hex": "0200000001...",
 *   "txid": "abcd...",
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_buildctvspending(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("template_hash") || !params.contains("funding_utxo") ||
            !params.contains("outputs")) {
            result["error"] = "template_hash, funding_utxo, and outputs required";
            return result;
        }

        // Parse outputs (same as createctvtemplate)
        std::vector<dinero::wallet::CTVOutput> outputs;
        for (const auto& output_json : params["outputs"]) {
            dinero::wallet::CTVOutput output;
            output.address = output_json["address"].get<std::string>();
            output.value = output_json["value"].get<uint64_t>();
            output.scriptPubKey = decodeAddress(output.address);
            outputs.push_back(output);
        }

        // Build CTV template
        uint32_t locktime = params.value("locktime", 0);
        int32_t version = params.value("version", 2);
        auto ctv_template = dinero::wallet::buildCTVTemplate(outputs, locktime, version);

        // Parse funding UTXO
        const auto& utxo_json = params["funding_utxo"];
        dinero::wallet::CanonicalWalletUTXO funding_utxo;
        funding_utxo.txid = dinero::uint256::FromHex(utxo_json["txid"].get<std::string>());
        funding_utxo.vout = utxo_json["vout"].get<uint32_t>();
        funding_utxo.value = utxo_json["value"].get<uint64_t>();
        funding_utxo.height = utxo_json["height"].get<uint32_t>();
        funding_utxo.is_coinbase = utxo_json.value("is_coinbase", false);

        // Build spending transaction using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto spending_tx = dinero::wallet::buildCTVSpendingTx(
            ctv_template,
            funding_utxo,
            0  // input_index
        );

        // Serialize transaction (simplified - would need proper serialization)
        result["txid"] = spending_tx.getTxId().GetHex();
        result["version"] = spending_tx.version;
        result["locktime"] = spending_tx.lockTime;
        result["vin_count"] = static_cast<int>(spending_tx.vin.size());
        result["vout_count"] = static_cast<int>(spending_tx.vout.size());
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.buildctvspending: " +
                spending_tx.getTxId().GetHex());
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to build CTV spending tx: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.buildctvspending error: " + std::string(e.what()));
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// CSFS RPC METHODS
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.createcsfsdelegation - Create unsigned CSFS delegation
 *
 * Phase C.4: RPC exposure of createCSFSDelegation()
 *
 * @param params {
 *   "pubkey": "abcd1234...",
 *   "message": "delegation message",
 *   "purpose": "oracle"
 * }
 *
 * @returns {
 *   "pubkey": "abcd1234...",
 *   "message": "delegation message",
 *   "message_hex": "64656c65...",
 *   "purpose": "oracle",
 *   "is_signed": false,
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_createcsfsdelegation(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("pubkey") || !params.contains("message")) {
            result["error"] = "pubkey and message required";
            return result;
        }

        // Parse pubkey
        std::string pubkey_hex = params["pubkey"].get<std::string>();
        if (pubkey_hex.size() != 64) {
            result["error"] = "pubkey must be 64 hex characters (32 bytes x-only)";
            return result;
        }
        auto pubkey = hexToBytes(pubkey_hex);

        // Parse message
        std::string message_str = params["message"].get<std::string>();
        std::vector<uint8_t> message(message_str.begin(), message_str.end());

        // Parse optional purpose
        std::string purpose = params.value("purpose", "delegation");

        // Create CSFS delegation using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto delegation = dinero::wallet::createCSFSDelegation(pubkey, message, purpose);

        // Prepare response
        result["pubkey"] = bytesToHex(delegation.pubkey);
        result["message"] = std::string(delegation.message.begin(), delegation.message.end());
        result["message_hex"] = bytesToHex(delegation.message);
        result["purpose"] = delegation.purpose;
        result["is_signed"] = delegation.is_signed;
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.createcsfsdelegation: " + purpose);
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create CSFS delegation: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.createcsfsdelegation error: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.signcsfs - Sign CSFS delegation
 *
 * Phase C.4: RPC exposure of signCSFSDelegation()
 *
 * @param params {
 *   "delegation": {...},
 *   "privkey": "secret1234..."
 * }
 *
 * @returns {
 *   "pubkey": "abcd1234...",
 *   "message": "delegation message",
 *   "signature": "abcd1234...",
 *   "is_signed": true,
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_signcsfs(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("delegation") || !params.contains("privkey")) {
            result["error"] = "delegation and privkey required";
            return result;
        }

        // Parse delegation
        const auto& delegation_json = params["delegation"];
        dinero::wallet::CSFSDelegationBuilder delegation;

        delegation.pubkey = hexToBytes(delegation_json["pubkey"].get<std::string>());
        std::string message_str = delegation_json["message"].get<std::string>();
        delegation.message = std::vector<uint8_t>(message_str.begin(), message_str.end());
        delegation.purpose = delegation_json.value("purpose", "delegation");
        delegation.is_signed = false;

        // Parse private key
        std::string privkey_hex = params["privkey"].get<std::string>();
        if (privkey_hex.size() != 64) {
            result["error"] = "privkey must be 64 hex characters (32 bytes)";
            return result;
        }
        auto privkey = hexToBytes(privkey_hex);

        // Sign delegation using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto signed_delegation = dinero::wallet::signCSFSDelegation(delegation, privkey);

        // Prepare response
        result["pubkey"] = bytesToHex(signed_delegation.pubkey);
        result["message"] = std::string(signed_delegation.message.begin(),
                                       signed_delegation.message.end());
        result["signature"] = bytesToHex(signed_delegation.signature);
        result["is_signed"] = signed_delegation.is_signed;
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.signcsfs: delegation signed");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to sign CSFS delegation: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.signcsfs error: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.createcsfsscript - Create CSFS-locked scriptPubKey
 *
 * Phase C.4: RPC exposure of createCSFSScript()
 *
 * @param params {
 *   "pubkey": "abcd1234...",
 *   "message": "delegation message",
 *   "continuation_script": ""
 * }
 *
 * @returns {
 *   "script_hex": "20abcd1234...",
 *   "type": "tapscript_csfs",
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_createcsfsscript(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("pubkey") || !params.contains("message")) {
            result["error"] = "pubkey and message required";
            return result;
        }

        // Parse pubkey
        auto pubkey = hexToBytes(params["pubkey"].get<std::string>());

        // Parse message
        std::string message_str = params["message"].get<std::string>();
        std::vector<uint8_t> message(message_str.begin(), message_str.end());

        // Parse optional continuation script
        std::vector<uint8_t> continuation_script;
        if (params.contains("continuation_script")) {
            continuation_script = hexToBytes(params["continuation_script"].get<std::string>());
        }

        // Create CSFS script using wallet helper
        // Phase C.4: RPC ONLY exposes construction - NO validation
        auto script = dinero::wallet::createCSFSScript(pubkey, message, continuation_script);

        // Prepare response
        result["script_hex"] = bytesToHex(script);
        result["type"] = "tapscript_csfs";
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.createcsfsscript: script created");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create CSFS script: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.createcsfsscript error: " + std::string(e.what()));
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// UTILITY RPC METHODS
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.estimatecovenantfee - Estimate covenant witness fees
 *
 * Phase C.4: RPC exposure of estimateCovenantWitnessSize()
 *
 * @param params {
 *   "covenant_type": "ctv",
 *   "template_size": 32
 * }
 *
 * @returns {
 *   "covenant_type": "ctv",
 *   "witness_size": 36,
 *   "witness_vbytes": 36,
 *   "success": true
 * }
 */
din::Json rpc_context_wallet_estimatecovenantfee(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    try {
        // Validate parameters
        if (!params.contains("covenant_type")) {
            result["error"] = "covenant_type required";
            return result;
        }

        std::string type_str = params["covenant_type"].get<std::string>();
        dinero::wallet::CovenantType covenant_type;

        if (type_str == "ctv") {
            covenant_type = dinero::wallet::CovenantType::CTV;
        } else if (type_str == "csfs") {
            covenant_type = dinero::wallet::CovenantType::CSFS;
        } else if (type_str == "txhash") {
            covenant_type = dinero::wallet::CovenantType::TXHASH;
        } else if (type_str == "ccv") {
            covenant_type = dinero::wallet::CovenantType::CCV;
        } else {
            result["error"] = "Invalid covenant_type (must be: ctv, csfs, txhash, ccv)";
            return result;
        }

        // Parse optional template size
        size_t template_size = params.value("template_size", 32);

        // Estimate witness size using wallet helper
        size_t witness_size = dinero::wallet::estimateCovenantWitnessSize(
            covenant_type,
            template_size
        );

        // Prepare response
        result["covenant_type"] = type_str;
        result["witness_size"] = static_cast<int>(witness_size);
        result["witness_vbytes"] = static_cast<int>(witness_size);  // Same for witness
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.v1";

        if (ctx.logger) {
            ctx.logger->info("[RPC] wallet.estimatecovenantfee: " +
                type_str + " = " + std::to_string(witness_size) + " vbytes");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to estimate covenant fee: ") + e.what();
        result["success"] = false;
        if (ctx.logger) {
            ctx.logger->error("[RPC] wallet.estimatecovenantfee error: " + std::string(e.what()));
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// RPC REGISTRATION
// ═══════════════════════════════════════════════════════════════

/**
 * Register all covenant RPC methods
 *
 * Phase C.4: Registration function for covenant RPC methods
 */
void register_context_wallet_covenant_methods() {
    // Phase C.4: CTV RPC methods
    g_rpcRegistry.registerHandler(
        "wallet.createctvtemplate",
        rpc_context_wallet_createctvtemplate,
        RegisterMode::Overwrite,
        "context-aware"
    );

    g_rpcRegistry.registerHandler(
        "wallet.createctvscript",
        rpc_context_wallet_createctvscript,
        RegisterMode::Overwrite,
        "context-aware"
    );

    g_rpcRegistry.registerHandler(
        "wallet.buildctvspending",
        rpc_context_wallet_buildctvspending,
        RegisterMode::Overwrite,
        "context-aware"
    );

    // Phase C.4: CSFS RPC methods
    g_rpcRegistry.registerHandler(
        "wallet.createcsfsdelegation",
        rpc_context_wallet_createcsfsdelegation,
        RegisterMode::Overwrite,
        "context-aware"
    );

    g_rpcRegistry.registerHandler(
        "wallet.signcsfs",
        rpc_context_wallet_signcsfs,
        RegisterMode::Overwrite,
        "context-aware"
    );

    g_rpcRegistry.registerHandler(
        "wallet.createcsfsscript",
        rpc_context_wallet_createcsfsscript,
        RegisterMode::Overwrite,
        "context-aware"
    );

    // Phase C.4: Utility RPC methods
    g_rpcRegistry.registerHandler(
        "wallet.estimatecovenantfee",
        rpc_context_wallet_estimatecovenantfee,
        RegisterMode::Overwrite,
        "context-aware"
    );
}
