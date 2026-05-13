#include "rpc/rpc_registry.h"
#include "rpc/payment_monitor.h"
#include "common/logger.h"
#include <iostream>

extern RpcRegistry g_rpcRegistry;

// Global PaymentMonitor instance (set by daemon at startup)
dinero::rpc::PaymentMonitor* g_payment_monitor = nullptr;

namespace din {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// Payment RPC Implementation Functions
// ═══════════════════════════════════════════════════════════════

din::Json payment_watch_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        dinero::g_logger.info("[Payment RPC] payment.watch called, params type: " +
                              std::to_string(params.type()));

        if (params.isArray() && params.size() > 0) {
            dinero::g_logger.info("[Payment RPC] params[0] type: " +
                                  std::to_string(params[0].type()));
        }

        if (!g_payment_monitor) {
            result["error"] = "Payment monitor not initialized";
            result["code"] = -32000;
            return result;
        }

        // Parse parameters - support both positional array and named object
        dinero::rpc::WatchConfig config;

        if (params.isArray() && params.size() > 0) {
            // Check if first param is an object (CLI sends JSON as params[0])
            if (params[0].isObject()) {
                // Already parsed JSON object
                const din::Json& obj = params[0];
                if (!obj.isMember("address") || !obj["address"].isString()) {
                    result["error"] = "Missing or invalid 'address' parameter";
                    result["code"] = -32602;
                    return result;
                }

                config.address = obj["address"].asString();

                if (obj.isMember("expected_amount")) {
                    if (obj["expected_amount"].isDouble()) {
                        config.expected_amount_una = static_cast<uint64_t>(
                            obj["expected_amount"].asDouble() * 1e8
                        );
                    } else if (obj["expected_amount"].isUInt64()) {
                        config.expected_amount_una = obj["expected_amount"].asUInt64();
                    }
                }

                if (obj.isMember("webhook_url") && obj["webhook_url"].isString()) {
                    config.webhook_url = obj["webhook_url"].asString();
                }

                if (obj.isMember("min_confirmations") && obj["min_confirmations"].isUInt()) {
                    config.min_confirmations = obj["min_confirmations"].asUInt();
                }

                if (obj.isMember("auto_unwatch") && obj["auto_unwatch"].isBool()) {
                    config.auto_unwatch_after_payment = obj["auto_unwatch"].asBool();
                }
            } else if (params[0].isString()) {
                // Try to parse string as JSON (common when CLI sends '{"address":"..."}')
                std::string json_str = params[0].asString();
                if (json_str.front() == '{' && json_str.back() == '}') {
                    ::Json::CharReaderBuilder builder;
                    ::Json::CharReader* reader = builder.newCharReader();
                    din::Json obj;
                    std::string errors;
                    bool parsed = reader->parse(json_str.c_str(), json_str.c_str() + json_str.size(), &obj, &errors);
                    delete reader;
                    if (parsed && obj.isObject()) {
                        // Successfully parsed JSON from string
                        if (!obj.isMember("address") || !obj["address"].isString()) {
                            result["error"] = "Missing or invalid 'address' parameter in JSON";
                            result["code"] = -32602;
                            return result;
                        }

                        config.address = obj["address"].asString();

                        if (obj.isMember("expected_amount")) {
                            if (obj["expected_amount"].isDouble()) {
                                config.expected_amount_una = static_cast<uint64_t>(
                                    obj["expected_amount"].asDouble() * 1e8
                                );
                            } else if (obj["expected_amount"].isUInt64()) {
                                config.expected_amount_una = obj["expected_amount"].asUInt64();
                            }
                        }

                        if (obj.isMember("webhook_url") && obj["webhook_url"].isString()) {
                            config.webhook_url = obj["webhook_url"].asString();
                        }

                        if (obj.isMember("min_confirmations") && obj["min_confirmations"].isUInt()) {
                            config.min_confirmations = obj["min_confirmations"].asUInt();
                        }

                        if (obj.isMember("auto_unwatch") && obj["auto_unwatch"].isBool()) {
                            config.auto_unwatch_after_payment = obj["auto_unwatch"].asBool();
                        }
                    } else {
                        // Not valid JSON, treat as address
                        config.address = json_str;
                    }
                } else {
                    // Plain string address
                    config.address = json_str;
                }
            } else {
                // Positional strings: payment.watch "rdin1test..." [expected_amount]
                config.address = params[0].asString();
                if (params.size() > 1 && params[1].isDouble()) {
                    config.expected_amount_una = static_cast<uint64_t>(
                        params[1].asDouble() * 1e8
                    );
                }
            }
        } else if (params.isObject()) {
            // Named: payment.watch '{"address":"rdin1test...", ...}'
            if (!params.isMember("address") || !params["address"].isString()) {
                result["error"] = "Missing or invalid 'address' parameter";
                result["code"] = -32602;
                return result;
            }

            config.address = params["address"].asString();

            // Optional: expected amount
            if (params.isMember("expected_amount")) {
                if (params["expected_amount"].isDouble()) {
                    config.expected_amount_una = static_cast<uint64_t>(
                        params["expected_amount"].asDouble() * 1e8
                    );
                } else if (params["expected_amount"].isUInt64()) {
                    config.expected_amount_una = params["expected_amount"].asUInt64();
                }
            }

            // Optional: webhook URL
            if (params.isMember("webhook_url") && params["webhook_url"].isString()) {
                config.webhook_url = params["webhook_url"].asString();
            }

            // Optional: min confirmations
            if (params.isMember("min_confirmations") && params["min_confirmations"].isUInt()) {
                config.min_confirmations = params["min_confirmations"].asUInt();
            }

            // Optional: auto unwatch
            if (params.isMember("auto_unwatch") && params["auto_unwatch"].isBool()) {
                config.auto_unwatch_after_payment = params["auto_unwatch"].asBool();
            }
        } else {
            result["error"] = "Invalid parameters: expected object or array";
            result["code"] = -32602;
            return result;
        }

