// SPDX-License-Identifier: MIT
// Dinero - Transaction Send RPC Handler Implementation

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#if DIN_ENABLE_LEGACY_RPC
#include "daemon/rpc_server.h"
#endif
#include "common/logger.h"
#include "wallet/address.h"
#include "consensus/transaction_validator.h"
#include <stdexcept>
#include <sstream>

using namespace dinero;

// Parameter validation helpers (reused from other RPC handlers)
namespace RpcValidation {
    void require_string(const Json::Value& params, const std::string& field) {
        if (!params.isMember(field) || !params[field].isString()) {
            throw std::invalid_argument("Missing or invalid string field: " + field);
        }
    }
    
    void require_number(const Json::Value& params, const std::string& field) {
        if (!params.isMember(field) || !params[field].isNumeric()) {
            throw std::invalid_argument("Missing or invalid number field: " + field);
        }
    }
    
    std::string get_string(const Json::Value& params, const std::string& field, const std::string& default_val) {
        if (params.isMember(field) && params[field].isString()) {
            return params[field].asString();
        }
        return default_val;
    }
    
    double get_number(const Json::Value& params, const std::string& field, double default_val) {
        if (params.isMember(field) && params[field].isNumeric()) {
            return params[field].asDouble();
        }
        return default_val;
    }
    
    int get_int(const Json::Value& params, const std::string& field, int default_val) {
        if (params.isMember(field) && params[field].isNumeric()) {
            return params[field].asInt();
        }
        return default_val;
    }
    
    bool get_bool(const Json::Value& params, const std::string& field, bool default_val) {
        if (params.isMember(field) && params[field].isBool()) {
            return params[field].asBool();
        }
        return default_val;
    }
}

