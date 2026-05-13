#if !DIN_ENABLE_LEGACY_RPC
#error "rpc_server.cpp was compiled with legacy disabled"
#endif

#include "daemon/rpc_server.h"
#include "daemon/config.h"
#include "common/logger.h"
#include "rpc/rpc_canonicalizer.h"
#include "rpc/address_validation.h"
#include "mining/mining_script_override.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mining_service.h"
#include "mining/address_validator.h"
#include "daemon/bech32_decode.h"
#include "wallet/wallet_manager.h"
#include "wallet/psbt.h"
#include "consensus/consensus.hpp"
#include "consensus/chain_identity.h"
#include "consensus/pow.hpp"
#include "consensus/chainparams.h"  // For Params().name to get network type
#include "primitives/uint256.h"
#include "primitives/block.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_db.h"
#include "policy/fee_estimator.h"  // Phase 3: CT fee estimation
#include <boost/beast/core/detail/base64.hpp>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <json/json.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include "compat/net_compat.h"
#include <sqlite3.h>

namespace dinero {

// ---- tiny helpers (no project macros needed) ----
static std::vector<uint8_t> decodeBase64ToBytes(const std::string& b64) {
  return din::from_base64(b64);
}

static std::string hexBE32(const std::vector<uint8_t>& raw32) {
  // Display txid in Bitcoin-style big-endian hex
  static const char* kHex = "0123456789abcdef";
  std::array<uint8_t,32> h{};
  std::copy(raw32.begin(), raw32.end(), h.begin());
  std::reverse(h.begin(), h.end());
  std::string s; s.resize(64);
  for (size_t i=0;i<32;i++) {
    s[2*i]   = kHex[(h[i] >> 4) & 0xF];
    s[2*i+1] = kHex[h[i] & 0xF];
  }
  return s;
}

static inline uint32_t ReadLE32u(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static Json::Value MiningProfileDisabledError() {
    Json::Value error;
    error["code"] = -32020;
    error["message"] = "Local mining disabled by sync profile: " + GetConfig().sync_profile;
    return error;
}

static StatusOr<Block> ReadLegacyRpcBlock(const uint256& hash, ChainDB* chain_db) {
    auto* daemon_ctx = DaemonContext::instance();
    if (daemon_ctx && daemon_ctx->chainstate) {
        auto chainstate_service =
            std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate);
        if (chainstate_service) {
            return chainstate_service->getBlockByHash(hash);
        }
    }

    if (chain_db) {
        return dinero::storage::ReadArchivalBlock(
            *chain_db,
            daemon_ctx ? daemon_ctx->block_storage.get() : nullptr,
            hash);
    }

    return Status::Internal;
}

RPCServer::RPCServer() : m_running(false), m_port(8332), 
                          m_blockchain(nullptr), m_mining(nullptr), m_mempool(nullptr),
                          m_wallet_balance_service(nullptr), m_wallet_manager(nullptr),
                          m_peer_manager(nullptr), m_headers_sync(nullptr),
                          m_wallet_unlocked(false), m_wallet_locked(true),
                          m_server_socket(-1) {
    
    g_logger.info("Initializing RPC server");
    
    // Initialize core method handlers
    initializeMethodHandlers();
}

RPCServer::~RPCServer() {
    g_logger.info("Shutting down RPC server");
    shutdown();
}

void RPCServer::initializeMethodHandlers() {
    // Core blockchain methods
    m_method_handlers["getblockchaininfo"] = rpc::adapt([this](const std::string& params) { 
        return getBlockchainInfo(); 
    });
    
    m_method_handlers["getblockcount"] = rpc::adapt([this](const std::string& params) { 
        return getBlockCount(); 
    });
    
    m_method_handlers["getbestblockhash"] = rpc::adapt([this](const std::string& params) { 
        return getBestBlockHash(); 
    });
    
    m_method_handlers["getblockhash"] = rpc::adapt([this](const std::string& params) { 
        return getBlockHash(params); 
    });
    
    m_method_handlers["getblock"] = rpc::adapt([this](const std::string& params) {
        return getBlock(params);
    });

    m_method_handlers["getblockheader"] = rpc::adapt([this](const std::string& params) {
        return getBlockHeader(params);
    });

    m_method_handlers["getrawtransaction"] = rpc::adapt([this](const std::string& params) {
        return getRawTransaction(params);
    });

    m_method_handlers["getchaintips"] = rpc::adapt([this](const std::string& params) {
        return getChainTips();
    });
    
    m_method_handlers["getpeers"] = rpc::adapt([this](const std::string& params) { 
        return getPeers(); 
    });
    
    // Mining methods
    m_method_handlers["getblocktemplate"] = rpc::adaptJson([this](const Json::Value& params) { 
        return getBlockTemplate(params); 
    });
    
    // Health check
    m_method_handlers["ping"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        (void)params;
        Json::Value result;
        result["status"] = "pong";
        return result;
    });
    
    // Basic info methods
    m_method_handlers["getinfo"] = rpc::adapt([this](const std::string& params) { 
        return getInfo(); 
    });
    
    // Network methods (using existing string-based implementation)
    m_method_handlers["getnetworkinfo"] = rpc::adapt([this](const std::string& params) {
        return getNetworkInfo();
    });
    
    // Mempool methods (using existing string-based implementation)
    m_method_handlers["getmempoolinfo"] = rpc::adapt([this](const std::string& params) {
        return getMempoolInfo();
    });
    