        // Optional: client_id (for WebSocket notifications)
        config.client_id = ctx.client_id.empty() ? "default" : ctx.client_id;

        // Start watching
        std::string watch_id = g_payment_monitor->watch_address(config);

        result["watch_id"] = watch_id;
        result["status"] = "watching";
        result["address"] = config.address;

        if (config.expected_amount_una > 0) {
            result["expected_amount_una"] = static_cast<Json::UInt64>(config.expected_amount_una);
            result["expected_amount_din"] = static_cast<double>(config.expected_amount_una) / 1e8;
        }

        result["rpc_schema"] = "din.payment.v1";

        dinero::g_logger.info("[Payment RPC] Started watching address: " + config.address);

    } catch (const std::exception& e) {
        result["error"] = std::string("payment.watch error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json payment_unwatch_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!g_payment_monitor) {
            result["error"] = "Payment monitor not initialized";
            result["code"] = -32000;
            return result;
        }

        // Parse parameters
        if (!params.isMember("watch_id") || !params["watch_id"].isString()) {
            result["error"] = "Missing or invalid 'watch_id' parameter";
            result["code"] = -32602;
            return result;
        }

        std::string watch_id = params["watch_id"].asString();

        // Stop watching
        bool success = g_payment_monitor->unwatch(watch_id);

        result["success"] = success;

        if (success) {
            result["message"] = "Watch removed successfully";
        } else {
            result["message"] = "Watch not found";
        }

        result["rpc_schema"] = "din.payment.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("payment.unwatch error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json payment_status_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!g_payment_monitor) {
            result["error"] = "Payment monitor not initialized";
            result["code"] = -32000;
            return result;
        }

        // Check if querying by watch_id or address
        if (params.isMember("watch_id") && params["watch_id"].isString()) {
            // Query specific watch
            std::string watch_id = params["watch_id"].asString();

            auto watch_state = g_payment_monitor->get_watch_status(watch_id);

            if (!watch_state.has_value()) {
                result["error"] = "Watch not found";
                result["code"] = -32602;
                return result;
            }

            const auto& state = watch_state.value();

            result["watch_id"] = state.watch_id;
            result["address"] = state.config.address;
            result["active"] = state.active;

            auto created_time = std::chrono::duration_cast<std::chrono::seconds>(
                state.created_at.time_since_epoch()
            ).count();
            result["created_at"] = static_cast<Json::Int64>(created_time);

            // Payments array
            din::Json payments_array = din::arr();
            for (const auto& payment : state.detected_payments) {
                din::Json payment_obj = din::obj();
                payment_obj["txid"] = payment.txid;
                payment_obj["amount_una"] = static_cast<Json::UInt64>(payment.amount_una);
                payment_obj["amount_din"] = static_cast<double>(payment.amount_una) / 1e8;
                payment_obj["confirmations"] = payment.confirmations;

                std::string risk_str;
                switch (payment.risk) {
                    case dinero::rpc::RiskLevel::NONE:   risk_str = "none"; break;
                    case dinero::rpc::RiskLevel::LOW:    risk_str = "low"; break;
                    case dinero::rpc::RiskLevel::MEDIUM: risk_str = "medium"; break;
                    case dinero::rpc::RiskLevel::HIGH:   risk_str = "high"; break;
                }
                payment_obj["risk"] = risk_str;

                auto detected_time = std::chrono::duration_cast<std::chrono::seconds>(
                    payment.detected_at.time_since_epoch()
                ).count();
                payment_obj["detected_at"] = static_cast<Json::Int64>(detected_time);

                payments_array.append(payment_obj);
            }

            result["payments"] = payments_array;
            result["payment_count"] = static_cast<int>(state.detected_payments.size());

        } else if (params.isMember("address") && params["address"].isString()) {
            // Query by address
            std::string address = params["address"].asString();

            uint64_t since_timestamp = 0;
            if (params.isMember("since") && params["since"].isUInt64()) {
                since_timestamp = params["since"].asUInt64();
            }

            auto payments = g_payment_monitor->get_payments(address, since_timestamp);

            result["address"] = address;

            din::Json payments_array = din::arr();
            for (const auto& payment : payments) {
                din::Json payment_obj = din::obj();
                payment_obj["txid"] = payment.txid;
                payment_obj["amount_una"] = static_cast<Json::UInt64>(payment.amount_una);
                payment_obj["amount_din"] = static_cast<double>(payment.amount_una) / 1e8;
                payment_obj["confirmations"] = payment.confirmations;

                std::string risk_str;
                switch (payment.risk) {
                    case dinero::rpc::RiskLevel::NONE:   risk_str = "none"; break;
                    case dinero::rpc::RiskLevel::LOW:    risk_str = "low"; break;
                    case dinero::rpc::RiskLevel::MEDIUM: risk_str = "medium"; break;
                    case dinero::rpc::RiskLevel::HIGH:   risk_str = "high"; break;
                }
                payment_obj["risk"] = risk_str;

                auto detected_time = std::chrono::duration_cast<std::chrono::seconds>(
                    payment.detected_at.time_since_epoch()
                ).count();
                payment_obj["detected_at"] = static_cast<Json::Int64>(detected_time);

                payments_array.append(payment_obj);
            }

            result["payments"] = payments_array;
            result["payment_count"] = static_cast<int>(payments.size());

        } else {
            result["error"] = "Must provide either 'watch_id' or 'address' parameter";
            result["code"] = -32602;
            return result;
        }

        result["rpc_schema"] = "din.payment.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("payment.status error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json payment_analyze_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!g_payment_monitor) {
            result["error"] = "Payment monitor not initialized";
            result["code"] = -32000;
            return result;
        }

        // Parse parameters
        if (!params.isMember("txid") || !params["txid"].isString()) {
            result["error"] = "Missing or invalid 'txid' parameter";
            result["code"] = -32602;
            return result;
        }

        std::string txid = params["txid"].asString();

        // Analyze transaction
        auto payment = g_payment_monitor->analyze_transaction(txid);

        result["txid"] = payment.txid;
        result["confirmations"] = payment.confirmations;

        // Risk level
        std::string risk_str;
        switch (payment.risk) {
            case dinero::rpc::RiskLevel::NONE:   risk_str = "none"; break;
            case dinero::rpc::RiskLevel::LOW:    risk_str = "low"; break;
            case dinero::rpc::RiskLevel::MEDIUM: risk_str = "medium"; break;
            case dinero::rpc::RiskLevel::HIGH:   risk_str = "high"; break;
        }
        result["risk"] = risk_str;

        // Risk factors
        din::Json risk_factors = din::arr();

        if (payment.rbf_enabled) {
            risk_factors.append("RBF enabled - transaction can be replaced");
        }

        if (payment.double_spend_detected) {
            risk_factors.append("Double-spend attempt detected");
        }

        if (payment.fee_rate_una_per_byte < 1.0) {
            risk_factors.append("Low fee rate - may not confirm quickly");
        }

        if (payment.peer_count < 4) {
            risk_factors.append("Limited network connectivity");
        }

        result["risk_factors"] = risk_factors;

        // Detailed metrics
        result["rbf_enabled"] = payment.rbf_enabled;
        result["double_spend_detected"] = payment.double_spend_detected;
        result["fee_rate_sat_per_byte"] = payment.fee_rate_una_per_byte;
        result["peer_count"] = payment.peer_count;

        // Recommendation
        if (payment.risk == dinero::rpc::RiskLevel::NONE) {
            result["recommendation"] = "Safe - confirmed on-chain";
        } else if (payment.risk == dinero::rpc::RiskLevel::LOW) {
            result["recommendation"] = "Low risk - safe for instant acceptance";
        } else if (payment.risk == dinero::rpc::RiskLevel::MEDIUM) {
            result["recommendation"] = "Medium risk - wait for 1 confirmation";
        } else {
            result["recommendation"] = "High risk - wait for multiple confirmations";
        }

        result["rpc_schema"] = "din.payment.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("payment.analyze error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

} // namespace rpc
} // namespace din