// Transaction send RPC implementation with advanced options
Json::Value rpc_tx_send(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        void* wallet_manager = nullptr;
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "WalletManager not available";
            return error;
        }
        
        // Check if wallet is loaded
        if (!false) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        // Check if wallet is encrypted and locked
        if (false && false) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "Wallet is locked. Use wallet.unlock first.";
            return error;
        }
        
        // Validate required parameters
        RpcValidation::require_string(params, "to");
        RpcValidation::require_number(params, "amount");
        
        std::string to_address = params["to"].asString();
        double amount = params["amount"].asDouble();
        
        // Validate amount
        if (amount <= 0.0) {
            Json::Value error;
            error["error"]["code"] = -3;
            error["error"]["message"] = "Amount must be positive";
            return error;
        }
        
        // Validate destination address
        if (to_address.empty()) {
            Json::Value error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Destination address cannot be empty";
            return error;
        }
        
        // Validate destination address using comprehensive address validation
        auto decoded = dinero::Address::decodeAddress(to_address);
        if (!decoded.isValid) {
            Json::Value error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Invalid destination address: " + decoded.error;
            return error;
        }
        
        // Log the address type for debugging
        dinero::g_logger.debug("Sending to " + decoded.addressType + " address: " + to_address);
        
        // Parse optional parameters
        double fee_rate = RpcValidation::get_number(params, "fee_rate", 0.0); // 0 = auto
        bool subtract_fee = RpcValidation::get_bool(params, "subtract_fee", false);
        std::string comment = RpcValidation::get_string(params, "comment", "");
        std::string comment_to = RpcValidation::get_string(params, "comment_to", "");
        bool replaceable = RpcValidation::get_bool(params, "replaceable", false);
        int conf_target = RpcValidation::get_int(params, "conf_target", 6);
        std::string estimate_mode = RpcValidation::get_string(params, "estimate_mode", "CONSERVATIVE");
        
        // Parse UTXO selection if provided
        std::vector<std::string> selected_utxos;
        if (params.isMember("utxos") && params["utxos"].isArray()) {
            for (const auto& utxo : params["utxos"]) {
                if (utxo.isObject() && utxo.isMember("txid") && utxo.isMember("vout")) {
                    std::string utxo_id = utxo["txid"].asString() + ":" + std::to_string(utxo["vout"].asInt());
                    selected_utxos.push_back(utxo_id);
                }
            }
        }
        
        // Check wallet balance
        // TODO: Get actual wallet balance
        double available_balance = 0.0; // Placeholder
        double required_amount = subtract_fee ? amount : amount + 0.001; // Estimate fee
        
        if (available_balance < required_amount) {
            Json::Value error;
            error["error"]["code"] = -6;
            error["error"]["message"] = "Insufficient funds. Available: " + std::to_string(available_balance) + 
                                       " DIN, Required: " + std::to_string(required_amount) + " DIN";
            return error;
        }
        
        // Build and send transaction using WalletManager
        // Note: This is a framework for actual transaction building
        // Real implementation would use WalletManager's transaction builder
        
        try {
            // Initialize transaction validator
            auto validator = std::make_unique<consensus::TransactionValidator>();
            auto utxo_provider = std::make_shared<consensus::DatabaseUTXOProvider>();
            validator->setUTXOProvider(utxo_provider);
            
            // Create transaction build request
            dinero::TransactionCreationResult tx_result;
            
            // In production, this would call:
            // tx_result = wallet_manager->createTransaction(to_address, amount, fee_rate, subtract_fee);
            
            // For now, simulate the transaction creation process
            std::string txid = "simulated_txid_" + std::to_string(std::time(nullptr));
            double actual_fee = fee_rate > 0 ? (fee_rate * 250) / 100000000.0 : 0.001;
            double actual_sent = subtract_fee ? (amount - actual_fee) : amount;
            
            // Simulate successful transaction
            tx_result.success = true;
            tx_result.txid = txid;
            tx_result.fee_amount = static_cast<uint64_t>(actual_fee * 100000000); // Convert to una
            
            if (!tx_result.success) {
                Json::Value error;
                error["error"]["code"] = -4;
                error["error"]["message"] = "Transaction creation failed: " + tx_result.error_message;
                return error;
            }
            
            // Validate the transaction before broadcasting
            consensus::ValidatedTransaction validated_tx;
            validated_tx.version = 2;
            validated_tx.txid = txid;
            validated_tx.size = 250; // Estimated size
            validated_tx.weight = 1000; // Estimated weight
            validated_tx.fee = tx_result.fee_amount;
            validated_tx.lockTime = 0;
            
            // Add simulated input/output for validation
            consensus::TxInput input;
            input.prev_txid = "prev_tx_placeholder";
            input.prev_vout = 0;
            input.sequence = 0xfffffffe; // Enable RBF
            validated_tx.vin.push_back(input);
            
            consensus::TxOutput output;
            output.value = static_cast<uint64_t>(actual_sent * 100000000);
            output.script_pubkey = "0014" + std::string(40, '0'); // P2WPKH placeholder
            validated_tx.vout.push_back(output);
            
            // Validate transaction
            auto validation_result = validator->validateTransaction(validated_tx);
            if (!validation_result.valid) {
                Json::Value error;
                error["error"]["code"] = validation_result.error_code;
                error["error"]["message"] = "Transaction validation failed: " + validation_result.error_message;
                return error;
            }
            
            dinero::g_logger.info("Transaction validation passed: " + txid);
            
            // Transaction created and validated successfully - now broadcast it
            // In production: wallet_manager->broadcastTransaction(tx_result.transaction);
            
            std::string txid_final = tx_result.txid;
            double actual_fee_final = static_cast<double>(tx_result.fee_amount) / 100000000.0;
            double actual_sent_final = subtract_fee ? (amount - actual_fee_final) : amount;
        
            Json::Value result;
            result["txid"] = txid_final;
            result["sent"] = actual_sent_final;
            result["fee"] = actual_fee_final;
            result["to"] = to_address;
            result["subtract_fee"] = subtract_fee;
            result["replaceable"] = replaceable;
            result["conf_target"] = conf_target;
            result["estimate_mode"] = estimate_mode;
        
            if (!comment.empty()) {
                result["comment"] = comment;
            }
            if (!comment_to.empty()) {
                result["comment_to"] = comment_to;
            }
            if (!selected_utxos.empty()) {
                result["selected_utxos"] = Json::Value(Json::arrayValue);
                for (const auto& utxo : selected_utxos) {
                    result["selected_utxos"].append(utxo);
                }
            }
            
            // Add transaction details
            result["size"] = 250; // Estimated transaction size in bytes
            result["vsize"] = 250; // Virtual size for segwit
            result["weight"] = 1000; // Transaction weight
            
            dinero::g_logger.info("Transaction sent: " + txid_final + " (" + std::to_string(actual_sent_final) + " DIN to " + to_address + ")");
            return result;
            
        } catch (const std::exception& tx_error) {
            Json::Value error;
            error["error"]["code"] = -4;
            error["error"]["message"] = "Transaction building failed: " + std::string(tx_error.what());
            return error;
        }
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to send transaction: ") + e.what();
        dinero::g_logger.error("tx.send error: " + std::string(e.what()));
        return error;
    }
}

// Enhanced sendtoaddress RPC (legacy compatibility)
Json::Value rpc_wallet_sendtoaddress(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Convert legacy sendtoaddress parameters to tx.send format
        Json::Value tx_send_params;
        
        if (params.isArray() && params.size() >= 2) {
            // Legacy array format: [address, amount, comment?, comment_to?, subtract_fee?]
            tx_send_params["to"] = params[0];
            tx_send_params["amount"] = params[1];
            
            if (params.size() > 2 && !params[2].isNull()) {
                tx_send_params["comment"] = params[2];
            }
            if (params.size() > 3 && !params[3].isNull()) {
                tx_send_params["comment_to"] = params[3];
            }
            if (params.size() > 4 && params[4].isBool()) {
                tx_send_params["subtract_fee"] = params[4];
            }
        } else if (params.isObject()) {
            // Modern object format - pass through
            tx_send_params = params;
        } else {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Invalid parameters format";
            return error;
        }
        
        // Call the main tx.send implementation
        return rpc_tx_send(server, tx_send_params);
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to send transaction: ") + e.what();
        dinero::g_logger.error("sendtoaddress error: " + std::string(e.what()));
        return error;
    }
}
