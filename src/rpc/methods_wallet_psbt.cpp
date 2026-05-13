#include "daemon/execution_context.h"
#include "wallet/tx_builder_iface.h"
#include "wallet/psbt.h"
#include "wallet/psbt_signer.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_metrics.h"
#include "compat/jsoncpp_compat.h"
#include <sqlite3.h>

namespace din {
namespace rpc {

namespace {

std::string DetectWalletPolicyFromContext(const ExecutionContext& ctx) {
    if (!ctx.wallet_manager) {
        return "bip84";
    }

    sqlite3* db = ctx.wallet_manager->getCurrentDatabase();
    if (!db) {
        return "bip84";
    }

    std::string wallet_policy = "bip84";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT wallet_policy FROM wallet_meta WHERE id = 1", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* policy_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (policy_cstr && *policy_cstr) {
                wallet_policy = policy_cstr;
            }
        }
    }
    if (stmt) {
        sqlite3_finalize(stmt);
    }

    return wallet_policy == "bip86" ? "bip86" : "bip84";
}

} // namespace

Json::Value walletcreatefundedpsbt_handler(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value reply; 
    reply["rpc_schema"] = "din.rpc.v1";

    if (!ctx.tx_builder) {
        // JSON-RPC -32603 Internal error
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -32603;
        reply["error"]["message"] = "tx builder unavailable";
        return reply;
    }

    // Parse params -> inputs/outputs/feerate/RBF (validate shapes)
    if (!params.isMember("inputs") || !params["inputs"].isArray() ||
        !params.isMember("outputs") || !params["outputs"].isArray()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "inputs and outputs arrays required";
        return reply;
    }

    TxBuildRequest req;
    
    // Parse inputs
    for (const auto& in : params["inputs"]) {
        if (!in.isMember("txid") || !in.isMember("vout")) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -8;
            reply["error"]["message"] = "input must have txid and vout";
            return reply;
        }
        
        TxIn ti;
        ti.txid = in["txid"].asString();
        ti.vout = in["vout"].asUInt();
        if (in.isMember("redeemScript")) {
            ti.redeem_script = in["redeemScript"].asString();
        }
        req.inputs.push_back(std::move(ti));
    }
    
    // Parse outputs
    for (const auto& out : params["outputs"]) {
        if (!out.isMember("address") || !out.isMember("value")) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -8;
            reply["error"]["message"] = "output must have address and value";
            return reply;
        }
        
        std::string addr = out["address"].asString();
        int64_t val = out["value"].asInt64(); // una
        req.outputs.push_back({addr, val});
    }
    
    // Parse optional parameters
    req.fee_rate_una_vb = params.get("fee_rate_una_vb", 20).asInt64(); // Default 20 una/vB
    req.replace_by_fee = params.get("rbf", false).asBool();

    // Validate request
    if (req.outputs.empty()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "at least one output required";
        return reply;
    }

    // Create transaction via clean interface
    TxBuildErr err{};
    if (auto res = ctx.tx_builder->create(req, err)) {
        Json::Value ok(Json::objectValue);
        ok["psbt"] = res->psbt_base64;
        ok["fee"] = Json::Int64(res->fee_paid);
        ok["changepos"] = res->change_output_value > 0 ? 1 : -1; // Change output position
        ok["changeposvalue"] = Json::Int64(res->change_output_value);
        reply["result"] = ok;
        return reply;
    }

    // Map errors to JSON-RPC application codes
    Json::Value e(Json::objectValue);
    switch (err) {
        case TxBuildErr::InsufficientFunds: 
            e["code"] = -6; 
            e["message"] = "insufficient funds"; 
            break;
        case TxBuildErr::InvalidAddress:    
            e["code"] = -5; 
            e["message"] = "invalid address";    
            break;
        case TxBuildErr::DustOutput:        
            e["code"] = -4; 
            e["message"] = "dust output";        
            break;
        default:                            
            e["code"] = -1; 
            e["message"] = "builder internal error"; 
            break;
    }
    reply["error"] = e;
    return reply;
}

