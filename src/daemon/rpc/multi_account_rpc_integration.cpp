#include "din_json.h"
#include "daemon/rpc/multi_account_rpc_handlers.h"
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "common/Log.hpp"

// Global RPC registry reference
extern RpcRegistry g_rpcRegistry;

void registerMultiAccountRpcMethods() {
    try {
        LOG_I("Registering Multi-Account RPC methods...");
        
        // Create a dummy RPCServer for the handlers
        static dinero::RPCServer dummyServer;
        static std::unique_ptr<MultiAccountRpcHandlers> handlers;
        if (!handlers) {
            handlers = std::make_unique<MultiAccountRpcHandlers>(dummyServer);
        }
        
        // Simple multi-account.create method
        g_rpcRegistry.registerHandler("multiaccount.create", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                // Convert din::Json to Json::Value
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                // Call the handler
                Json::Value result = handlers->createAccount(jsonParams);
                
                // Convert back to din::Json
                din::Json dinResult = din::obj();
                if (result.isMember("result")) {
                    dinResult["result"] = result["result"];
                }
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                if (result.isMember("rpc_schema")) {
                    dinResult["rpc_schema"] = result["rpc_schema"].asString();
                }
                if (result.isMember("schema_rev")) {
                    dinResult["schema_rev"] = result["schema_rev"].asInt();
                }
                
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                error["result"] = din::Json();
                return error;
            }
        });
        
        // Simple multiaccount.list method
        g_rpcRegistry.registerHandler("multiaccount.list", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                Json::Value result = handlers->listAccounts(jsonParams);
                
                din::Json dinResult = din::obj();
                if (result.isMember("result")) {
                    dinResult["result"] = result["result"];
                }
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                if (result.isMember("rpc_schema")) {
                    dinResult["rpc_schema"] = result["rpc_schema"].asString();
                }
                if (result.isMember("schema_rev")) {
                    dinResult["schema_rev"] = result["schema_rev"].asInt();
                }
                
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                error["result"] = din::Json();
                return error;
            }
        });
        
        // Register transaction RPC methods
        g_rpcRegistry.registerHandler("multiaccount.sendtransaction", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                Json::Value result = handlers->sendTransaction(jsonParams);
                din::Json dinResult = din::obj();
                dinResult["result"] = result["result"];
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                return error;
            }
        });
        
        g_rpcRegistry.registerHandler("multiaccount.gettransactionhistory", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                Json::Value result = handlers->getTransactionHistory(jsonParams);
                din::Json dinResult = din::obj();
                dinResult["result"] = result["result"];
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                return error;
            }
        });
        
        g_rpcRegistry.registerHandler("multiaccount.getutxos", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                Json::Value result = handlers->getUTXOs(jsonParams);
                din::Json dinResult = din::obj();
                dinResult["result"] = result["result"];
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                return error;
            }
        });
        
        g_rpcRegistry.registerHandler("multiaccount.estimatefee", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            try {
                Json::Value jsonParams;
                if (params.isObject()) {
                    jsonParams = Json::objectValue;
                    for (const auto& key : params.getMemberNames()) {
                        const Json::Value& value = params[key];
                        if (value.isString()) {
                            jsonParams[key] = value.asString();
                        } else if (value.isInt()) {
                            jsonParams[key] = value.asInt();
                        } else if (value.isDouble()) {
                            jsonParams[key] = value.asDouble();
                        } else if (value.isBool()) {
                            jsonParams[key] = value.asBool();
                        }
                    }
                }
                
                Json::Value result = handlers->estimateFee(jsonParams);
                din::Json dinResult = din::obj();
                dinResult["result"] = result["result"];
                if (result.isMember("error")) {
                    dinResult["error"] = result["error"];
                }
                return dinResult;
            } catch (const std::exception& e) {
                din::Json error = din::obj();
                error["error"] = din::obj();
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Internal error: " + std::string(e.what());
                return error;
            }
        });
        
        LOG_I("✅ Multi-Account RPC methods registered successfully");
        LOG_I("📋 Available methods:");
        LOG_I("   multiaccount.create - Create a new account");
        LOG_I("   multiaccount.list - List all accounts");
        LOG_I("   multiaccount.sendtransaction - Send transaction from account");
        LOG_I("   multiaccount.gettransactionhistory - Get transaction history");
        LOG_I("   multiaccount.getutxos - Get account UTXOs");
        LOG_I("   multiaccount.estimatefee - Estimate transaction fees");
        
    } catch (const std::exception& e) {
        LOG_E("Failed to register Multi-Account RPC methods: " + std::string(e.what()));
        throw;
    }
}