    // Wallet methods
    m_method_handlers["getnewaddress"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return getNewAddress(params);
    });
    
    m_method_handlers["getbalance"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return getBalance(params);
    });
    
    m_method_handlers["listunspent"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return listUnspent(params);
    });
    
    m_method_handlers["listtransactions"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return listTransactions(params);
    });
    
    // validateAddress method would be implemented here
    // This would validate bech32 addresses and check if they belong to the wallet
    // });
    
    // Mining methods
    m_method_handlers["mining.start"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return miningStart(params);
    });
    
    m_method_handlers["mining.stop"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return miningStop(params);
    });
    
    m_method_handlers["mining.status"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return miningStatus(params);
    });
    
    // Fee estimation
    m_method_handlers["estimatesmartfee"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return estimateSmartFee(params);
    });

    // ═══════════════════════════════════════════════════════════════════════════
    // CT Fee Configuration RPC (Phase 3: CT Fee Market Tuning)
    // ═══════════════════════════════════════════════════════════════════════════

    // ct.setminfee - Set minimum CT fee rate (sat/vB)
    m_method_handlers["ct.setminfee"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        if (!params.isArray() || params.empty()) {
            result["error"] = "Missing fee_rate parameter. Usage: ct.setminfee <fee_rate_una_vb>";
            return result;
        }

        double fee_rate = params[0].asDouble();
        if (fee_rate < 1.0 || fee_rate > 1000.0) {
            result["error"] = "Fee rate must be between 1 and 1000 una/vB";
            return result;
        }

        auto& config = m_mempool->mempool().GetCTConfig();
        config.ct_min_fee_rate = fee_rate;

        result["status"] = "updated";
        result["ct_min_fee_rate"] = fee_rate;
        g_logger.info("CT minimum fee rate set to " + std::to_string(fee_rate) + " sat/vB");
        return result;
    });

    // ct.setweightmultiplier - Set CT weight multiplier
    m_method_handlers["ct.setweightmultiplier"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        if (!params.isArray() || params.empty()) {
            result["error"] = "Missing multiplier parameter. Usage: ct.setweightmultiplier <multiplier>";
            return result;
        }

        double multiplier = params[0].asDouble();
        if (multiplier < 1.0 || multiplier > 10.0) {
            result["error"] = "Weight multiplier must be between 1.0 and 10.0";
            return result;
        }

        auto& config = m_mempool->mempool().GetCTConfig();
        config.ct_weight_multiplier = multiplier;

        result["status"] = "updated";
        result["ct_weight_multiplier"] = multiplier;
        g_logger.info("CT weight multiplier set to " + std::to_string(multiplier));
        return result;
    });

    // ct.setmaxperblock - Set max CT transactions per block
    m_method_handlers["ct.setmaxperblock"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        if (!params.isArray() || params.empty()) {
            result["error"] = "Missing max_count parameter. Usage: ct.setmaxperblock <max_count>";
            return result;
        }

        uint32_t max_count = params[0].asUInt();
        if (max_count < 1 || max_count > 1000) {
            result["error"] = "Max CT per block must be between 1 and 1000";
            return result;
        }

        auto& config = m_mempool->mempool().GetCTConfig();
        config.max_ct_per_block = max_count;

        result["status"] = "updated";
        result["max_ct_per_block"] = max_count;
        g_logger.info("Max CT transactions per block set to " + std::to_string(max_count));
        return result;
    });

    // ct.getfeeconfig - Get current CT fee configuration
    m_method_handlers["ct.getfeeconfig"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        const auto& config = m_mempool->mempool().GetCTConfig();

        result["ct_min_fee_rate"] = config.ct_min_fee_rate;
        result["ct_weight_multiplier"] = config.ct_weight_multiplier;
        result["ct_proof_weight_factor"] = config.ct_proof_weight_factor;
        result["max_ct_per_block"] = config.max_ct_per_block;
        result["prefer_transparent_below_fee"] = config.prefer_transparent_below_fee;

        // Add computed values for user convenience
        result["info"]["description"] = "CT (Confidential Transaction) fee policy parameters";
        result["info"]["ct_min_fee_rate_desc"] = "Minimum fee rate for CT transactions (sat/vB)";
        result["info"]["ct_weight_multiplier_desc"] = "Weight multiplier for CT verification overhead";
        result["info"]["effective_min_fee"] = config.ct_min_fee_rate * config.ct_weight_multiplier;

        return result;
    });

    // ct.setproofweightfactor - Set CT proof weight factor
    m_method_handlers["ct.setproofweightfactor"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        if (!params.isArray() || params.empty()) {
            result["error"] = "Missing factor parameter. Usage: ct.setproofweightfactor <factor>";
            return result;
        }

        uint32_t factor = params[0].asUInt();
        if (factor < 1 || factor > 16) {
            result["error"] = "Proof weight factor must be between 1 and 16";
            return result;
        }

        auto& config = m_mempool->mempool().GetCTConfig();
        config.ct_proof_weight_factor = factor;

        result["status"] = "updated";
        result["ct_proof_weight_factor"] = factor;
        g_logger.info("CT proof weight factor set to " + std::to_string(factor));
        return result;
    });

    // ct.estimatefee - Estimate fee for CT transaction
    m_method_handlers["ct.estimatefee"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        Json::Value result;

        if (!m_mempool) {
            result["error"] = "Mempool service not available";
            return result;
        }

        // Parse parameters: [proof_bytes, target_blocks]
        size_t proof_bytes = 5000;  // Default: ~7 outputs
        uint32_t target_blocks = 6;  // Default: normal priority

        if (params.isArray() && params.size() > 0) {
            proof_bytes = params[0].asUInt64();
        }
        if (params.isArray() && params.size() > 1) {
            target_blocks = params[1].asUInt();
        }

        // Map target blocks to FeeTarget
        policy::FeeTarget target = policy::FeeTarget::NORMAL;
        if (target_blocks <= 1) {
            target = policy::FeeTarget::IMMEDIATE;
        } else if (target_blocks <= 3) {
            target = policy::FeeTarget::FAST;
        } else if (target_blocks <= 6) {
            target = policy::FeeTarget::NORMAL;
        } else if (target_blocks <= 12) {
            target = policy::FeeTarget::SLOW;
        } else {
            target = policy::FeeTarget::ECONOMY;
        }

        // Get CT fee estimate from mempool's fee estimator
        auto& fee_estimator = m_mempool->mempool().getFeeEstimator();
        auto ct_estimate = fee_estimator.estimateCTFee(target, proof_bytes);

        // Build result
        result["base_fee_rate_sat_kb"] = static_cast<Json::UInt64>(ct_estimate.base_fee_rate);
        result["ct_adjusted_rate_sat_kb"] = static_cast<Json::UInt64>(ct_estimate.ct_adjusted_rate);
        result["ct_multiplier"] = ct_estimate.ct_multiplier;
        result["ct_proof_weight_factor"] = ct_estimate.ct_proof_weight;
        result["estimated_proof_bytes"] = static_cast<Json::UInt64>(ct_estimate.estimated_proof_bytes);
        result["confidence"] = ct_estimate.confidence;
        result["is_sufficient_data"] = ct_estimate.is_sufficient_data;
        result["target_blocks"] = target_blocks;

        // Convenience: convert to una/vB for user display
        result["base_fee_rate_una_vb"] = ct_estimate.base_fee_rate / 1000.0;
        result["ct_adjusted_rate_una_vb"] = ct_estimate.ct_adjusted_rate / 1000.0;

        // Example fee calculation for 250-byte tx + proof
        uint64_t example_fee = ct_estimate.estimateFeeForSize(250, proof_bytes);
        result["example_fee_for_250b_tx"] = static_cast<Json::UInt64>(example_fee);

        return result;
    });

    // === PSBT workflow (LEGACY DISABLED - Use vNext RPC) ===
    // Legacy PSBT handlers removed - use vNext RPC endpoints instead
    // All PSBT operations now go through g_rpcRegistry in main.cpp
    
    // === Realtime events ===
    m_method_handlers["events.subscribe"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return rpcEventsSubscribe(params);
    });
    m_method_handlers["events.unsubscribe"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return rpcEventsUnsubscribe(params);
    });

    // === Mining block generation ===
    m_method_handlers["generatetoaddress"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return generateToAddress(params);
    });

    // === Wallet transaction methods ===
    m_method_handlers["listtransactions"] = rpc::adaptJson([this](const Json::Value& params) -> Json::Value {
        return listTransactions(params);
    });
}

bool RPCServer::initialize(int port) {
    m_port = port;
    
    g_logger.info("RPC server initializing on port " + std::to_string(m_port));
    
    m_running.store(true);
    
    // Create server socket
    m_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_socket < 0) {
        g_logger.error("Failed to create server socket");
        return false;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    
    // Bind socket to localhost
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    server_addr.sin_port = htons(m_port);
    
    if (bind(m_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        g_logger.error("Failed to bind server socket to port " + std::to_string(m_port));
        COMPAT_CLOSE_SOCKET(m_server_socket);
        return false;
    }
    
    // Listen for connections
    if (listen(m_server_socket, 100) < 0) {
        g_logger.error("Failed to listen on server socket");
        COMPAT_CLOSE_SOCKET(m_server_socket);
        return false;
    }
    
    g_logger.info("RPC listening on 127.0.0.1:" + std::to_string(m_port));
    
    // Start RPC server thread
    m_rpc_thread = std::thread(&RPCServer::run, this);
    
    g_logger.info("RPC server started on port " + std::to_string(m_port));
    
    return true;
}

void RPCServer::shutdown() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) {
        return; // Already shut down
    }
    
    g_logger.info("RPC server shutdown started");
    
    // Close server socket
    if (m_server_socket >= 0) {
        COMPAT_CLOSE_SOCKET(m_server_socket);
        m_server_socket = -1;
    }
    
    // Join RPC thread
    if (m_rpc_thread.joinable()) {
        m_rpc_thread.join();
    }
    
    g_logger.info("RPC server shutdown complete");
}

void RPCServer::run() {
    g_logger.info("RPC server thread started");
    
    while (m_running.load()) {
        // Accept client connections
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(m_server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (m_running.load()) {
                g_logger.error("Failed to accept client connection");
            }
            continue;
        }
        
        // Handle client in separate thread
        std::thread([this, client_socket, client_addr]() {
            try {
                handleClient(client_socket, client_addr);
            } catch (const std::exception& e) {
                g_logger.error("[RPCServer] Exception in client handler: " + std::string(e.what()));
            } catch (...) {
                g_logger.error("[RPCServer] Unknown exception in client handler");
            }
        }).detach();
    }
    
    g_logger.info("RPC server thread stopped");
}

