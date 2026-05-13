// SPDX-License-Identifier: MIT
// Dinero CLI - Production-Grade Enhancements
// Wallet scoping, output contracts, security checks, connection transparency

#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sys/stat.h>
#include <json/json.h>

namespace dinero::cli {

// CLI API version for output contract stability
const std::string CLI_API_VERSION = "1.0.0";

// Wallet scoping validation
class WalletScoping {
public:
    // Commands that require wallet context
    static const std::set<std::string> WALLET_REQUIRED_COMMANDS;
    
    static bool requiresWallet(const std::string& command_group, const std::string& command) {
        std::string full_command = command_group + "." + command;
        return WALLET_REQUIRED_COMMANDS.count(full_command) > 0;
    }
    
    static std::string validateWalletContext(const std::string& wallet_context, 
                                           const std::string& command_group,
                                           const std::string& command) {
        if (requiresWallet(command_group, command) && wallet_context.empty()) {
            return "Error: Wallet context required for '" + command_group + " " + command + 
                   "'. Use --wallet <name> or -w <name>";
        }
        return "";
    }
    
    // Add wallet parameter to RPC call if needed
    static Json::Json::Value addWalletContext(Json::Json::Value params, const std::string& wallet_context,
                                      const std::string& command_group, const std::string& command) {
        if (requiresWallet(command_group, command) && !wallet_context.empty()) {
            params["wallet"] = wallet_context;
        }
        return params;
    }
};

// Output contract enforcement for stable automation
class OutputContract {
public:
    enum class Format {
        JSON_STABLE,    // Stable, versioned JSON for automation
        JSON_PRETTY,    // Pretty JSON for humans
        PLAIN,          // Simple text output
        TABLE           // Human-readable tables
    };
    
    static Format parseFormat(const std::string& format_str) {
        if (format_str == "json") return Format::JSON_STABLE;
        if (format_str == "json-pretty") return Format::JSON_PRETTY;
        if (format_str == "plain") return Format::PLAIN;
        if (format_str == "table") return Format::TABLE;
        return Format::JSON_STABLE; // Default to stable
    }
    
    static Json::Json::Value createStableOutput(const Json::Json::Value& rpc_result, 
                                        const std::string& command_group,
                                        const std::string& command,
                                        bool success = true,
                                        const std::string& error_message = "") {
        Json::Json::Value output;
        output["cli_version"] = CLI_API_VERSION;
        output["command"] = command_group + "." + command;
        output["timestamp"] = static_cast<int64_t>(std::time(nullptr));
        output["success"] = success;
        
        if (success) {
            output["result"] = rpc_result;
        } else {
            output["error"] = error_message;
        }
        
        return output;
    }
    
    static void printOutput(const Json::Json::Value& data, Format format, bool color = true) {
        switch (format) {
            case Format::JSON_STABLE:
            case Format::JSON_PRETTY: {
                Json::StreamWriterBuilder builder;
                if (format == Format::JSON_PRETTY) {
                    builder["indentation"] = "  ";
                } else {
                    builder["indentation"] = "";
                }
                std::cout << Json::writeString(builder, data) << std::endl;
                break;
            }
            case Format::PLAIN: {
                if (data.contains("result")) {
                    printPlainResult(data["result"]);
                } else if (data.contains("error")) {
                    std::cerr << data["error"].asString() << std::endl;
                } else {
                    std::cout << data.asString() << std::endl;
                }
                break;
            }
            case Format::TABLE: {
                printTableResult(data, color);
                break;
            }
        }
    }
    
private:
    static void printPlainResult(const Json::Json::Value& result) {
        if (result.isString()) {
            std::cout << result.asString() << std::endl;
        } else if (result.isNumber()) {
            std::cout << result.asString() << std::endl;
        } else if (result.isObject()) {
            for (const auto& key : result.getMemberNames()) {
                std::cout << key << ": " << result[key].asString() << std::endl;
            }
        } else if (result.isArray()) {
            for (const auto& item : result) {
                std::cout << item.asString() << std::endl;
            }
        }
    }
    
    static void printTableResult(const Json::Json::Value& data, bool color) {
        // Simple table formatting - can be enhanced
        if (data.contains("result")) {
            const Json::Json::Value& result = data["result"];
            
            if (result.isObject()) {
                for (const auto& key : result.getMemberNames()) {
                    std::cout << std::left << std::setw(20) << key << ": " 
                              << result[key].asString() << std::endl;
                }
            } else {
                printPlainResult(result);
            }
        }
    }
};

// Security checks for production deployment
class SecurityChecks {
public:
    struct PermissionCheck {
        bool valid;
        std::string message;
        std::string fix_command;
    };
    