Json::Value walletprocesspsbt_handler(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value reply;
    reply["rpc_schema"] = "din.rpc.v1";
    
    // Validate parameters
    if (!params.isMember("psbt") || !params["psbt"].isString()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required";
        return reply;
    }
    
    std::string psbt_b64 = params["psbt"].asString();
    bool sign = params.get("sign", true).asBool();
    std::string sighash_type_str = params.get("sighashtype", "ALL").asString();
    
    try {
        // Decode PSBT
        auto psbt_bytes = from_base64(psbt_b64);
        Psbt psbt;
        if (!deserialize(psbt_bytes, psbt)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "invalid PSBT format";
            return reply;
        }
        
        // Validate PSBT
        std::string validation_error;
        if (!validate_psbt(psbt, validation_error)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "PSBT validation failed: " + validation_error;
            return reply;
        }
        
        size_t signed_count = 0;
        Json::Value warnings = Json::Value(Json::arrayValue);
        Json::Value errors = Json::Value(Json::arrayValue);

        if (sign) {
            // Use real keystore from ExecutionContext
            if (!ctx.key_store) {
                reply["error"] = Json::Value(Json::objectValue);
                reply["error"]["code"] = -4;
                reply["error"]["message"] = "No keystore available (wallet may not be loaded)";
                return reply;
            }

            // Create signer with real keystore and sign
            auto keystore_ptr = std::shared_ptr<IKeyStore>(ctx.key_store, [](IKeyStore*){}); // Non-owning shared_ptr
            PsbtSigner signer(keystore_ptr, DetectWalletPolicyFromContext(ctx));
            PsbtSignResult sign_result = signer.signPsbt(psbt);

            // Check for fatal errors
            if (!sign_result.success) {
                reply["error"] = Json::Value(Json::objectValue);
                reply["error"]["code"] = -4;
                reply["error"]["message"] = sign_result.error;

                // Add detailed input errors if available
                if (!sign_result.input_errors.empty()) {
                    Json::Value input_errors = Json::Value(Json::arrayValue);
                    for (const auto& err : sign_result.input_errors) {
                        Json::Value err_obj(Json::objectValue);
                        err_obj["input"] = Json::UInt64(err.input_index);
                        err_obj["error"] = err.error;
                        err_obj["severity"] = err.severity;
                        input_errors.append(err_obj);
                    }
                    reply["error"]["input_errors"] = input_errors;
                }

                return reply;
            }

            signed_count = sign_result.signed_count;

            // Collect warnings and errors for successful signing
            for (const auto& err : sign_result.input_errors) {
                Json::Value err_obj(Json::objectValue);
                err_obj["input"] = Json::UInt64(err.input_index);
                err_obj["message"] = err.error;

                if (err.severity == "warning") {
                    warnings.append(err_obj);
                } else {
                    errors.append(err_obj);
                }
            }

            // Update metrics
            WalletMetrics::incrementPsbtSignedInputs(signed_count);
        }

        // Serialize updated PSBT
        auto updated_psbt_bytes = serialize(psbt);
        std::string updated_psbt_b64 = to_base64(updated_psbt_bytes);

        // Check if complete
        bool complete = is_psbt_complete(psbt);

        // Build response
        Json::Value result(Json::objectValue);
        result["psbt"] = updated_psbt_b64;
        result["complete"] = complete;
        result["signed_inputs"] = Json::UInt64(signed_count);

        // Add warnings/errors if any
        if (!warnings.empty()) {
            result["warnings"] = warnings;
        }
        if (!errors.empty()) {
            result["errors"] = errors;
        }

        // Add helpful message if incomplete
        if (!complete && signed_count < psbt.inputs.size()) {
            result["incomplete_reason"] = "PSBT incomplete: " +
                std::to_string(psbt.inputs.size() - signed_count) +
                " of " + std::to_string(psbt.inputs.size()) +
                " inputs missing signatures";
        }

        reply["result"] = result;
        return reply;
        
    } catch (const std::exception& e) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("PSBT processing failed: ") + e.what();
        return reply;
    }
}

Json::Value analyzepsbt_handler(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value reply;
    reply["rpc_schema"] = "din.rpc.v1";
    
    // Validate parameters
    if (!params.isMember("psbt") || !params["psbt"].isString()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required";
        return reply;
    }
    
    std::string psbt_b64 = params["psbt"].asString();
    
    try {
        // Decode PSBT
        auto psbt_bytes = from_base64(psbt_b64);
        Psbt psbt;
        if (!deserialize(psbt_bytes, psbt)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "invalid PSBT format";
            return reply;
        }
        
        // Analyze PSBT
        auto analysis = analyze_psbt(psbt);
        
        // Build response
        Json::Value result(Json::objectValue);
        result["errors"] = Json::Value(Json::arrayValue);
        result["warnings"] = Json::Value(Json::arrayValue);
        result["next_steps"] = Json::Value(Json::arrayValue);
        result["is_final"] = analysis.is_final;
        result["is_complete"] = analysis.is_complete;
        result["estimated_vsize"] = Json::UInt64(analysis.estimated_vsize);
        result["estimated_fee"] = Json::UInt64(analysis.estimated_fee);
        result["missing"] = Json::Value(Json::arrayValue);
        result["unknown"] = Json::Value(Json::arrayValue);
        
        // Convert vectors to JSON arrays
        for (const auto& error : analysis.errors) {
            result["errors"].append(error);
        }
        for (const auto& warning : analysis.warnings) {
            result["warnings"].append(warning);
        }
        for (const auto& step : analysis.next_steps) {
            result["next_steps"].append(step);
        }
        for (const auto& missing : analysis.missing) {
            result["missing"].append(missing);
        }
        for (const auto& unknown : analysis.unknown) {
            result["unknown"].append(unknown);
        }
        
        reply["result"] = result;
        return reply;
        
    } catch (const std::exception& e) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("PSBT analysis failed: ") + e.what();
        return reply;
    }
}