void RPCServer::handleClient(int client_socket, struct sockaddr_in client_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    g_logger.debug("New client connected from " + std::string(client_ip));
    
    // Read HTTP request
    char buffer[4096];
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        COMPAT_CLOSE_SOCKET(client_socket);
        return;
    }
    
    buffer[bytes_read] = '\0';
    std::string request(buffer);
    
    std::string response = parseHTTPRequest(request);
    send(client_socket, response.c_str(), response.length(), 0);
    
    COMPAT_CLOSE_SOCKET(client_socket);
    g_logger.debug("Client disconnected: " + std::string(client_ip));
}

std::string RPCServer::parseHTTPRequest(const std::string& request) {
    // Parse HTTP request to extract JSON-RPC content
    size_t body_start = request.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return createHTTPResponse("Invalid HTTP request", 400);
    }
    
    std::string headers = request.substr(0, body_start);
    std::string body = request.substr(body_start + 4);
    
    // Check if it's a POST request
    if (headers.find("POST") == 0) {
        std::string json_response = handleJSONRPC(body);
        return createHTTPResponse(json_response, 200);
    } else if (headers.find("GET") == 0) {
        // Handle GET requests (status)
        Json::Value response;
        response["status"] = "running";
        response["port"] = m_port;
        response["version"] = "1.0.0";
        return createHTTPResponse(Json::StyledWriter().write(response), 200);
    } else {
        return createHTTPResponse("Method not allowed", 405);
    }
}

std::string RPCServer::createHTTPResponse(const std::string& body, int status_code) {
    std::stringstream response;
    response << "HTTP/1.1 " << status_code << " ";
    
    switch (status_code) {
        case 200: response << "OK"; break;
        case 400: response << "Bad Request"; break;
        case 405: response << "Method Not Allowed"; break;
        case 500: response << "Internal Server Error"; break;
        default: response << "Unknown"; break;
    }
    
    response << "\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    
    return response.str();
}

std::string RPCServer::handleJSONRPC(const std::string& json_request) {
    try {
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value request_json;
        std::string errors;
        bool success = reader->parse(json_request.c_str(), json_request.c_str() + json_request.length(), &request_json, &errors);
        delete reader;
        
        if (!success) {
            Json::Value error_response;
            error_response["jsonrpc"] = "2.0";
            error_response["error"]["code"] = -32700;
            error_response["error"]["message"] = "Parse error";
            error_response["id"] = Json::Value::null;
            // ✅ Enforce schema contract on parse errors too
            error_response["rpc_schema"] = "din.rpc.v1";
            error_response["schema_rev"] = 1;
            return Json::StyledWriter().write(error_response);
        }
        
        // Handle single request
        Json::Value response = handleSingleRequest(request_json);
        return Json::StyledWriter().write(response);
        
    } catch (const std::exception& e) {
        Json::Value error_response;
        error_response["jsonrpc"] = "2.0";
        error_response["error"]["code"] = -32603;
        error_response["error"]["message"] = "Internal error: " + std::string(e.what());
        error_response["id"] = Json::Value::null;
        // ✅ Enforce schema contract on top-level exceptions too
        error_response["rpc_schema"] = "din.rpc.v1";
        error_response["schema_rev"] = 1;
        return Json::StyledWriter().write(error_response);
    }
}

Json::Value RPCServer::handleSingleRequest(const Json::Value& request) {
    Json::Value response;
    Json::Value error;
    
    try {
        // Validate request format
        if (!request.isObject()) {
            error["code"] = -32600;
            error["message"] = "Invalid Request";
            response["jsonrpc"] = "2.0";
            response["error"] = error;
            response["id"] = Json::Value::null;
            // ✅ Enforce schema contract on validation errors too
            response["rpc_schema"] = "din.rpc.v1";
            response["schema_rev"] = 1;
            return response;
        }
        
        // Extract request components
        std::string method = request.get("method", "").asString();
        Json::Value params = request.get("params", Json::Value::null);
        Json::Value id = request.get("id", Json::Value::null);
        
        // Build response
        response["jsonrpc"] = "2.0";
        response["id"] = id;
        
        // Execute method
        auto it = m_method_handlers.find(method);
        if (it != m_method_handlers.end()) {
            // Convert Json::Value params to string for handler
            Json::StreamWriterBuilder builder;
            std::string params_str = Json::writeString(builder, params);
            std::string result_str = it->second(params_str);
            
            // Parse result string back to Json::Value
            Json::CharReaderBuilder reader_builder;
            Json::CharReader* reader = reader_builder.newCharReader();
            Json::Value result;
            std::string parse_errors;
            bool success = reader->parse(result_str.c_str(), result_str.c_str() + result_str.length(), &result, &parse_errors);
            delete reader;
            
            if (success && result.isObject() && result.isMember("error")) {
                response["error"] = result["error"];
            } else if (success) {
                response["result"] = result;
            } else {
                error["code"] = -32603;
                error["message"] = "Internal error: Failed to parse result";
                response["error"] = error;
            }
        } else {
            error["code"] = -32601;
            error["message"] = "Method not found: " + method;
            response["error"] = error;
        }
        
        // ✅ Enforce schema contract on ALL replies (success and error)
        response["rpc_schema"] = "din.rpc.v1";
        response["schema_rev"] = 1;
        
        return response;
        
    } catch (const std::exception& e) {
        error["code"] = -32603;
        error["message"] = "Internal error: " + std::string(e.what());
        response["jsonrpc"] = "2.0";
        response["error"] = error;
        response["id"] = request.get("id", Json::Value::null);
        // ✅ Enforce schema contract on exception paths too
        response["rpc_schema"] = "din.rpc.v1";
        response["schema_rev"] = 1;
        return response;
    }
}

// Core RPC method implementations

