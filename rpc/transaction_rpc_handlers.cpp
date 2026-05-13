// SPDX-License-Identifier: MIT
// Dinero - Transaction RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <stdexcept>

// Use dinero namespace for consistency
using namespace dinero;

// Transaction operations
json::json rpc_tx_send(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "to");
        RpcValidation::require_number(params, "amount");
        
        std::string to_address = params["to"].get<std::string>();
        double amount = params["amount"].get<double>();
        double fee_rate = RpcValidation::get_number(params, "fee_rate", 0.0001); // Default fee rate
        bool subtract_fee = RpcValidation::get_bool(params, "subtract_fee", false);
        bool dry_run = RpcValidation::get_bool(params, "dry_run", false);
        
        // Parse UTXO selection if provided
        std::vector<std::string> selected_utxos;
        if (params.contains("utxos") && params["utxos"].is_array()) {
            for (const auto& utxo : params["utxos"]) {
                if (utxo.is_string()) {
                    selected_utxos.push_back(utxo.get<std::string>());
                }
            }
        }
        
        auto& wallet_manager = WalletManager::getInstance();
        
        // Validate address format
        if (!wallet_manager.isValidAddress(to_address)) {
            json::json error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Invalid address format";
            return error;
        }
        
        // Create transaction (dry run or actual)
        auto tx_result = wallet_manager.createTransaction(
            to_address, amount, fee_rate, subtract_fee, selected_utxos, dry_run
        );
        
        json::json result;
        result["to"] = to_address;
        result["amount"] = amount;
        result["fee"] = tx_result.fee;
        result["fee_rate"] = fee_rate;
        result["subtract_fee"] = subtract_fee;
        result["dry_run"] = dry_run;
        
        if (dry_run) {
            // Dry run - show funding plan without broadcasting
            result["funding_plan"] = json::json(json::arrayjson);
            for (const auto& input : tx_result.inputs) {
                json::json input_obj;
                input_obj["txid"] = input.txid;
                input_obj["vout"] = input.vout;
                input_obj["amount"] = input.amount;
                result["funding_plan"].push_back(input_obj);
            }
            result["total_input"] = tx_result.total_input;
            result["change"] = tx_result.change;
            result["change_address"] = tx_result.change_address;
            result["message"] = "Transaction created successfully (dry run - not broadcast)";
        } else {
            // Actual send - broadcast transaction
            result["txid"] = tx_result.txid;
            result["broadcasted"] = tx_result.broadcasted;
            result["message"] = tx_result.broadcasted ? 
                "Transaction sent successfully" : 
                "Transaction created but broadcast failed";
        }
        
        dinero::g_logger.info("Transaction " + (dry_run ? "dry run" : "sent") + 
                    ": " + to_address + " amount: " + std::to_string(amount));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to send transaction: ") + e.what();
        return error;
    }
}