    static PermissionCheck checkCookiePermissions(const std::string& cookie_path) {
        struct stat st;
        if (stat(cookie_path.c_str(), &st) != 0) {
            return {false, "Cannot access cookie file: " + cookie_path, ""};
        }
        
        // Check if permissions are too loose (should be 0600)
        mode_t perms = st.st_mode & 0777;
        if (perms != 0600) {
            std::ostringstream msg;
            msg << "Cookie file has insecure permissions: " << std::oct << perms << std::dec;
            
            return {
                false, 
                msg.str(),
                "chmod 600 " + cookie_path
            };
        }
        
        return {true, "Cookie permissions OK", ""};
    }
    
    static Json::Json::Value redactSensitiveFields(Json::Json::Value value) {
        // Redact sensitive fields in JSON output
        static const std::vector<std::string> sensitive_fields = {
            "seed_words", "mnemonic", "private_key", "privkey", 
            "passphrase", "password", "cookie", "auth", "secret"
        };
        
        if (value.isObject()) {
            for (const auto& field : sensitive_fields) {
                if (value.contains(field)) {
                    value[field] = "[REDACTED]";
                }
            }
            
            // Recursively redact nested objects
            for (auto& member : value.getMemberNames()) {
                value[member] = redactSensitiveFields(value[member]);
            }
        } else if (value.isArray()) {
            for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
                value[i] = redactSensitiveFields(value[i]);
            }
        }
        
        return value;
    }
    
    static void warnInsecurePermissions(const PermissionCheck& check) {
        if (!check.valid) {
            std::cerr << "Security Warning: " << check.message << std::endl;
            if (!check.fix_command.empty()) {
                std::cerr << "Fix with: " << check.fix_command << std::endl;
            }
        }
    }
};

// Connection transparency for debugging
class ConnectionInfo {
public:
    struct Connection {
        std::string rpc_url;
        std::string cookie_path;
        std::string nodeinfo_path;
        std::string discovery_method;  // "explicit" or "auto"
        std::string network;
        std::string datadir;
    };
    
    static void showConnectionInfo(const Connection& conn, bool verbose = false) {
        if (verbose) {
            std::cerr << "Connection Details:" << std::endl;
            std::cerr << "  Network: " << conn.network << std::endl;
            std::cerr << "  Data Dir: " << conn.datadir << std::endl;
            std::cerr << "  NodeInfo: " << conn.nodeinfo_path << std::endl;
            std::cerr << "  Cookie: " << conn.cookie_path << std::endl;
            std::cerr << "  Discovery: " << conn.discovery_method << std::endl;
        }
        
        // Always show connection URL for transparency
        std::cerr << "Connected: " << conn.rpc_url 
                  << " (" << conn.discovery_method << ")" << std::endl;
    }
    
    static Connection createConnection(const std::string& rpc_url,
                                    const std::string& cookie_path,
                                    const std::string& nodeinfo_path,
                                    const std::string& discovery_method,
                                    const std::string& network,
                                    const std::string& datadir) {
        return {rpc_url, cookie_path, nodeinfo_path, discovery_method, network, datadir};
    }
};

// Enhanced error handling with proper exit codes
class ErrorHandler {
public:
    enum ExitCode {
        SUCCESS = 0,
        INTERNAL_ERROR = 1,    // Internal CLI error
        USAGE_ERROR = 2,       // Invalid arguments/usage
        CONNECT_ERROR = 3,     // Cannot connect to daemon
        AUTH_ERROR = 4,        // Authentication failed
        RPC_ERROR = 5,         // RPC method error
        NOT_FOUND_ERROR = 6,   // Resource not found
        TIMEOUT_ERROR = 7      // Operation timed out
    };
    
    static ExitCode mapRpcError(int rpc_code, const std::string& rpc_message) {
        // Map RPC error codes to CLI exit codes
        if (rpc_code == 401) return AUTH_ERROR;
        if (rpc_code == 404) return NOT_FOUND_ERROR;
        if (rpc_code == 408 || rpc_message.find("timeout") != std::string::npos) return TIMEOUT_ERROR;
        if (rpc_code >= 500) return CONNECT_ERROR;
        return RPC_ERROR;
    }
    
    static void printError(const std::string& message, ExitCode code, 
                          OutputContract::Format format = OutputContract::Format::PLAIN) {
        if (format == OutputContract::Format::JSON_STABLE || 
            format == OutputContract::Format::JSON_PRETTY) {
            Json::Json::Value error_output;
            error_output["cli_version"] = CLI_API_VERSION;
            error_output["success"] = false;
            error_output["error"] = message;
            error_output["exit_code"] = static_cast<int>(code);
            error_output["timestamp"] = static_cast<int64_t>(std::time(nullptr));
            
            OutputContract::printOutput(error_output, format);
        } else {
            std::cerr << "Error: " << message << std::endl;
        }
    }
};

} // namespace dinero::cli