std::string RPCServer::getBlockchainInfo() {
    Json::Value result;

    // Get chain name from Params() (mainnet, testnet, or regtest)
    result["chain"] = dinero::Params().name;

    // Get actual blockchain state from ChainDB
    if (execution_context_.hasChainDB()) {
        auto tip_result = execution_context_.chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result["blocks"] = static_cast<Json::UInt64>(tip_result.value().height);
            result["headers"] = static_cast<Json::UInt64>(tip_result.value().height);
            result["bestblockhash"] = tip_result.value().hash.GetHex();
        } else {
            result["blocks"] = 0;
            result["headers"] = 0;
            result["bestblockhash"] = "0000000000000000000000000000000000000000000000000000000000000000";
        }
    } else {
        result["blocks"] = 0;
        result["headers"] = 0;
        result["bestblockhash"] = "0000000000000000000000000000000000000000000000000000000000000000";
    }

    result["difficulty"] = 1.0;
    result["mediantime"] = 0;
    result["verificationprogress"] = 1.0;
    result["initialblockdownload"] = false;
    result["chainwork"] = "0000000000000000000000000000000000000000000000000000000000000000";

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getBlockCount() {
    Json::Value result;

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (execution_context_.hasChainDB()) {
        auto tip_result = execution_context_.chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result = static_cast<Json::UInt64>(tip_result.value().height);
        } else {
            result = static_cast<Json::UInt64>(0);
        }
    } else {
        result = static_cast<Json::UInt64>(0);
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getBestBlockHash() {
    Json::Value result;

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (execution_context_.hasChainDB()) {
        auto tip_result = execution_context_.chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result = tip_result.value().hash;
        } else {
            result = "0000000000000000000000000000000000000000000000000000000000000000";
        }
    } else {
        result = "0000000000000000000000000000000000000000000000000000000000000000";
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getBlockHash(const std::string& params) {
    Json::Value result;

    // Parse height from params string
    int height = 0;
    try {
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value parsed_params;
        std::string errors;
        bool success = reader->parse(params.c_str(), params.c_str() + params.length(), &parsed_params, &errors);
        delete reader;

        if (success && parsed_params.isArray() && parsed_params.size() > 0) {
            height = parsed_params[0].asInt();
        } else {
            return rpc::createErrorResponseStr(-1, "Invalid parameters: expected height");
        }
    } catch (const std::exception& e) {
        return rpc::createErrorResponseStr(-1, std::string("Parse error: ") + e.what());
    }

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (execution_context_.hasChainDB()) {
        auto hash_result = execution_context_.chain_db->getBlockHashByHeight(height);
        if (hash_result.status() == dinero::Status::Ok) {
            result = hash_result.value();
        } else {
            return rpc::createErrorResponseStr(-5, "Block height out of range");
        }
    } else {
        return rpc::createErrorResponseStr(-1, "ChainDB not available");
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getBlock(const std::string& params) {
    Json::Value result;

    // Parse hash from params string
    std::string hash;
    int verbosity = 1; // default: full block with tx details
    try {
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value parsed_params;
        std::string errors;
        bool success = reader->parse(params.c_str(), params.c_str() + params.length(), &parsed_params, &errors);
        delete reader;

        if (success && parsed_params.isArray() && parsed_params.size() > 0) {
            hash = parsed_params[0].asString();
            if (parsed_params.size() > 1 && parsed_params[1].isInt()) {
                verbosity = parsed_params[1].asInt();
            }
        } else {
            return rpc::createErrorResponseStr(-1, "Invalid parameters: expected hash");
        }
    } catch (const std::exception& e) {
        return rpc::createErrorResponseStr(-1, std::string("Parse error: ") + e.what());
    }

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (!execution_context_.hasChainDB()) {
        return rpc::createErrorResponseStr(-1, "ChainDB not available");
    }

    // Get block from ChainDB
    dinero::uint256 block_hash(hash);
    auto block_result = ReadLegacyRpcBlock(block_hash, execution_context_.chain_db);
    if (block_result.status() != dinero::Status::Ok) {
        return rpc::createErrorResponseStr(-5, "Block not found");
    }

    const auto& block = block_result.value();

    // Get block height
    auto height_result = execution_context_.chain_db->getBlockHeight(block_hash);
    int block_height = height_result.status() == dinero::Status::Ok ? height_result.value() : -1;

    // Get chain tip for confirmations
    auto tip_result = execution_context_.chain_db->getTip();
    int confirmations = 1;
    if (tip_result.status() == dinero::Status::Ok && block_height >= 0) {
        confirmations = tip_result.value().height - block_height + 1;
    }

    if (verbosity == 0) {
        // Verbosity 0: Return hex-encoded block data
        result = block.Serialize();
    } else {
        // Verbosity 1+: Return JSON object
        result["hash"] = hash;
        result["confirmations"] = confirmations;
        result["height"] = block_height;
        result["version"] = block.header.version;
        result["merkleroot"] = block.header.merkle_root;
        result["time"] = static_cast<Json::UInt64>(block.header.timestamp);
        result["mediantime"] = static_cast<Json::UInt64>(block.header.timestamp);
        result["nonce"] = block.header.nonce;
        result["bits"] = block.header.difficulty;
        result["difficulty"] = static_cast<double>(block.header.difficulty);
        result["previousblockhash"] = block.header.prev_block_hash.empty() ? block.header.prev_block_hash : block.header.prev_block_hash;
        result["utreexocommitment"] = block.header.utreexo_root;

        // Transaction list
        Json::Value tx_array(Json::arrayValue);
        for (const auto& tx : block.vtx) {
            tx_array.append(tx.GetTxid());
        }
        result["tx"] = tx_array;
        result["size"] = static_cast<Json::UInt64>(block.Serialize().length());
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getBlockHeader(const std::string& params) {
    Json::Value result;

    // Parse hash from params string
    std::string hash;
    bool verbose = true; // default: JSON object
    try {
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value parsed_params;
        std::string errors;
        bool success = reader->parse(params.c_str(), params.c_str() + params.length(), &parsed_params, &errors);
        delete reader;

        if (success && parsed_params.isArray() && parsed_params.size() > 0) {
            hash = parsed_params[0].asString();
            if (parsed_params.size() > 1 && parsed_params[1].isBool()) {
                verbose = parsed_params[1].asBool();
            }
        } else {
            return rpc::createErrorResponseStr(-1, "Invalid parameters: expected hash");
        }
    } catch (const std::exception& e) {
        return rpc::createErrorResponseStr(-1, std::string("Parse error: ") + e.what());
    }

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (!execution_context_.hasChainDB()) {
        return rpc::createErrorResponseStr(-1, "ChainDB not available");
    }

    // Get header from ChainDB
    dinero::uint256 block_hash(hash);
    auto header_result = execution_context_.chain_db->getHeader(block_hash);
    if (header_result.status() != dinero::Status::Ok) {
        return rpc::createErrorResponseStr(-5, "Block not found");
    }

    const auto& header = header_result.value();

    // Get block height
    auto height_result = execution_context_.chain_db->getBlockHeight(block_hash);
    int block_height = height_result.status() == dinero::Status::Ok ? height_result.value() : -1;

    // Get chain tip for confirmations
    auto tip_result = execution_context_.chain_db->getTip();
    int confirmations = 1;
    if (tip_result.status() == dinero::Status::Ok && block_height >= 0) {
        confirmations = tip_result.value().height - block_height + 1;
    }

    if (!verbose) {
        // Return hex-encoded header
        auto header_bytes = header.SerializeForHash();
        std::string hex_header;
        for (auto byte : header_bytes) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", byte);
            hex_header += buf;
        }
        result = hex_header;
    } else {
        // Return JSON object
        result["hash"] = hash;
        result["confirmations"] = confirmations;
        result["height"] = block_height;
        result["version"] = header.version;
        result["merkleroot"] = header.merkle_root;
        result["time"] = static_cast<Json::UInt64>(header.timestamp);
        result["mediantime"] = static_cast<Json::UInt64>(header.timestamp);
        result["nonce"] = header.nonce;
        result["bits"] = header.difficulty;
        result["difficulty"] = static_cast<double>(header.difficulty);
        result["previousblockhash"] = header.prev_block_hash.empty() ? header.prev_block_hash : header.prev_block_hash;
        result["utreexocommitment"] = header.utreexo_root;
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getRawTransaction(const std::string& params) {
    Json::Value result;

    // Parse txid from params string
    std::string txid;
    bool verbose = false; // default: hex string
    try {
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value parsed_params;
        std::string errors;
        bool success = reader->parse(params.c_str(), params.c_str() + params.length(), &parsed_params, &errors);
        delete reader;

        if (success && parsed_params.isArray() && parsed_params.size() > 0) {
            txid = parsed_params[0].asString();
            if (parsed_params.size() > 1) {
                if (parsed_params[1].isBool()) {
                    verbose = parsed_params[1].asBool();
                } else if (parsed_params[1].isInt()) {
                    verbose = (parsed_params[1].asInt() != 0);
                }
            }
        } else {
            return rpc::createErrorResponseStr(-1, "Invalid parameters: expected txid");
        }
    } catch (const std::exception& e) {
        return rpc::createErrorResponseStr(-1, std::string("Parse error: ") + e.what());
    }

    // Phase 4B: Wire to ChainDB via ExecutionContext
    if (!execution_context_.hasChainDB()) {
        return rpc::createErrorResponseStr(-1, "ChainDB not available");
    }

    // Get transaction location from TX index
    dinero::uint256 txid_uint256(txid);
    auto tx_location_result = execution_context_.chain_db->getTxLocation(txid_uint256);
    if (tx_location_result.status() != dinero::Status::Ok) {
        return rpc::createErrorResponseStr(-5, "Transaction not found");
    }

    const auto& [block_hash, tx_offset] = tx_location_result.value();

    // Get block to extract transaction
    auto block_result = ReadLegacyRpcBlock(block_hash, execution_context_.chain_db);
    if (block_result.status() != dinero::Status::Ok) {
        return rpc::createErrorResponseStr(-5, "Block not found");
    }

    const auto& block = block_result.value();

    // Find transaction in block
    dinero::Transaction found_tx;
    bool tx_found = false;
    for (const auto& tx : block.vtx) {
        if (tx.GetTxid() == txid) {
            found_tx = tx;
            tx_found = true;
            break;
        }
    }

    if (!tx_found) {
        return rpc::createErrorResponseStr(-5, "Transaction not found in block");
    }

    // Get block height for confirmations
    auto height_result = execution_context_.chain_db->getBlockHeight(block_hash);
    int block_height = height_result.status() == dinero::Status::Ok ? height_result.value() : -1;

    auto tip_result = execution_context_.chain_db->getTip();
    int confirmations = 1;
    if (tip_result.status() == dinero::Status::Ok && block_height >= 0) {
        confirmations = tip_result.value().height - block_height + 1;
    }

    if (!verbose) {
        // Return hex-encoded transaction
        result = found_tx.Serialize();
    } else {
        // Return JSON object
        result["txid"] = txid;
        result["hash"] = txid;
        result["version"] = found_tx.version;
        result["size"] = static_cast<Json::UInt64>(found_tx.Serialize().length());
        result["locktime"] = found_tx.locktime;
        result["blockhash"] = block_hash;
        result["confirmations"] = confirmations;
        result["time"] = static_cast<Json::UInt64>(block.header.timestamp);
        result["blocktime"] = static_cast<Json::UInt64>(block.header.timestamp);

        // Inputs
        Json::Value vin(Json::arrayValue);
        for (const auto& input : found_tx.inputs) {
            Json::Value in;
            in["txid"] = input.prevTxId;
            in["vout"] = input.prevOutIndex;
            in["scriptSig"] = input.scriptSig;
            in["sequence"] = input.sequence;
            vin.append(in);
        }
        result["vin"] = vin;

        // Outputs
        Json::Value vout(Json::arrayValue);
        for (size_t i = 0; i < found_tx.outputs.size(); i++) {
            const auto& output = found_tx.outputs[i];
            Json::Value out;
            out["value"] = static_cast<Json::UInt64>(output.amount);
            out["n"] = static_cast<Json::UInt>(i);
            Json::Value scriptPubKey;
            scriptPubKey["hex"] = output.scriptPubKey;
            out["scriptPubKey"] = scriptPubKey;
            vout.append(out);
        }
        result["vout"] = vout;
    }

    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getChainTips() {
    Json::Value result(Json::arrayValue);
    
    // For now, return the main chain tip
    if (m_blockchain) {
        Json::Value tip;
        tip["height"] = m_blockchain ? 0 : 0;
        tip["hash"] = m_blockchain ? "0000000000000000000000000000000000000000000000000000000000000000" : "0000000000000000000000000000000000000000000000000000000000000000";
        tip["status"] = "active";
        tip["branchlen"] = 0;
        
        result.append(tip);
    }
    
    return rpc::createSuccessResponseStr(result);
}

std::string RPCServer::getPeers() {
    Json::Value result(Json::arrayValue);
    
    // Return empty array for now - peer management will be implemented later
    
    return rpc::createSuccessResponseStr(result);
}

Json::Value RPCServer::getBlockTemplate(const Json::Value& params) {
    Json::Value result;

    g_logger.info("⭐⭐⭐ getBlockTemplate CALLED in rpc_server.cpp ⭐⭐⭐");

    if (!m_blockchain) {
        return createErrorResponse(-32603, "Blockchain not available");
    }

    if (!m_mining) {
        return createErrorResponse(-32603, "Mining not available");
    }

    try {
        const Consensus consensus = GetConsensusForCurrentNetwork();

        // Get current blockchain state
        uint32_t current_height = m_blockchain->getLatestHeight();
        std::string prev_hash = m_blockchain->getLatestHash();
        uint32_t next_height = current_height + 1;

        int64_t current_time = static_cast<int64_t>(std::time(nullptr));
        uint32_t next_bits = GetNextWorkRequiredWithChainDB(
            static_cast<int32_t>(next_height),
            current_time,
            consensus,
            execution_context_.chain_db
        );

        // Format bits as 8-char hex string (BIP22 standard - no 0x prefix)
        char bits_hex[9];
        std::snprintf(bits_hex, sizeof(bits_hex), "%08x", next_bits);

        // DEBUG logging
        const char* phase = "ASERT";  // ASERT from block 1, no bootstrap phase
        g_logger.info("getBlockTemplate: H=" + std::to_string(next_height) +
                     " nextBits=0x" + std::string(bits_hex) +
                     " phase=" + std::string(phase));

        // Create block template
        result["version"] = 1;
        result["previousblockhash"] = prev_hash;
        result["height"] = next_height;
        result["bits"] = std::string(bits_hex);  // 8-char hex string, no 0x prefix
        result["curtime"] = static_cast<Json::UInt64>(current_time);
        result["mintime"] = static_cast<Json::UInt64>(current_time - 7200);
        result["maxtime"] = static_cast<Json::UInt64>(current_time + 7200);

        // Longpoll ID (use previous block hash)
        result["longpollid"] = prev_hash;

        // Mutable fields
        Json::Value mutable_fields(Json::arrayValue);
        mutable_fields.append("time");
        mutable_fields.append("transactions");
        mutable_fields.append("prevblock");
        result["mutable"] = mutable_fields;

        // Add transactions (empty for now - would get from mempool)
        result["transactions"] = Json::Value(Json::arrayValue);

    } catch (const std::exception& e) {
        g_logger.error("getBlockTemplate error: " + std::string(e.what()));
        return createErrorResponse(-32603, std::string("Error creating block template: ") + e.what());
    }

    return result;
}

std::string RPCServer::getInfo() {
    Json::Value result;

    // Basic node information
    result["version"] = 100000; // Version 1.0.0
    result["subversion"] = "/Dinero:1.0.0/";
    result["protocolversion"] = 70015;

    // Network information - get from Params()
    const std::string& network = dinero::Params().name;
    result["network"] = network;
    result["testnet"] = (network == "testnet");
    result["regtest"] = (network == "regtest");

    // Blockchain information
    if (execution_context_.hasChainDB()) {
        auto tip_result = execution_context_.chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result["blocks"] = static_cast<Json::UInt64>(tip_result.value().height);
        } else {
            result["blocks"] = 0;
        }
        result["timeoffset"] = 0;
        result["connections"] = 0;
    } else if (m_blockchain) {
        result["blocks"] = 0; // Safe default without calling incomplete type methods
        result["timeoffset"] = 0;
        result["connections"] = 0;
    } else {
        result["blocks"] = 0;
        result["timeoffset"] = 0;
        result["connections"] = 0;
    }
    
    // Wallet information
    result["walletversion"] = 60000;
    result["balance"] = 0.0;
    result["keypoololdest"] = 0;
    result["keypoolsize"] = 100;
    result["unlocked_until"] = 0;
    
    // Mining information
    if (m_mining) {
        result["difficulty"] = 1.0;
        result["generate"] = false; // Safe default without calling incomplete type methods
        result["genproclimit"] = 1;
        result["hashespersec"] = 0;
    } else {
        result["difficulty"] = 1.0;
        result["generate"] = false;
        result["genproclimit"] = 1;
        result["hashespersec"] = 0;
    }
    
    result["paytxfee"] = 0.0;
    result["relayfee"] = 0.00001;
    result["errors"] = "";

    // Dinero motto (permanent identity and branding)
    result["motto"] = std::string(dinero::consensus::kGenesisMotto);

    return rpc::createSuccessResponseStr(result);
}

Json::Value RPCServer::createErrorResponse(int code, const std::string& message) {
    Json::Value error;
    error["code"] = code;
    error["message"] = message;
    return error;
}

// Component setters are defined inline in header

// All setter methods are defined inline in header file

std::string RPCServer::handleRequest(const std::string& request) {
    // Handle single parameter request (HTTP request body)
    return parseHTTPRequest(request);
}

std::string RPCServer::handleRequest(const std::string& request, const std::string& path) {
    // Handle HTTP request with path
    return parseHTTPRequest(request);
}

// Missing core RPC method implementations

std::string RPCServer::getNetworkInfo() {
    Json::Value result;
    
    result["version"] = 100000;
    result["subversion"] = "/Dinero:1.0.0/";
    result["protocolversion"] = 70015;
    // Get actual peer count from peer manager
    int peer_count = 0;
    if (m_peer_manager) {
        // Assuming peer manager has a method to get connection count
        // For now, return 0 for regtest mode which is expected
        peer_count = 0; // Regtest typically has no peers
    }
    result["connections"] = peer_count;
    result["relayfee"] = 1000; // 1000 una/kvB minimum relay fee
    
    // Network interfaces
    Json::Value networks(Json::arrayValue);
    Json::Value ipv4;
    ipv4["name"] = "ipv4";
    ipv4["limited"] = false;
    ipv4["reachable"] = true;
    ipv4["proxy"] = "";
    ipv4["proxy_randomize_credentials"] = false;
    networks.append(ipv4);
    result["networks"] = networks;
    
    result["localaddresses"] = Json::Value(Json::arrayValue);
    
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, result);
}

std::string RPCServer::getMempoolInfo() {
    Json::Value result;

    if (m_mempool) {
        result["loaded"] = true;

        // STEP 3.1: Query actual mempool statistics (not hardcoded zeros)
        size_t mempool_size = m_mempool->mempool().size();
        size_t mempool_bytes = m_mempool->mempool().getTotalSize();
        uint64_t total_fees = m_mempool->mempool().getTotalFees();

        // Memory usage estimate (entries + transaction data)
        size_t memory_usage = mempool_bytes;

        result["size"] = static_cast<int>(mempool_size);
        result["bytes"] = static_cast<int>(mempool_bytes);
        result["usage"] = static_cast<int>(memory_usage);
        result["maxmempool"] = 300000000; // 300MB default
        result["mempoolminfee"] = 1000; // 1000 una/kvB
        result["minrelaytxfee"] = 1000; // 1000 una/kvB
        result["unbroadcastcount"] = 0;
        result["total_fee"] = static_cast<double>(total_fees) / 1e8; // DIN
    } else {
        result["loaded"] = false;
        result["size"] = 0;
        result["bytes"] = 0;
        result["usage"] = 0;
        result["maxmempool"] = 0;
        result["mempoolminfee"] = 1000;
        result["minrelaytxfee"] = 1000;
        result["unbroadcastcount"] = 0;
    }

    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, result);
}

Json::Value RPCServer::getNewAddress(const Json::Value& params) {
    try {
        if (!m_wallet_manager) {
            Json::Value error;
            error["code"] = -32601;
            error["message"] = "Wallet not available";
            return error;
        }
        
        // Extract label from parameters if provided
        std::string label = "";
        if (params.isObject() && params.isMember("label")) {
            label = params["label"].asString();
        } else if (params.isArray() && params.size() > 0) {
            label = params[0].asString();
        }
        
        // Generate real address from wallet
        std::string address = m_wallet_manager->getNewAddress(label);
        if (address.empty()) {
            Json::Value error;
            error["code"] = -32603;
            error["message"] = "Failed to generate new address";
            return error;
        }
        
        return Json::Value(address);
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Address generation failed: ") + e.what();
        return error;
    }
}

Json::Value RPCServer::getBalance(const Json::Value& params) {
    Json::Value result;
    
    if (!m_wallet_manager) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet not available";
        return error;
    }
    
    // Get actual balance from wallet database
    sqlite3* db = m_wallet_manager->getCurrentDatabase();
    if (!db) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet database not available";
        return error;
    }
    
    // Query UTXOs to calculate balance
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT SUM(value) FROM utxos WHERE spent = 0";
    
    uint64_t total_balance = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total_balance = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // Convert to DIN (assuming 1 DIN = 100,000,000 una)
    double balance_din = static_cast<double>(total_balance) / 100000000.0;
    
    result["balance_una"] = static_cast<int64_t>(total_balance);
    result["balance_din"] = std::to_string(balance_din);
    // Calculate unconfirmed and immature balances
    uint64_t unconfirmed_balance = 0;
    uint64_t immature_balance = 0;
    
    // Query for unconfirmed transactions (0 confirmations)
    const char* unconfirmed_sql = "SELECT SUM(value) FROM utxos WHERE spent = 0 AND confirmations = 0";
    if (sqlite3_prepare_v2(db, unconfirmed_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            unconfirmed_balance = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // Query for immature coinbase transactions (< 100 confirmations)
    const char* immature_sql = "SELECT SUM(value) FROM utxos WHERE spent = 0 AND is_coinbase = 1 AND confirmations < 100";
    if (sqlite3_prepare_v2(db, immature_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            immature_balance = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    double unconfirmed_din = static_cast<double>(unconfirmed_balance) / 100000000.0;
    double immature_din = static_cast<double>(immature_balance) / 100000000.0;
    
    result["unconfirmed_una"] = static_cast<int64_t>(unconfirmed_balance);
    result["unconfirmed_din"] = std::to_string(unconfirmed_din);
    result["immature_una"] = static_cast<int64_t>(immature_balance);
    result["immature_din"] = std::to_string(immature_din);
    
    return result;
}

Json::Value RPCServer::listUnspent(const Json::Value& params) {
    Json::Value result(Json::arrayValue);
    
    if (!m_wallet_manager) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet not available";
        return error;
    }
    
    // Get actual UTXOs from wallet database
    sqlite3* db = m_wallet_manager->getCurrentDatabase();
    if (!db) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet database not available";
        return error;
    }
    
    // Query UTXOs
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT txid, vout, value, script_pubkey, address FROM utxos WHERE spent = 0";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Json::Value utxo;
            utxo["txid"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            utxo["vout"] = sqlite3_column_int(stmt, 1);
            utxo["value"] = sqlite3_column_int64(stmt, 2);
            utxo["scriptPubKey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            utxo["address"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            // Calculate actual confirmations based on blockchain height
            int confirmations = 1; // Default for unconfirmed
            if (m_blockchain) {
                uint32_t current_height = m_blockchain->getBlockHeight();
                uint32_t utxo_height = sqlite3_column_int(stmt, 5); // Assuming height is stored
                if (current_height >= utxo_height) {
                    confirmations = current_height - utxo_height + 1;
                }
            }
            utxo["confirmations"] = confirmations;
            utxo["spendable"] = true;
            utxo["solvable"] = true;
            
            result.append(utxo);
        }
        sqlite3_finalize(stmt);
    }
    
    return result;
}

Json::Value RPCServer::listTransactions(const Json::Value& params) {
    Json::Value result(Json::arrayValue);
    
    if (!m_wallet_manager) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet not available";
        return error;
    }
    
    // Get actual transaction history from wallet database
    sqlite3* db = m_wallet_manager->getCurrentDatabase();
    if (!db) {
        Json::Value error;
        error["code"] = -32601;
        error["message"] = "Wallet database not available";
        return error;
    }
    
    // Query transactions
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT txid, category, amount, address, block_height, block_time FROM transactions ORDER BY block_time DESC LIMIT 100";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Json::Value tx;
            tx["txid"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            tx["category"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            tx["amount"] = sqlite3_column_double(stmt, 2);
            tx["address"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            tx["blockheight"] = sqlite3_column_int(stmt, 4);
            tx["blocktime"] = sqlite3_column_int64(stmt, 5);
            // Calculate actual confirmations based on blockchain height
            int confirmations = 1; // Default for unconfirmed
            if (m_blockchain) {
                uint32_t current_height = m_blockchain->getBlockHeight();
                uint32_t tx_height = sqlite3_column_int(stmt, 4); // blockheight column
                if (current_height >= tx_height) {
                    confirmations = current_height - tx_height + 1;
                }
            }
            tx["confirmations"] = confirmations;
            tx["time"] = sqlite3_column_int64(stmt, 5);
            
            result.append(tx);
        }
        sqlite3_finalize(stmt);
    }
    
    return result;
}

Json::Value RPCServer::miningStart(const Json::Value& params) {
    Json::Value result;
    if (!GetConfig().allow_local_mining) {
        return MiningProfileDisabledError();
    }
    try {
        // Access mining service via DaemonContext singleton
        auto* ctx = DaemonContext::instance();
        if (!ctx || !ctx->mining) {
            Json::Value error;
            error["code"] = -32601;
            error["message"] = "Mining service not available";
            return error;
        }

        auto mining = std::dynamic_pointer_cast<MiningService>(ctx->mining);
        if (!mining) {
            Json::Value error;
            error["code"] = -32601;
            error["message"] = "Mining service not initialized";
            return error;
        }

        int threads = 0;  // 0 = auto-detect
        std::string address;

        if (params.isArray() && params.size() > 0) {
            threads = params[0].asInt();
            if (params.size() > 1) {
                address = params[1].asString();
            }
        }

        // Resolve address if not explicitly provided
        if (address.empty()) {
            address = mining->getMiningAddress();
        }

        if (address.empty()) {
            Json::Value error;
            error["code"] = -8;
            error["message"] = "Mining address required. Use: mining.start <threads> <address>";
            return error;
        }

        // Validate address format and Taproot-only policy
        if (!dinero::mining::IsValidDineroAddress(address)) {
            Json::Value error;
            error["code"] = -5;
            error["message"] = "Invalid Dinero address: " + address;
            return error;
        }
        if (!dinero::mining::IsCoinbaseEligibleAddress(address)) {
            Json::Value error;
            error["code"] = -5;
            error["message"] = dinero::mining::GetTaprootRequiredMessage(address);
            return error;
        }

        // Configure and start mining
        mining->setMiningAddress(address);
        bool started = mining->getMiningManager().startMining(threads);
        if (!started) {
            Json::Value error;
            error["code"] = -32603;
            error["message"] = "Mining start failed (see logs for details)";
            return error;
        }

        const auto& stats = mining->getMiningManager().getStats();
        result["status"] = "started";
        result["threads"] = static_cast<int>(stats.active_threads.load());
        result["address"] = address;
        result["hashrate"] = stats.current_hashrate.load();
        result["blocks_mined"] = static_cast<int>(stats.blocks_found.load());
        return result;
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Mining start failed: ") + e.what();
        return error;
    }
}

Json::Value RPCServer::miningStop(const Json::Value& params) {
    Json::Value result;
    if (!GetConfig().allow_local_mining) {
        return MiningProfileDisabledError();
    }

    try {
        auto* ctx = DaemonContext::instance();
        if (!ctx || !ctx->mining) {
            Json::Value error;
            error["code"] = -32601;
            error["message"] = "Mining service not available";
            return error;
        }

        auto mining = std::dynamic_pointer_cast<MiningService>(ctx->mining);
        if (!mining) {
            Json::Value error;
            error["code"] = -32601;
            error["message"] = "Mining service not initialized";
            return error;
        }

        const auto& stats = mining->getMiningManager().getStats();
        uint64_t blocks_found = stats.blocks_found.load();
        double hashrate = stats.current_hashrate.load();
        uint64_t total_hashes = stats.total_hashes.load();

        mining->getMiningManager().stopMining();

        result["status"] = "stopped";
        result["blocks_mined"] = static_cast<int>(blocks_found);
        result["final_hashrate"] = hashrate;
        result["total_hashes"] = static_cast<Json::UInt64>(total_hashes);
        return result;
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Mining stop failed: ") + e.what();
        return error;
    }
}

Json::Value RPCServer::miningStatus(const Json::Value& params) {
    Json::Value result;
    if (!GetConfig().allow_local_mining) {
        result["is_mining"] = false;
        result["supported"] = false;
        result["error"] = "Local mining disabled by sync profile: " + GetConfig().sync_profile;
        return result;
    }

    try {
        auto* ctx = DaemonContext::instance();
        if (!ctx || !ctx->mining) {
            result["is_mining"] = false;
            result["error"] = "Mining service not available";
            return result;
        }

        auto mining = std::dynamic_pointer_cast<MiningService>(ctx->mining);
        if (!mining) {
            result["is_mining"] = false;
            result["error"] = "Mining service not initialized";
            return result;
        }

        const auto& stats = mining->getMiningManager().getStats();
        result["is_mining"] = stats.is_mining.load();
        result["threads"] = static_cast<int>(stats.active_threads.load());
        result["hashrate_hs"] = stats.current_hashrate.load();
        result["blocks_found"] = static_cast<int>(stats.blocks_found.load());
        result["total_hashes"] = static_cast<Json::UInt64>(stats.total_hashes.load());
        result["last_block_ts"] = static_cast<Json::UInt64>(stats.last_block_time.load());
        result["current_job_id"] = stats.current_job_id;
        result["current_job_height"] = static_cast<int>(stats.current_height);
        result["current_job_bits"] = static_cast<int>(stats.current_difficulty);
        return result;
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Mining status failed: ") + e.what();
        return error;
    }
}

Json::Value RPCServer::estimateSmartFee(const Json::Value& params) {
    Json::Value result;
    
    try {
        int conf_target = 6; // Default 6 blocks
        std::string estimate_mode = "CONSERVATIVE";
        
        if (params.isArray() && params.size() > 0) {
            conf_target = params[0].asInt();
            if (params.size() > 1) {
                estimate_mode = params[1].asString();
            }
        }
        
        // Implement basic fee estimation based on network conditions
        double base_fee = 1000.0; // 1000 una/kvB base fee
        double priority_multiplier = 1.0;
        
        // Adjust fee based on confirmation target
        if (conf_target <= 2) {
            priority_multiplier = 3.0; // High priority
        } else if (conf_target <= 6) {
            priority_multiplier = 2.0; // Medium priority  
        } else {
            priority_multiplier = 1.0; // Low priority
        }
        
        double estimated_fee = base_fee * priority_multiplier;
        result["feerate"] = estimated_fee;
        result["feerate_din"] = std::to_string(estimated_fee / 1000000.0);
        result["blocks"] = conf_target;
        result["errors"] = Json::Value(Json::arrayValue);
        result["warnings"] = Json::Value(Json::arrayValue);
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Fee estimation failed: ") + e.what();
        return error;
    }
    
    return result;
}

// === PSBT WORKFLOW IMPLEMENTATIONS (LEGACY DISABLED) ===
// All PSBT handlers removed - use vNext RPC in main.cpp instead

// Legacy PSBT handler removed - use vNext RPC instead
Json::Value RPCServer::rpcPsbtCreate(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.create)";
    return error;
}

Json::Value RPCServer::rpcPsbtFund(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.fund)";
    return error;
}

Json::Value RPCServer::rpcPsbtSign(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.sign)";
    return error;
}

Json::Value RPCServer::rpcPsbtSubmit(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.submit)";
    return error;
}

Json::Value RPCServer::rpcPsbtDecode(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.decode)";
    return error;
}