// ═══════════════════════════════════════════════════════════════
// Registration Function (Outside Namespace - for global linkage)
// ═══════════════════════════════════════════════════════════════

void registerPaymentRPC() {
    dinero::g_logger.info("[Payment RPC] Registering payment RPC methods...");

    // payment.watch
    g_rpcRegistry.registerHandler("payment.watch",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::payment_watch_impl(ctx, params);
        },
        "payment");

    // payment.unwatch
    g_rpcRegistry.registerHandler("payment.unwatch",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::payment_unwatch_impl(ctx, params);
        },
        "payment");

    // payment.status
    g_rpcRegistry.registerHandler("payment.status",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::payment_status_impl(ctx, params);
        },
        "payment");

    // payment.analyze
    g_rpcRegistry.registerHandler("payment.analyze",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::payment_analyze_impl(ctx, params);
        },
        "payment");

    dinero::g_logger.info("✅ Payment RPC methods registered (4 methods)");

    std::cout << "[Payment RPC] Registered methods:" << std::endl;
    std::cout << "[Payment RPC]   - payment.watch    (Start watching address for payments)" << std::endl;
    std::cout << "[Payment RPC]   - payment.unwatch  (Stop watching address)" << std::endl;
    std::cout << "[Payment RPC]   - payment.status   (Get payment status)" << std::endl;
    std::cout << "[Payment RPC]   - payment.analyze  (Analyze transaction risk)" << std::endl;
}
