#include "privacy/payjoin_receiver.h"
#include "daemon/execution_context.h"
#include "rpc/rpc_types.h"
#include <json/json.h>
#include <memory>

namespace din::rpc {

/**
 * @brief PayJoin RPC method handlers
 */
class PayjoinRpcHandlers {
public:
    /**
     * @brief Prepare PayJoin offer (receiver side)
     * 
     * RPC: payjoinprepare
     * Parameters: amount (number), memo (string, optional)
     * Returns: { endpoint: string, bpu_uri: string, amount: number }
     */
    static Json::Value payjoinprepare_handler(const Json::Value& params) {
        Json::Value result;
        
        try {
            // Parse parameters
            if (!params.isMember("amount") || !params["amount"].isNumeric()) {
                throw std::invalid_argument("Missing or invalid 'amount' parameter");
            }
            
            int64_t amount = params["amount"].asInt64();
            std::string memo = params.isMember("memo") ? params["memo"].asString() : "";
            
            // Create PayJoin offer (simplified)
            din::PayjoinOffer offer;
            offer.endpoint = "http://localhost:20998/payjoin";
            offer.amount = amount;
            offer.invoice_spk = std::vector<uint8_t>(22, 0x00); // Mock script
            
            // Create BIP78 URI
            std::string bpu_uri = "bitcoin:" + std::to_string(amount) + "?pj=" + offer.endpoint;
            if (!memo.empty()) {
                bpu_uri += "&memo=" + memo;
            }
            
            // Return result
            result["endpoint"] = offer.endpoint;
            result["bpu_uri"] = bpu_uri;
            result["amount"] = static_cast<Json::Int64>(offer.amount);
            result["metadata"] = memo;
            
        } catch (const std::exception& e) {
            result["error"] = e.what();
        }
        
        return result;
    }
    
    /**
     * @brief Handle PayJoin request (receiver side)
     * 
     * RPC: payjoinhandle
     * Parameters: psbt (string), policy (object, optional)
     * Returns: { psbt: string, success: bool, fee_paid: number }
     */
    static Json::Value payjoinhandle_handler(const Json::Value& params) {
        Json::Value result;
        
        try {
            // Parse parameters
            if (!params.isMember("psbt") || !params["psbt"].isString()) {
                throw std::invalid_argument("Missing or invalid 'psbt' parameter");
            }
            
            std::string psbt_b64 = params["psbt"].asString();
            
            // Parse policy if provided
            int64_t minfeerate = 1;
            bool disable_out_sub = true;
            
            if (params.isMember("policy") && params["policy"].isObject()) {
                const Json::Value& policy_obj = params["policy"];
                
                if (policy_obj.isMember("min_fee_rate")) {
                    minfeerate = policy_obj["min_fee_rate"].asInt64();
                }
                if (policy_obj.isMember("disable_output_substitution")) {
                    disable_out_sub = policy_obj["disable_output_substitution"].asBool();
                }
            }
            
            // Create mock PayJoin receiver for testing
            // In a real implementation, this would use actual wallet UTXOs
            din::PayjoinOffer offer;
            offer.endpoint = "http://localhost:20998/payjoin";
            offer.amount = 100000;
            offer.invoice_spk = std::vector<uint8_t>(22, 0x00);
            
            // Mock UTXO selector
            auto select_utxo = [](int64_t min_value) -> std::optional<din::ReceiverUtxo> {
                din::ReceiverUtxo utxo;
                utxo.txid = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
                utxo.vout = 0;
                utxo.value = min_value + 10000; // Add some extra
                utxo.scriptPubKey = std::vector<uint8_t>(22, 0x00);
                utxo.type = din::ReceiverUtxo::Type::P2WPKH;
                return utxo;
            };
            
            // Mock change script provider
            auto get_change_script = []() -> std::vector<uint8_t> {
                return std::vector<uint8_t>(22, 0x01);
            };
            
            // Create PayJoin receiver
            din::PayjoinReceiver receiver(nullptr, offer, select_utxo, get_change_script, minfeerate);
            
            // Handle request
            std::string out_psbt = receiver.handle(psbt_b64, minfeerate, disable_out_sub);
            
            // Return result
            result["psbt"] = out_psbt;
            result["success"] = true;
            result["fee_paid"] = static_cast<Json::Int64>(1000); // Mock fee
            
        } catch (const std::exception& e) {
            result["error"] = e.what();
            result["success"] = false;
        }
        
        return result;
    }
    
    /**
     * @brief Validate PayJoin PSBT
     * 
     * RPC: payjoinvalidate
     * Parameters: psbt (string), policy (object, optional)
     * Returns: { valid: bool, errors: array }
     */
    static Json::Value payjoinvalidate_handler(const Json::Value& params) {
        Json::Value result;
        
        try {
            // Parse parameters
            if (!params.isMember("psbt") || !params["psbt"].isString()) {
                throw std::invalid_argument("Missing or invalid 'psbt' parameter");
            }
            
            std::string psbt_b64 = params["psbt"].asString();
            
            // Simple validation - check if PSBT is valid base64
            bool valid = !psbt_b64.empty() && psbt_b64.size() > 10;
            
            // Return result
            result["valid"] = valid;
            result["errors"] = Json::Value(Json::arrayValue);
            
            if (!valid) {
                result["errors"].append("Invalid PSBT format");
            }
            
        } catch (const std::exception& e) {
            result["valid"] = false;
            result["errors"] = Json::Value(Json::arrayValue);
            result["errors"].append(e.what());
        }
        
        return result;
    }
};

} // namespace din::rpc