// Legacy PSBT decode implementation removed

// === PSBT FINALIZE AND EXTRACT IMPLEMENTATIONS ===

using namespace boost::beast::detail::base64;

static std::vector<uint8_t> b64decode(const std::string& b64) {
  std::vector<uint8_t> out(decoded_size(b64.size()));
  auto res = decode(out.data(), b64.data(), b64.size());
  out.resize(res.first);
  return out;
}

static std::string b64encode(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.resize(encoded_size(bytes.size()));
  auto n = encode(&out[0], bytes.data(), bytes.size());
  out.resize(n);
  return out;
}

static std::string hex(const std::vector<uint8_t>& v) {
  static const char* H = "0123456789abcdef";
  std::string s; s.resize(v.size()*2);
  for (size_t i=0;i<v.size();++i) { s[2*i]=H[v[i]>>4]; s[2*i+1]=H[v[i]&0xF]; }
  return s;
}

// Legacy PSBT finalize handler removed - use vNext RPC instead
Json::Value RPCServer::rpcPsbtFinalize(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.finalize)";
    return error;
}

// Legacy PSBT finalize implementation removed

// Legacy PSBT extract handler removed - use vNext RPC instead
Json::Value RPCServer::rpcPsbtExtract(const Json::Value& params) {
    Json::Value error;
    error["code"] = -32601;
    error["message"] = "Legacy PSBT handler disabled - use vNext RPC (psbt.extract)";
    return error;
}