Json::Value combinepsbt_handler(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value reply;
    reply["rpc_schema"] = "din.rpc.v1";
    
    // Validate parameters
    if (!params.isMember("psbts") || !params["psbts"].isArray()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbts array parameter required";
        return reply;
    }
    
    const Json::Value& psbts = params["psbts"];
    if (psbts.size() < 2) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "At least 2 PSBTs required for combination";
        return reply;
    }
    
    try {
        // Parse first PSBT as base
        std::string first_psbt_b64 = psbts[0].asString();
        auto first_psbt_bytes = from_base64(first_psbt_b64);
        Psbt combined_psbt;
        if (!deserialize(first_psbt_bytes, combined_psbt)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "Invalid first PSBT format";
            return reply;
        }
        
        // Combine with remaining PSBTs
        for (Json::ArrayIndex i = 1; i < psbts.size(); ++i) {
            std::string psbt_b64 = psbts[i].asString();
            auto psbt_bytes = from_base64(psbt_b64);
            Psbt source_psbt;
            if (!deserialize(psbt_bytes, source_psbt)) {
                reply["error"] = Json::Value(Json::objectValue);
                reply["error"]["code"] = -22;
                reply["error"]["message"] = "Invalid PSBT format at index " + std::to_string(i);
                return reply;
            }
            
            if (!combine_psbt(combined_psbt, source_psbt)) {
                reply["error"] = Json::Value(Json::objectValue);
                reply["error"]["code"] = -22;
                reply["error"]["message"] = "Failed to combine PSBT at index " + std::to_string(i);
                return reply;
            }
        }
        
        // Serialize combined PSBT
        auto combined_psbt_bytes = serialize(combined_psbt);
        std::string combined_psbt_b64 = to_base64(combined_psbt_bytes);
        
        Json::Value result(Json::objectValue);
        result["psbt"] = combined_psbt_b64;
        
        reply["result"] = result;
        return reply;
        
    } catch (const std::exception& e) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("PSBT combination failed: ") + e.what();
        return reply;
    }
}

Json::Value finalizepsbt_handler(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value reply;
    reply["rpc_schema"] = "din.rpc.v1";
    
    // Validate parameters
    if (!params.isMember("psbt") || !params["psbt"].isString()) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required";
        return reply;
    }
    
    std::string psbt_b64 = params["psbt"].asString();
    bool extract = params.get("extract", true).asBool();
    
    try {
        // Decode PSBT
        auto psbt_bytes = from_base64(psbt_b64);
        Psbt psbt;
        if (!deserialize(psbt_bytes, psbt)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "invalid PSBT format";
            return reply;
        }
        
        // Validate PSBT
        std::string validation_error;
        if (!validate_psbt(psbt, validation_error)) {
            reply["error"] = Json::Value(Json::objectValue);
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "PSBT validation failed: " + validation_error;
            return reply;
        }
        
        // Check if PSBT is complete (has all required signatures)
        bool complete = is_psbt_complete(psbt);
        
        Json::Value result(Json::objectValue);
        result["complete"] = complete;
        
        if (complete && extract) {
            // Extract final transaction
            auto final_tx_bytes = extract_transaction(psbt);
            if (!final_tx_bytes.empty()) {
                // Convert to hex
                std::string hex_tx;
                hex_tx.reserve(final_tx_bytes.size() * 2);
                const char* hex_chars = "0123456789abcdef";
                for (uint8_t byte : final_tx_bytes) {
                    hex_tx.push_back(hex_chars[(byte >> 4) & 0xF]);
                    hex_tx.push_back(hex_chars[byte & 0xF]);
                }
                result["hex"] = hex_tx;
                
                // Update metrics
                WalletMetrics::incrementPsbtFinalized();
            }
        } else {
            // Return updated PSBT (potentially with finalized inputs)
            auto updated_psbt_bytes = serialize(psbt);
            std::string updated_psbt_b64 = to_base64(updated_psbt_bytes);
            result["psbt"] = updated_psbt_b64;
        }
        
        reply["result"] = result;
        return reply;
        
    } catch (const std::exception& e) {
        reply["error"] = Json::Value(Json::objectValue);
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("PSBT finalization failed: ") + e.what();
        return reply;
    }
}

} // namespace rpc
} // namespace din