// Legacy PSBT extract implementation removed

// === REALTIME EVENTS IMPLEMENTATIONS ===

Json::Value RPCServer::rpcEventsSubscribe(const Json::Value& params) {
    try {
        if (!params.isObject() || !params.isMember("topics") || !params["topics"].isArray()) {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "events.subscribe requires topics[] array";
            return error;
        }

        std::vector<std::string> topics;
        for (const auto& topic : params["topics"]) {
            if (!topic.isString()) {
                Json::Value error;
                error["code"] = -32602;
                error["message"] = "All topics must be strings";
                return error;
            }
            
            std::string topic_str = topic.asString();
            
            // Validate supported topics
            if (topic_str != "new_block" && topic_str != "mempool_tx" && 
                topic_str != "wallet_tx" && topic_str != "mining_update") {
                Json::Value error;
                error["code"] = -32602;
                error["message"] = "Unsupported topic: " + topic_str;
                return error;
            }
            
            topics.push_back(topic_str);
        }

        // Generate session ID
        std::string session_id = "ws_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        // Basic WebSocket subscription (simplified)
        // In a real implementation, this would integrate with the WebSocket hub
        
        Json::Value result;
        result["session_id"] = session_id;
        // Use the actual WebSocket port (21001) - the separate WebSocket server
        result["ws_url"] = "ws://127.0.0.1:21001/ws?sid=" + session_id;
        
        Json::Value subscribed_topics(Json::arrayValue);
        for (const auto& topic : topics) {
            subscribed_topics.append(topic);
        }
        result["topics"] = subscribed_topics;
        
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Event subscription failed: ") + e.what();
        return error;
    }
}

Json::Value RPCServer::rpcEventsUnsubscribe(const Json::Value& params) {
    try {
        if (!params.isObject() || !params.isMember("session_id") || !params["session_id"].isString()) {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "events.unsubscribe requires session_id string";
            return error;
        }

        std::string session_id = params["session_id"].asString();

        // Basic WebSocket unsubscription (simplified)
        // In a real implementation, this would close the WebSocket session
        
        Json::Value result;
        result["ok"] = true;
        result["session_id"] = session_id;
        result["closed"] = true;
        
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = std::string("Event unsubscription failed: ") + e.what();
        return error;
    }
}

void RPCServer::start() {
    // Start the RPC server
    if (!initialize()) {
        g_logger.error("Failed to initialize RPC server");
        return;
    }

    m_running.store(true);

    // Start RPC server thread
    m_rpc_thread = std::thread(&RPCServer::run, this);

    g_logger.info("RPC server started on port " + std::to_string(m_port));
}

// === Mining Block Generation ===
Json::Value RPCServer::generateToAddress(const Json::Value& params) {
    try {
        // Validate parameters
        if (!params.isArray() || params.size() < 2) {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "Invalid params: expected [nblocks, address]";
            return error;
        }

        int nblocks = params[0].asInt();
        std::string address = params[1].asString();

        if (nblocks <= 0 || nblocks > 1000) {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "Invalid number of blocks (must be 1-1000)";
            return error;
        }

        // Check if mining and blockchain are available
        if (!m_mining || !m_blockchain) {
            Json::Value error;
            error["code"] = -32603;
            error["message"] = "Mining or blockchain component not initialized";
            return error;
        }

        g_logger.info("generatetoaddress: Mining " + std::to_string(nblocks) + " blocks to " + address);

        // Decode the address to get witness version and program
        int witver = 0;
        std::vector<uint8_t> witprog;
        std::string expected_hrp = mining::GetBech32HRP();

        if (!mining::Bech32DecodeSegwit(address, expected_hrp, witver, witprog)) {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "Invalid address format: failed to decode bech32/bech32m";
            return error;
        }

        // Determine address type from witness version
        MiningAddressType addrType;
        if (witver == 0 && witprog.size() == 20) {
            addrType = MiningAddressType::TRANSPARENT;  // P2WPKH
        } else if (witver == 1 && witprog.size() == 32) {
            addrType = MiningAddressType::TAPROOT;  // P2TR
        } else if (witver == 2) {
            addrType = MiningAddressType::CONFIDENTIAL;  // CT
        } else {
            Json::Value error;
            error["code"] = -32602;
            error["message"] = "Unsupported address type: witness version " + std::to_string(witver);
            return error;
        }

        // Set the global mining override - this is what createBlockTemplate() checks
        g_mining_override_active = true;
        g_mining_override_witver = witver;
        g_mining_override_witprog = witprog;
        g_mining_override_type = addrType;

        g_logger.info("[generatetoaddress] Set global mining override: type=" +
                     std::to_string(static_cast<int>(addrType)) +
                     " witver=" + std::to_string(g_mining_override_witver) +
                     " witprog_len=" + std::to_string(g_mining_override_witprog.size()));

        Json::Value result(Json::arrayValue);

        try {
            // Mine blocks one at a time
            for (int i = 0; i < nblocks; i++) {
                uint32_t start_height = m_blockchain->getBlockHeight();

                // Create block template using Mining system
                Json::Value template_result = m_mining->createBlockTemplate();

                if (template_result.isMember("error")) {
                    g_logger.error("Failed to create block template: " + template_result["error"].asString());

                    // Restore original address
                    if (!original_address.empty()) {
                        m_mining->setMiningAddress(original_address);
                    }

                    Json::Value error;
                    error["code"] = -32603;
                    error["message"] = "Failed to create block template: " + template_result["error"].asString();
                    return error;
                }

                // Mine the block (this will call the mining loop)
                // For regtest, difficulty should be very low so this should be fast
                bool mined = false;
                if (m_miner_core) {
                    // Use MinerCore to actually mine the block
                    // This is a simplified approach - real implementation would be more robust
                    m_mining->setMiningEnabled(true);

                    // Wait for block to be mined (check every 100ms, timeout after 30 seconds)
                    int max_wait = 300; // 30 seconds
                    int wait_count = 0;

                    while (wait_count < max_wait) {
                        uint32_t current_height = m_blockchain->getBlockHeight();
                        if (current_height > start_height) {
                            mined = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        wait_count++;
                    }

                    m_mining->setMiningEnabled(false);
                }

                if (!mined) {
                    g_logger.error("Failed to mine block within timeout");

                    // Clear global mining override
                    g_mining_override_active = false;
                    g_mining_override_witprog.clear();

                    Json::Value error;
                    error["code"] = -32603;
                    error["message"] = "Mining timeout after 30 seconds";
                    return error;
                }

                // Get the hash of the newly mined block
                std::string block_hash = m_blockchain->getLatestHash();
                result.append(block_hash);

                g_logger.info("Mined block " + std::to_string(i + 1) + "/" + std::to_string(nblocks) + ": " + block_hash);

                // Phase 34: Remove confirmed transactions from mempool
                if (m_mempool && execution_context_.hasChainDB()) {
                    uint256 block_hash_uint256 = uint256::FromHexUnsafe(block_hash);
                    auto block_result = ReadLegacyRpcBlock(block_hash_uint256, execution_context_.chain_db);

                    if (block_result.ok()) {
                        const Block& block = block_result.value();
                        std::vector<uint256> confirmed_txids;

                        // Extract all transaction IDs from the block (skip coinbase for mempool)
                        for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
                            confirmed_txids.push_back(block.vtx[tx_idx].GetTxid());
                        }

                        // Remove confirmed transactions from mempool
                        if (!confirmed_txids.empty()) {
                            m_mempool->removeConfirmedTransactions(confirmed_txids);
                            g_logger.info("Removed " + std::to_string(confirmed_txids.size()) +
                                        " confirmed transactions from mempool");
                        }
                    }
                }
            }

            // Clear global mining override
            g_mining_override_active = false;
            g_mining_override_witprog.clear();

            return result;

        } catch (const std::exception& e) {
            // Clear global mining override on error
            g_mining_override_active = false;
            g_mining_override_witprog.clear();

            Json::Value error;
            error["code"] = -32603;
            error["message"] = "Mining error: " + std::string(e.what());
            return error;
        }

    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = "Internal error: " + std::string(e.what());
        return error;
    }
}

} // namespace dinero
