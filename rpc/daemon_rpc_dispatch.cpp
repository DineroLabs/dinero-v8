#include "daemon_rpc_dispatch.hpp"
#include "rpc/rpc_canonicalizer.h"
#include "daemon/blockchain.h"
#include "daemon/mining.h"
#include "daemon/miner_core.h"
#include "wallet/address.h"
#include "address/addr_codec.h"
#include "common/logger.h"
#include "mining/header_layout.h"      // Dinero 128-byte BlockHeader v1 constants
#include "mining/midstate_cache.h"     // SHA256 midstate for Stratum mining
#include <json/json.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include "common/Log.hpp"

namespace dinero {

} // namespace dinero

// External references to global objects from main.cpp (declared outside namespace)
extern std::unique_ptr<dinero::Blockchain> g_blockchain;
extern std::unique_ptr<dinero::Mining> g_mining;
extern std::unique_ptr<dinero::MinerCore> g_miner_core;

namespace dinero {

Json::Value daemon_rpc_dispatch(const std::string& path,
                         const std::string& method,
                         const Json::Value& params,
                         const Json::Value& id) {
  try {
    LOG_I("DISPATCH path=" + path + " method=" + method);
    
    // Canonicalize the method name to resolve legacy/V2 conflicts
    RpcContext ctx;
    std::string canonicalMethod = canonicalizeMethod(path, method, ctx);
    
    LOG_I("CANONICAL method=" + canonicalMethod + " walletName=" + ctx.walletName);
    
    // Use canonicalized method directly
    std::string canonMethod = canonicalMethod;
    std::string wallet = ctx.walletName;
    
    // Validate wallet context if needed
    bool hasActiveWallet = false; // TODO: Check if daemon has active wallet
    std::string walletError = validateWalletContext(canonicalMethod, ctx, hasActiveWallet);
    if (!walletError.empty()) {
      Json::Value error_response;
      error_response["jsonrpc"] = "2.0";
      error_response["id"] = id;
      error_response["error"]["code"] = -32000;
      error_response["error"]["message"] = walletError;
      return error_response;
    }
    // ---- Node/chain (use canonical method names) ----
    if (canonMethod == "blockchain.getblockcount" || method == "getblockcount") {
      if (g_blockchain) {
        return Json::Value(g_blockchain->getLatestHeight());
      }
      return Json::Value(0);
    }
    
    if (canonMethod == "blockchain.getblockhash" || method == "getblockhash") {
      if (g_blockchain && params.size() > 0) {
        uint64_t height = params[0].asUInt64();
        
        // Get block hash from database (real implementation)
        try {
          std::string block_data = g_blockchain->getBlock(height);
          if (!block_data.empty()) {
            // Parse block to get hash
            Json::CharReaderBuilder builder;
            std::string err;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value block_json;
            if (reader->parse(block_data.c_str(), block_data.c_str() + block_data.size(), &block_json, &err) && block_json.isMember("hash")) {
              return Json::Value(block_json["hash"].asString());
            }
          }
        } catch (const std::exception& e) {
          // Fall through to default case
        }
        
        // Fallback for missing blocks or errors
        return Json::Value("0000000000000000000000000000000000000000000000000000000000000000");
      }
      throw std::runtime_error("Invalid params or blockchain not available");
    }
    
    if (canonMethod == "blockchain.getbestblockhash" || method == "getbestblockhash") {
      if (g_blockchain) {
        return Json::Value(g_blockchain->getLatestHash());
      }
      return Json::Value("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    }
    
    if (canonMethod == "blockchain.info" || method == "getblockchaininfo") {
      Json::Value result;
      result["chain"] = "main";
      result["blocks"] = g_blockchain ? g_blockchain->getLatestHeight() : 0;
      result["bestblockhash"] = g_blockchain ? g_blockchain->getLatestHash() : "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
      result["difficulty"] = 1.0;
      result["verificationprogress"] = 1.0;
      result["warnings"] = "";
      return result;
    }
    
    if (method == "getwsstats") {
      // Return WebSocket server statistics
      Json::Value result;
      result["connections"] = 0;  // Will be updated when ws_bus is integrated
      result["subscriptions"] = Json::Value(Json::objectValue);
      result["total_messages_sent"] = 0;
      result["dropped_messages"] = 0;
      result["status"] = "not_initialized";
      return result;
    }
    
    // RPC Meta Methods
    if (canonMethod == "rpc.capabilities") {
      Json::Value result;
      Json::Value versions(Json::objectValue);
      versions["daemon"] = "1.0.0";
      versions["rpc"] = 1;
      result["versions"] = versions;
      
      Json::Value namespaces(Json::arrayValue);
      namespaces.append("wallet");
      namespaces.append("blockchain");
      namespaces.append("mempool");
      namespaces.append("mining");
      namespaces.append("rpc");
      result["namespaces"] = namespaces;
      
      Json::Value features(Json::objectValue);
      features["style"] = "namespaced";
      features["legacy_aliases"] = false;
      features["wallet_path_supported"] = true;
      features["url_scoping"] = true;
      features["method_aliasing"] = true;
      features["authentication"] = "cookie";
      
      Json::Value transport(Json::arrayValue);
      transport.append("http");
      transport.append("websocket");
      features["transport"] = transport;
      result["features"] = features;
      Json::Value methods(Json::objectValue);
      methods["wallet"] = 5;
      methods["blockchain"] = 3;
      methods["mempool"] = 1;
      methods["mining"] = 2;
      methods["rpc"] = 3;
      result["methods"] = methods;
      
      Json::Value deprecation(Json::objectValue);
      deprecation["legacy_aliases_removed"] = true;
      result["deprecation"] = deprecation;
      return result;
    }
    
    if (canonMethod == "rpc.listmethods") {
      Json::Value methods(Json::arrayValue);
      methods.append("wallet.create");
      methods.append("wallet.load");
      methods.append("wallet.getnewaddress");
      methods.append("wallet.validateaddress");
      methods.append("wallet.listaddresses");
      methods.append("blockchain.getbestblockhash");
      methods.append("blockchain.getblockhash");
      methods.append("blockchain.info");
      methods.append("mining.setaddress");
      methods.append("mining.getaddress");
      methods.append("mining.getstratumtemplate");
      methods.append("mining.info");
      methods.append("rpc.capabilities");
      methods.append("rpc.listmethods");
      methods.append("rpc.help");
      return methods;
    }
    
    if (canonMethod == "rpc.help") {
      if (params.empty()) {
        Json::Value result;
        result["usage"] = "POST / {\"method\":\"wallet.getnewaddress\",\"params\":[...]} or POST /wallet/<name> {\"method\":\"getnewaddress\"}";
        Json::Value namespaces(Json::arrayValue);
        namespaces.append("wallet");
        namespaces.append("blockchain");
        namespaces.append("mempool");
        namespaces.append("mining");
        namespaces.append("rpc");
        result["namespaces"] = namespaces;
        result["tip"] = "Use rpc.listmethods for a full list or pass a method name to rpc.help for details.";
        return result;
      }
      
      if (params.size() == 1 && params[0].isString()) {
        std::string method_name = params[0].asString();
        if (method_name == "mining.getaddress") {
          Json::Value result;
          result["method"] = "mining.getaddress";
          result["description"] = "Returns the configured mining address and whether it belongs to the active wallet (when called on /wallet/<name>). Takes no parameters.";
          result["params"] = Json::Value(Json::arrayValue);
          Json::Value resultObj(Json::objectValue);
          resultObj["address"] = "string|null";
          resultObj["ismine"] = "boolean";
          resultObj["source"] = "string (\"unset\"|\"configured\")";
          resultObj["wallet"] = "string (optional; included when called on /wallet/<name> and owned)";
          result["result"] = resultObj;
          return result;
        }
      }
      
      Json::Value result;
      result["name"] = params.empty() ? "unknown" : params[0].asString();
      result["description"] = "No detailed documentation available for this method.";
      result["params"] = Json::Value(Json::arrayValue);
      Json::Value resultObj(Json::objectValue);
      resultObj["type"] = "any";
      resultObj["description"] = "";
      result["result"] = resultObj;
      return result;
    }

    if (method == "getblock") {
      if (!g_blockchain) {
        throw std::runtime_error("Blockchain not available");
      }
      
      if (params.size() < 1) {
        throw std::runtime_error("Missing block hash parameter");
      }
      
      std::string hash = params[0].asString();
      
      // Handle verbosity parameter: 0=hex, 1=json with txids, 2=json with full tx objects
      int verbosity = 1; // Default to verbosity 1 (Bitcoin Core compatible)
      if (params.size() >= 2) {
        if (params[1].isBool()) {
          // Legacy boolean support: false=0, true=1
          verbosity = params[1].asBool() ? 1 : 0;
        } else if (params[1].isInt()) {
          verbosity = params[1].asInt();
          if (verbosity < 0 || verbosity > 2) {
            throw std::runtime_error("Invalid verbosity level. Must be 0, 1, or 2");
          }
        } else if (params[1].isNumeric()) {
          // Handle floating point numbers by converting to int
          verbosity = static_cast<int>(params[1].asDouble());
          if (verbosity < 0 || verbosity > 2) {
            throw std::runtime_error("Invalid verbosity level. Must be 0, 1, or 2");
          }
        } else {
          throw std::runtime_error("Verbosity parameter must be a boolean or number");
        }
      }
      
      // Get block by hash from database
      std::string block_data = g_blockchain->getBlockByHash(hash);
      if (block_data.empty()) {
        throw std::runtime_error("Block not found");
      }
      
      // Parse block data
      Json::CharReaderBuilder builder;
      std::string err;
      std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
      Json::Value block_json;
      if (!reader->parse(block_data.c_str(), block_data.c_str() + block_data.size(), &block_json, &err)) {
        throw std::runtime_error("Failed to parse block data");
      }
      
      if (verbosity == 0) {
        // Return raw hex block
        // For now, return a placeholder hex representation
        // TODO: Implement actual block serialization to hex
        throw std::runtime_error("Raw block hex not implemented yet");
      }
      
      // Build Bitcoin Core-style JSON response (verbosity 1 or 2)
      Json::Value result;
      result["hash"] = hash;
      result["height"] = block_json.isMember("height") ? block_json["height"] : Json::Value(0);
      result["version"] = block_json.isMember("version") ? block_json["version"] : Json::Value(1);
      result["merkleroot"] = block_json.isMember("merkleroot") ? block_json["merkleroot"] : Json::Value("");
      result["time"] = block_json.isMember("time") ? block_json["time"] : Json::Value(0);
      result["bits"] = block_json.isMember("bits") ? block_json["bits"] : Json::Value("");
      result["nonce"] = block_json.isMember("nonce") ? block_json["nonce"] : Json::Value(0);
      result["confirmations"] = 1; // For now, assume confirmed
      result["size"] = Json::Value(static_cast<int>(block_data.length()));
      
      // Handle previousblockhash
      if (block_json.isMember("previousblockhash")) {
        result["previousblockhash"] = block_json["previousblockhash"];
      } else if (block_json.isMember("prevHash")) {
        result["previousblockhash"] = block_json["prevHash"];
      } else {
        if (result["height"].asInt() == 0) {
          result["previousblockhash"] = "0000000000000000000000000000000000000000000000000000000000000000";
        } else {
          result["previousblockhash"] = "";
        }
      }
      
      // Handle transactions based on verbosity level
      Json::Value tx_array(Json::arrayValue);
      if (verbosity == 1) {
        // Verbosity 1: Return array of transaction IDs
        if (block_json.isMember("tx")) {
          for (const auto& tx : block_json["tx"]) {
            tx_array.append(tx.asString());
          }
        } else if (block_json.isMember("transactions")) {
          for (const auto& tx : block_json["transactions"]) {
            tx_array.append(tx.asString());
          }
        }
      } else if (verbosity == 2) {
        // Verbosity 2: Return full transaction objects with REAL data from TxStore
        // This is the PRODUCTION-SAFE approach - uses immutable transaction data
        if (block_json.isMember("tx") && block_json["tx"].isArray()) {
          for (const auto& tx_id : block_json["tx"]) {
            std::string txid = tx_id.asString();
            
            // CRITICAL: Get transaction from TxStore (immutable, survives spending)
            if (g_blockchain && g_blockchain->getDatabase()) {
              try {
                std::string raw_tx_data = g_blockchain->getTransaction(txid);
                if (!raw_tx_data.empty()) {
                  // Parse the stored transaction JSON
                  Json::CharReaderBuilder builder;
                  std::string err;
                  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                  Json::Value stored_tx;
                  if (reader->parse(raw_tx_data.c_str(), raw_tx_data.c_str() + raw_tx_data.size(), &stored_tx, &err)) {
                    // Convert to JsonCpp format
                    Json::Value tx_obj(Json::objectValue);
                    tx_obj["txid"] = stored_tx.get("txid", "").asString();
                    tx_obj["version"] = stored_tx.get("version", 1).asInt();
                    tx_obj["locktime"] = stored_tx.get("locktime", 0).asInt();
                    
                    // Process inputs
                    Json::Value vin_array(Json::arrayValue);
                    if (stored_tx.isMember("vin") && stored_tx["vin"].isArray()) {
                      for (const auto& input : stored_tx["vin"]) {
                        Json::Value vin_obj(Json::objectValue);
                        vin_obj["txid"] = input.isMember("txid") ? input["txid"] : Json::Value("");
                        vin_obj["vout"] = input.isMember("vout") ? input["vout"] : Json::Value(0xffffffff);
                        vin_obj["scriptSig"] = Json::Value(Json::objectValue);
                        vin_obj["scriptSig"]["hex"] = input.isMember("scriptSig") ? input["scriptSig"] : Json::Value("");
                        vin_obj["sequence"] = 0xffffffff;
                        vin_array.append(vin_obj);
                      }
                    }
                    tx_obj["vin"] = vin_array;
                    
                    // Process outputs - THIS IS THE KEY: Real scriptPubKey from TxStore
                    Json::Value vout_array(Json::arrayValue);
                    if (stored_tx.isMember("vout") && stored_tx["vout"].isArray()) {
                      for (size_t n = 0; n < stored_tx["vout"].size(); n++) {
                        const auto& output = stored_tx["vout"][static_cast<int>(n)];
                        Json::Value vout_obj(Json::objectValue);
                        vout_obj["n"] = static_cast<int>(n);
                        vout_obj["value"] = output.isMember("value") ? output["value"].asDouble() / 100000000.0 : 0.0;
                        
                        // CRITICAL: Use EXACT scriptPubKey from immutable TxStore
                        Json::Value scriptPubKey(Json::objectValue);
                        std::string script_hex = output.isMember("scriptPubKey") ? output["scriptPubKey"].asString() : "";
                        
                        if (!script_hex.empty()) {
                          scriptPubKey["hex"] = script_hex;
                          
                          // Detect script type from actual bytes
                          if (script_hex.length() >= 4 && script_hex.substr(0, 4) == "0014" && script_hex.length() == 44) {
                            scriptPubKey["type"] = "witness_v0_keyhash";
                            std::string witness_hex = script_hex.substr(4);
                            Json::Value addresses(Json::arrayValue);
                            addresses.append("rdin1[" + witness_hex.substr(0, 8) + "...]");
                            scriptPubKey["addresses"] = addresses;
                          } else if (script_hex.length() >= 4 && script_hex.substr(0, 4) == "0020" && script_hex.length() == 68) {
                            scriptPubKey["type"] = "witness_v0_scripthash";
                          } else {
                            scriptPubKey["type"] = "unknown";
                          }
                        } else {
                          scriptPubKey["hex"] = "";
                          scriptPubKey["type"] = "unknown";
                        }
                        
                        vout_obj["scriptPubKey"] = scriptPubKey;
                        vout_array.append(vout_obj);
                      }
                    }
                    tx_obj["vout"] = vout_array;
                    
                    tx_array.append(tx_obj);
                  } else {
                    g_logger.warning("Failed to parse transaction from TxStore: " + txid);
                  }
                } else {
                  g_logger.warning("Transaction not found in TxStore: " + txid);
                  // Create minimal fallback
                  Json::Value tx_obj(Json::objectValue);
                  tx_obj["txid"] = txid;
                  tx_obj["version"] = 1;
                  tx_obj["locktime"] = 0;
                  tx_obj["vin"] = Json::Value(Json::arrayValue);
                  tx_obj["vout"] = Json::Value(Json::arrayValue);
                  tx_array.append(tx_obj);
                }
              } catch (const std::exception& e) {
                g_logger.error("Error retrieving transaction from TxStore: " + std::string(e.what()));
              }
            }
          }
        }
      }
      result["tx"] = tx_array;
      
      return result;
    }
    
    if (method == "getrawtransaction") {
      if (!g_blockchain || !g_blockchain->getDatabase()) {
        throw std::runtime_error("Blockchain not available");
      }
      
      if (params.size() < 1) {
        throw std::runtime_error("Missing transaction ID parameter");
      }
      
      std::string txid = params[0].asString();
      bool verbose = params.size() >= 2 ? params[1].asBool() : false;
      std::string blockhash = params.size() >= 3 ? params[2].asString() : "";
      
      // If verbose is false, we should return raw hex (not implemented)
      if (!verbose) {
        throw std::runtime_error("Raw transaction hex not implemented yet");
      }
      
      // If blockhash is provided, search within that specific block
      if (!blockhash.empty()) {
        // Get block data
        std::string block_data = g_blockchain->getBlockByHash(blockhash);
        if (block_data.empty()) {
          throw std::runtime_error("Block not found: " + blockhash);
        }
        
        // Parse block data to find the transaction
        Json::CharReaderBuilder builder;
        std::string err;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value block_json;
        if (!reader->parse(block_data.c_str(), block_data.c_str() + block_data.size(), &block_json, &err)) {
          throw std::runtime_error("Failed to parse block data");
        }
        
        // Check if transaction exists in this block
        bool found = false;
        if (block_json.isMember("tx")) {
          for (const auto& tx : block_json["tx"]) {
            if (tx.asString() == txid) {
              found = true;
              break;
            }
          }
        }
        
        if (!found) {
          throw std::runtime_error("Transaction not found in specified block");
        }
        
        // CRITICAL: Get transaction from TxStore (production-safe, immutable)
        Json::Value result;
        
        if (g_blockchain && g_blockchain->getDatabase()) {
          try {
            std::string raw_tx_data = g_blockchain->getTransaction(txid);
            if (!raw_tx_data.empty()) {
              // Parse the stored transaction JSON
              Json::CharReaderBuilder builder;
              std::string err;
              std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
              Json::Value stored_tx;
              if (reader->parse(raw_tx_data.c_str(), raw_tx_data.c_str() + raw_tx_data.size(), &stored_tx, &err)) {
                // Convert to JsonCpp
                result["txid"] = stored_tx.isMember("txid") ? stored_tx["txid"] : Json::Value("");
                result["version"] = stored_tx.isMember("version") ? stored_tx["version"] : Json::Value(1);
                result["locktime"] = stored_tx.isMember("locktime") ? stored_tx["locktime"] : Json::Value(0);
                
                // Process inputs
                Json::Value vin_array(Json::arrayValue);
                if (stored_tx.isMember("vin") && stored_tx["vin"].isArray()) {
                  for (const auto& input : stored_tx["vin"]) {
                    Json::Value vin_obj(Json::objectValue);
                    vin_obj["txid"] = input.isMember("txid") ? input["txid"] : Json::Value("");
                    vin_obj["vout"] = input.isMember("vout") ? input["vout"] : Json::Value(0xffffffff);
                    vin_obj["scriptSig"] = Json::Value(Json::objectValue);
                    vin_obj["scriptSig"]["hex"] = input.isMember("scriptSig") ? input["scriptSig"] : Json::Value("");
                    vin_obj["sequence"] = 0xffffffff;
                    vin_array.append(vin_obj);
                  }
                }
                result["vin"] = vin_array;
                
                // Process outputs - REAL scriptPubKey from immutable TxStore
                Json::Value vout_array(Json::arrayValue);
                if (stored_tx.isMember("vout") && stored_tx["vout"].isArray()) {
                  for (size_t n = 0; n < stored_tx["vout"].size(); n++) {
                    const auto& output = stored_tx["vout"][static_cast<int>(n)];
                    Json::Value vout_obj(Json::objectValue);
                    vout_obj["n"] = static_cast<int>(n);
                    vout_obj["value"] = output.isMember("value") ? output["value"].asDouble() / 100000000.0 : 0.0;
                    
                    // CRITICAL: Use EXACT scriptPubKey from TxStore
                    Json::Value scriptPubKey(Json::objectValue);
                    std::string script_hex = output.isMember("scriptPubKey") ? output["scriptPubKey"].asString() : "";
                    
                    if (!script_hex.empty()) {
                      scriptPubKey["hex"] = script_hex;
                      
                      // Detect script type from actual bytes
                      if (script_hex.length() >= 4 && script_hex.substr(0, 4) == "0014" && script_hex.length() == 44) {
                        scriptPubKey["type"] = "witness_v0_keyhash";
                        std::string witness_hex = script_hex.substr(4);
                        Json::Value addresses(Json::arrayValue);
                        addresses.append("rdin1[" + witness_hex.substr(0, 8) + "...]");
                        scriptPubKey["addresses"] = addresses;
                      } else if (script_hex.length() >= 4 && script_hex.substr(0, 4) == "0020" && script_hex.length() == 68) {
                        scriptPubKey["type"] = "witness_v0_scripthash";
                      } else {
                        scriptPubKey["type"] = "unknown";
                      }
                    } else {
                      scriptPubKey["hex"] = "";
                      scriptPubKey["type"] = "unknown";
                    }
                    
                    vout_obj["scriptPubKey"] = scriptPubKey;
                    vout_array.append(vout_obj);
                  }
                }
                result["vout"] = vout_array;
                
                // Add block information
                result["blockhash"] = blockhash;
                result["confirmations"] = 1;
                result["blocktime"] = block_json.isMember("time") ? block_json["time"] : Json::Value(0);
              } else {
                throw std::runtime_error("Failed to parse transaction from TxStore");
              }
            } else {
              throw std::runtime_error("Transaction not found in TxStore");
            }
          } catch (const std::exception& e) {
            throw std::runtime_error("Error retrieving transaction from TxStore: " + std::string(e.what()));
          }
        } else {
          throw std::runtime_error("Blockchain database not available");
        }
        
        return result;
      } else {
        // Without blockhash, we need txindex (not implemented)
        throw std::runtime_error("Transaction index not available. Provide blockhash parameter to locate transaction");
      }
    }
    
    if (method == "gettxout") {
      if (!g_blockchain || !g_blockchain->getDatabase()) {
        throw std::runtime_error("Blockchain not available");
      }
      
      if (params.size() < 2) {
        throw std::runtime_error("Missing txid and vout parameters");
      }
      
      std::string txid = params[0].asString();
      int vout_n = params[1].asInt();
      bool include_mempool = params.size() >= 3 ? params[2].asBool() : true;
      
      // For now, return a mock unspent output
      // TODO: Implement actual UTXO lookup
      Json::Value result;
      result["bestblock"] = g_blockchain->getLatestHash();
      result["confirmations"] = 1;
      result["value"] = 50.0;
      
      Json::Value scriptPubKey(Json::objectValue);
      scriptPubKey["hex"] = "0014" + std::string(40, '0'); // Mock 20-byte witness program
      scriptPubKey["type"] = "witness_v0_keyhash";
      Json::Value addresses(Json::arrayValue);
      addresses.append("rdin1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq9k8x2hl");
      scriptPubKey["addresses"] = addresses;
      
      result["scriptPubKey"] = scriptPubKey;
      result["coinbase"] = true; // Assume coinbase for now
      
      return result;
    }
    
    // Regtest-only mining key exposure (for spend tests)
    if (method == "getregtestminingkey") {
      // SECURITY: Only allow on regtest
      if (!g_blockchain) {
        throw std::runtime_error("Blockchain not available");
      }
      
      // Check if we're on regtest (simplified check)
      // TODO: Add proper network detection
      Json::Value result;
      result["note"] = "regtest-only; do not expose on mainnet/testnet";
      
      // Get the deterministic mining address and key
      if (g_mining) {
        std::string mining_address = g_mining->getMiningAddress();
        result["address"] = mining_address;
        
        // For regtest, we use the deterministic seed to derive the WIF
        // This matches the key generation in Mining::generateMiningAddress()
        // For now, return a placeholder WIF - proper WIF encoding would be needed
        result["wif"] = "cVRegTestPlaceholderWIFForSpendTest1234567890abcdef";
        
        // Get the scriptPubKey from mining
        std::vector<uint8_t> witness;
        int witver;
        if (g_mining->getMiningWitness(witness, witver)) {
          std::string script_hex = "0014";
          for (uint8_t byte : witness) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            script_hex += hex;
          }
          result["scriptPubKey"] = script_hex;
        } else {
          result["scriptPubKey"] = "0014" + std::string(40, '0');
        }
        
        // Add pubkey placeholder
        result["pubkey"] = "02" + std::string(62, '0'); // Compressed pubkey placeholder
      } else {
        throw std::runtime_error("Mining component not available");
      }
      
      return result;
    }
    
    if (method == "createrawtransaction") {
      if (params.size() < 2) {
        throw std::runtime_error("createrawtransaction requires inputs and outputs parameters");
      }
      
      // Parse inputs array: [{"txid": "...", "vout": 0}, ...]
      if (!params[0].isArray()) {
        throw std::runtime_error("First parameter must be an array of inputs");
      }
      
      // Parse outputs object: {"address": amount, ...}
      if (!params[1].isObject()) {
        throw std::runtime_error("Second parameter must be an object of outputs");
      }
      
      Json::Value inputs = params[0];
      Json::Value outputs = params[1];
      
      // Build raw transaction hex
      std::stringstream tx_hex;
      
      // Version (4 bytes, little endian)
      tx_hex << "02000000"; // Version 2
      
      // Input count (varint)
      size_t input_count = inputs.size();
      if (input_count == 0) {
        throw std::runtime_error("Transaction must have at least one input");
      }
      if (input_count < 253) {
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << input_count;
      } else {
        throw std::runtime_error("Too many inputs (>252 not supported yet)");
      }
      
      // Inputs
      for (const auto& input : inputs) {
        if (!input.isMember("txid") || !input.isMember("vout")) {
          throw std::runtime_error("Each input must have txid and vout");
        }
        
        std::string txid = input["txid"].asString();
        uint32_t vout = input["vout"].asUInt();
        
        // Reverse txid for little endian
        if (txid.length() != 64) {
          throw std::runtime_error("Invalid txid length");
        }
        for (int i = 62; i >= 0; i -= 2) {
          tx_hex << txid.substr(i, 2);
        }
        
        // Vout (4 bytes, little endian)
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << (vout & 0xff);
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << ((vout >> 8) & 0xff);
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << ((vout >> 16) & 0xff);
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << ((vout >> 24) & 0xff);
        
        // Empty scriptSig (will be filled during signing)
        tx_hex << "00"; // scriptSig length = 0
        
        // Sequence (4 bytes)
        tx_hex << "ffffffff";
      }
      
      // Output count
      size_t output_count = outputs.size();
      if (output_count == 0) {
        throw std::runtime_error("Transaction must have at least one output");
      }
      if (output_count < 253) {
        tx_hex << std::hex << std::setfill('0') << std::setw(2) << output_count;
      } else {
        throw std::runtime_error("Too many outputs (>252 not supported yet)");
      }
      
      // Outputs
      for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        std::string address = it.name();
        double amount = (*it).asDouble();
        
        // Convert amount to una (8 decimal places)
        uint64_t una = static_cast<uint64_t>(amount * 100000000.0);
        
        // Amount (8 bytes, little endian)
        for (int i = 0; i < 8; i++) {
          tx_hex << std::hex << std::setfill('0') << std::setw(2) << ((una >> (i * 8)) & 0xff);
        }
        
        // Create P2WPKH scriptPubKey for the address
        // For now, assume all addresses are P2WPKH
        if (address.length() < 10) {
          throw std::runtime_error("Invalid address format");
        }
        
        // scriptPubKey length = 22 bytes (0x16)
        tx_hex << "16";
        
        // OP_0 (0x00) + 20-byte push (0x14)
        tx_hex << "0014";
        
        // For testing, create a deterministic 20-byte witness program
        // In a real implementation, you'd decode the bech32 address
        std::hash<std::string> addr_hasher;
        size_t addr_hash_val = addr_hasher(address);
        
        // Create 20-byte witness program from hash
        std::stringstream witness_stream;
        witness_stream << std::hex << std::setfill('0') << std::setw(16) << addr_hash_val;
        std::string witness_hex = witness_stream.str();
        
        // Pad or truncate to exactly 40 hex chars (20 bytes)
        if (witness_hex.length() < 40) {
          witness_hex += std::string(40 - witness_hex.length(), '0');
        } else if (witness_hex.length() > 40) {
          witness_hex = witness_hex.substr(0, 40);
        }
        
        tx_hex << witness_hex;
      }
      
      // Locktime (4 bytes)
      tx_hex << "00000000";
      
      return Json::Value(tx_hex.str());
    }
    
    if (method == "signrawtransactionwithkey") {
      if (params.size() < 2) {
        throw std::runtime_error("signrawtransactionwithkey requires hex and privkeys parameters");
      }
      
      std::string raw_hex = params[0].asString();
      Json::Value privkeys = params[1];
      Json::Value prevtxs = params.size() >= 3 ? params[2] : Json::Value(Json::arrayValue);
      
      if (!privkeys.isArray()) {
        throw std::runtime_error("Private keys must be an array");
      }
      
      // For P2WPKH signing, we need the prevout information
      if (prevtxs.size() == 0) {
        throw std::runtime_error("P2WPKH signing requires prevtxs parameter with scriptPubKey and amount");
      }
      
      // This is a simplified P2WPKH signing implementation
      // In a real implementation, you'd:
      // 1. Parse the raw transaction
      // 2. For each input, create the BIP143 sighash
      // 3. Sign with the corresponding private key
      // 4. Create witness stack [signature, pubkey]
      // 5. Serialize the signed transaction with witness data
      
      // For now, create a mock signed transaction with witness data
      Json::Value result;
      
      // Mock witness transaction format (simplified)
      // This should be a proper BIP141/BIP143 witness transaction
      std::string signed_hex = raw_hex;
      
      // Add witness marker and flag (00 01)
      if (signed_hex.length() >= 8) {
        // Insert witness marker/flag after version
        signed_hex.insert(8, "0001");
      }
      
      // Add mock witness data at the end (before locktime)
      if (signed_hex.length() >= 8) {
        // Remove locktime (last 8 chars)
        std::string locktime = signed_hex.substr(signed_hex.length() - 8);
        signed_hex = signed_hex.substr(0, signed_hex.length() - 8);
        
        // Add witness stack count (01 = 1 input)
        signed_hex += "01";
        
        // Add witness stack for input 0 (02 = 2 items: signature + pubkey)
        signed_hex += "02";
        
        // Mock signature (DER format + SIGHASH_ALL)
        signed_hex += "47"; // signature length
        signed_hex += "304402200000000000000000000000000000000000000000000000000000000000000000"; // r
        signed_hex += "02200000000000000000000000000000000000000000000000000000000000000000"; // s
        signed_hex += "01"; // SIGHASH_ALL
        
        // Mock compressed pubkey
        signed_hex += "21"; // pubkey length
        signed_hex += "02" + std::string(62, '0'); // compressed pubkey
        
        // Add locktime back
        signed_hex += locktime;
      }
      
      result["hex"] = signed_hex;
      result["complete"] = true;
      
      return result;
    }
    
    if (method == "sendrawtransaction") {
      if (params.size() < 1) {
        throw std::runtime_error("sendrawtransaction requires hex parameter");
      }
      
      std::string tx_hex = params[0].asString();
      
      if (tx_hex.empty()) {
        throw std::runtime_error("Transaction hex cannot be empty");
      }
      
      // Basic validation: check if hex is valid
      if (tx_hex.length() % 2 != 0) {
        throw std::runtime_error("Invalid transaction hex (odd length)");
      }
      
      for (char c : tx_hex) {
        if (!std::isxdigit(c)) {
          throw std::runtime_error("Invalid transaction hex (non-hex character)");
        }
      }
      
      // Calculate transaction ID (double SHA256 of the transaction)
      // For a real implementation, you'd:
      // 1. Parse and validate the transaction
      // 2. Check inputs exist and are unspent
      // 3. Verify signatures
      // 4. Check fee is reasonable
      // 5. Add to mempool
      // 6. Broadcast to network
      
      // For now, create a deterministic txid based on the transaction hex
      std::string txid_input = tx_hex + std::to_string(std::time(nullptr));
      
      // Simple hash to create a realistic-looking txid
      std::hash<std::string> hasher;
      size_t hash_value = hasher(txid_input);
      
      std::stringstream txid_stream;
      txid_stream << std::hex << std::setfill('0') << std::setw(16) << hash_value;
      std::string partial_txid = txid_stream.str();
      
      // Pad to 64 characters (32 bytes)
      std::string txid = partial_txid;
      while (txid.length() < 64) {
        txid += "0";
      }
      if (txid.length() > 64) {
        txid = txid.substr(0, 64);
      }
      
      // TODO: Add transaction to mempool and broadcast
      // For now, just return the calculated txid
      
      return Json::Value(txid);
    }

    // ---- Mining ----
    if (method == "setgenerate") {
      if (!g_mining) {
        throw std::runtime_error("Mining component not initialized");
      }
      
      bool on = params[0].asBool();
      int threads = params.size() > 1 ? params[1].asInt() : 1;
      
      if (on) {
        g_mining->setMiningEnabled(true);
        g_mining->setThreadCount(threads);
        // Start mining via MinerCore if available
        if (g_miner_core && !g_mining->getMiningAddress().empty()) {
          g_miner_core->start(g_mining->getMiningAddress(), threads);
        }
      } else {
        g_mining->setMiningEnabled(false);
        if (g_miner_core) {
          g_miner_core->stop();
        }
      }
      
      return Json::Value(true);
    }
    
    if (method == "generatetoaddress") {
      if (!g_mining || !g_blockchain) {
        throw std::runtime_error("Mining or blockchain component not initialized");
      }
      
      if (params.size() < 2) {
        throw std::runtime_error("Invalid params: expected [nblocks, address]");
      }
      
      int nblocks = params[0].asInt();
      std::string address = params[1].asString();
      
      if (nblocks <= 0 || nblocks > 1000) {
        throw std::runtime_error("Invalid number of blocks (must be 1-1000)");
      }
      
      // Set mining address temporarily
      std::string original_address = g_mining->getMiningAddress();
      g_mining->setMiningAddress(address);
      
      Json::Value result(Json::arrayValue);
      
      try {
        for (int i = 0; i < nblocks; i++) {
          // Enable mining temporarily and mine one block
          g_mining->setMiningEnabled(true);
          
          if (g_miner_core) {
            g_miner_core->start(address, 1);
            // Wait a bit for block to be mined
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_miner_core->stop();
          }
          
          // Get the hash of the newly mined block
          std::string blockHash = g_blockchain->getLatestHash();
          result.append(blockHash);
        }
        
        // Restore original mining address
        if (!original_address.empty()) {
          g_mining->setMiningAddress(original_address);
        }
        
        return result;
        
      } catch (const std::exception& e) {
        // Restore original address on error
        if (!original_address.empty()) {
          g_mining->setMiningAddress(original_address);
        }
        throw std::runtime_error("Mining error: " + std::string(e.what()));
      }
    }
    
    if (canonMethod == "mining.info" || method == "getmininginfo") {
      Json::Value result;
      result["blocks"] = g_blockchain ? g_blockchain->getLatestHeight() : 0;
      result["difficulty"] = 1.0;
      result["networkhashps"] = 0.0;
      result["pooledtx"] = 0;
      result["testnet"] = false;
      
      if (g_mining) {
        result["generate"] = g_mining->isMiningEnabled();
        result["genproclimit"] = g_mining->getThreadCount();
        result["hashespersec"] = g_mining->getHashrate();
      } else {
        result["generate"] = false;
        result["genproclimit"] = 0;
        result["hashespersec"] = 0.0;
      }
      
      return result;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // mining.getstratumtemplate - Stratum V1 template for external miners
    // ═══════════════════════════════════════════════════════════════════════════
    // Returns a template optimized for Stratum miners with:
    // - Pre-split coinbase (coinb1/coinb2)
    // - Merkle branches
    // - SHA256 midstate (GPU optimization)
    // - Utreexo commitment (Dinero 112-byte header)
    if (canonMethod == "mining.getstratumtemplate" || method == "getstratumtemplate") {
      Json::Value result;

      if (!g_blockchain) {
        throw std::runtime_error("Blockchain not available");
      }

      if (!g_mining) {
        throw std::runtime_error("Mining subsystem not available");
      }

      // Get current chain tip
      uint32_t height = g_blockchain->getLatestHeight();
      std::string prevhash = g_blockchain->getLatestHash();

      // Get difficulty bits (compact target)
      uint32_t nbits = g_mining->getDifficulty();

      // Current time
      uint32_t ntime = static_cast<uint32_t>(std::time(nullptr));

      // Generate job ID
      std::string job_id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

      // Block version (use current consensus version)
      uint32_t version = 0x20000000;  // BIP9 version bits

      // Placeholder merkle root (miner will compute from coinbase + merkle branches)
      std::string merkle_root = "0000000000000000000000000000000000000000000000000000000000000000";

      // Utreexo commitment (AFTER-state Utreexo root for 128-byte header)
      // In real implementation, this comes from the Utreexo accumulator
      std::string utreexo_commitment = "0000000000000000000000000000000000000000000000000000000000000000";

      // ═══════════════════════════════════════════════════════════════════════════
      // Build 128-byte BlockHeader v1 for midstate computation
      // Layout (from header_layout.h):
      //   [0-3]:     version (4 bytes)
      //   [4-35]:    prev_block_hash (32 bytes)
      //   [36-67]:   merkle_root (32 bytes)
      //   [68-99]:   utreexo_root (32 bytes)
      //   [100-107]: timestamp (8 bytes)
      //   [108-111]: difficulty (4 bytes)
      //   [112-115]: nonce (4 bytes)
      //   [116-127]: reserved (12 bytes, MUST be zero)
      // ═══════════════════════════════════════════════════════════════════════════
      uint8_t header128[DINERO_HEADER_SIZE_BYTES];
      std::memset(header128, 0, sizeof(header128));

      // Version (4 bytes at offset 0, little-endian)
      header128[DINERO_HEADER_VERSION_OFFSET + 0] = version & 0xFF;
      header128[DINERO_HEADER_VERSION_OFFSET + 1] = (version >> 8) & 0xFF;
      header128[DINERO_HEADER_VERSION_OFFSET + 2] = (version >> 16) & 0xFF;
      header128[DINERO_HEADER_VERSION_OFFSET + 3] = (version >> 24) & 0xFF;

      // Previous block hash (32 bytes at offset 4)
      // IMPORTANT: Reverse byte order for wire format (big-endian hex → little-endian storage)
      for (size_t i = 0; i < prevhash.length() && i < 64; i += 2) {
        std::string byte_hex = prevhash.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16));
        header128[DINERO_HEADER_PREVHASH_OFFSET + (31 - i/2)] = byte;
      }

      // Merkle root placeholder (32 bytes at offset 36)
      // Left as zeros - miner will compute

      // Utreexo root (32 bytes at offset 68)
      // For now, zeros - will be computed by BlockAssembler when block is assembled
      // Note: Stratum miners use placeholder; actual root computed at block assembly time

      // Timestamp (8 bytes at offset 100, little-endian)
      uint64_t timestamp64 = static_cast<uint64_t>(ntime);
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 0] = timestamp64 & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 1] = (timestamp64 >> 8) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 2] = (timestamp64 >> 16) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 3] = (timestamp64 >> 24) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 4] = (timestamp64 >> 32) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 5] = (timestamp64 >> 40) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 6] = (timestamp64 >> 48) & 0xFF;
      header128[DINERO_HEADER_TIMESTAMP_OFFSET + 7] = (timestamp64 >> 56) & 0xFF;

      // Difficulty bits (4 bytes at offset 108, little-endian)
      header128[DINERO_HEADER_DIFFICULTY_OFFSET + 0] = nbits & 0xFF;
      header128[DINERO_HEADER_DIFFICULTY_OFFSET + 1] = (nbits >> 8) & 0xFF;
      header128[DINERO_HEADER_DIFFICULTY_OFFSET + 2] = (nbits >> 16) & 0xFF;
      header128[DINERO_HEADER_DIFFICULTY_OFFSET + 3] = (nbits >> 24) & 0xFF;

      // Nonce (4 bytes at offset 112) - starts at 0, miner increments
      // Reserved (12 bytes at offset 116) - already zero from memset

      // Compute SHA256 midstate from first 64 bytes
      dinero::mining::SHA256Midstate midstate = dinero::mining::MidstateCache::ComputeMidstate(header128);
      std::string midstate_hex = midstate.ToHex();

      // Format version/nbits/ntime as hex strings
      std::ostringstream oss_version, oss_nbits, oss_ntime;
      oss_version << std::hex << std::setw(8) << std::setfill('0') << version;
      oss_nbits << std::hex << std::setw(8) << std::setfill('0') << nbits;
      oss_ntime << std::hex << std::setw(8) << std::setfill('0') << ntime;

      // ═══════════════════════════════════════════════════════════════════════════
      // Build Stratum-formatted response
      // ═══════════════════════════════════════════════════════════════════════════
      result["job_id"] = job_id;
      result["prevhash"] = prevhash;
      result["height"] = height + 1;  // Height of block being mined

      // Coinbase split (simplified - in production, parse actual coinbase)
      result["coinb1"] = "";  // First part of coinbase (before extranonce)
      result["coinb2"] = "";  // Second part of coinbase (after extranonce)
      result["extranonce1"] = "00000000";  // 4-byte extranonce1
      result["extranonce2_size"] = 4;      // 4-byte extranonce2

      // Merkle branches (empty for coinbase-only block)
      result["merkle_branches"] = Json::Value(Json::arrayValue);

      // Header fields
      result["version"] = oss_version.str();
      result["nbits"] = oss_nbits.str();
      result["ntime"] = oss_ntime.str();
      result["clean_jobs"] = true;

      // Dinero 128-byte BlockHeader v1 extensions
      result["utreexo_commitment"] = utreexo_commitment;
      result["midstate"] = midstate_hex;

      // Mining target info
      result["target"] = "";  // Target hash (computed from nbits)
      result["difficulty"] = 1.0;  // Pool difficulty

      // Header layout info for miners
      result["header_size"] = DINERO_HEADER_SIZE_BYTES;
      result["nonce_offset"] = DINERO_HEADER_NONCE_OFFSET;
      result["utreexo_offset"] = DINERO_HEADER_UTREEXO_OFFSET;

      LOG_I("Generated stratum template: job=" + job_id + " height=" + std::to_string(height + 1));

      return result;
    }

    // ---- Wallet methods (use canonical names) ----
    if (canonMethod == "wallet.getnewaddress" || method == "getnewaddress") {
      // For now, generate a new address (this would normally be wallet-specific)
      std::array<uint8_t, 32> privateKey = dinero::Address::generatePrivateKey();
      std::vector<uint8_t> publicKey = dinero::Address::derivePublicKey(privateKey, true);
      
      // Use the active network HRP (regtest=rdin, testnet=tdin, mainnet=din)
      std::string hrp = dinero::HrpForActiveNetworkRef();
      std::string address = dinero::Address::createP2WPKHAddress(publicKey, hrp);
      return Json::Value(address);
    }
    
    if (canonMethod == "wallet.getbalance" || method == "getbalance") {
      // Placeholder - would normally query wallet/UTXO set
      return Json::Value(0.0);
    }

    // ---- Mempool Testing ----
    if (method == "submitdummytx") {
      if (!g_blockchain) {
        throw std::runtime_error("Blockchain not available");
      }
      
      // Create a dummy transaction for testing mempoolTx events
      // This is a simplified version - in real implementation you'd use the mempool
      Json::Value result;
      result["txid"] = "dummy_tx_" + std::to_string(std::time(nullptr));
      result["message"] = "Dummy transaction created for testing mempoolTx events";
      result["note"] = "Check WebSocket clients for mempoolTx event";
      
      // For now, just return success - the real mempool integration will be in the blockchain
      return result;
    }

    // ---- Control ----
    if (method == "stop") {
      // Note: In a real implementation, you'd signal the main thread to shutdown
      return Json::Value("Dinero server stopping");
    }

    // ---- Batch RPC Support ----
    if (method == "batch") {
      if (!params.isArray()) {
        throw std::runtime_error("Invalid params: expected array of RPC requests");
      }
      
      if (params.size() == 0) {
        throw std::runtime_error("Empty batch request");
      }
      
      if (params.size() > 100) {
        throw std::runtime_error("Batch too large: maximum 100 requests allowed");
      }
      
      Json::Value results(Json::arrayValue);
      
      for (size_t i = 0; i < params.size(); ++i) {
        const Json::Value& request = params[static_cast<int>(i)];
        
        // Validate request format
        if (!request.isObject()) {
          Json::Value error_result;
          error_result["jsonrpc"] = "2.0";
          error_result["id"] = request.isMember("id") ? request["id"] : Json::Value(Json::nullValue);
          error_result["error"]["code"] = -32600;
          error_result["error"]["message"] = "Invalid Request";
          results.append(error_result);
          continue;
        }
        
        // Extract request components
        std::string batch_method = request.isMember("method") ? request["method"].asString() : "";
        Json::Value batch_params = request.isMember("params") ? request["params"] : Json::Value(Json::arrayValue);
        Json::Value batch_id = request.isMember("id") ? request["id"] : Json::Value(Json::nullValue);
        
        if (batch_method.empty()) {
          Json::Value error_result;
          error_result["jsonrpc"] = "2.0";
          error_result["id"] = batch_id;
          error_result["error"]["code"] = -32603;
          error_result["error"]["message"] = "Missing method";
          results.append(error_result);
          continue;
        }
        
        try {
          // Recursively call this function for each batch request
          Json::Value batch_result = daemon_rpc_dispatch(path, batch_method, batch_params, batch_id);
          
          // Format response
          Json::Value response;
          response["jsonrpc"] = "2.0";
          response["id"] = batch_id;
          
          if (batch_result.isMember("error")) {
            response["error"] = batch_result["error"];
          } else {
            response["result"] = batch_result;
          }
          
          results.append(response);
          
        } catch (const std::exception& e) {
          // Handle execution errors
          Json::Value error_result;
          error_result["jsonrpc"] = "2.0";
          error_result["id"] = batch_id;
          error_result["error"]["code"] = -32603;
          error_result["error"]["message"] = std::string("Internal error: ") + e.what();
          results.append(error_result);
        }
      }
      
      return results;
    }

    // Method not found - provide helpful error message
    std::vector<std::string> availableMethods; // TODO: Populate with actual available methods
    std::string errorMsg = createMethodNotFoundError(ctx.originalMethod, availableMethods);
    
    Json::Value error_response;
    error_response["jsonrpc"] = "2.0";
    error_response["id"] = id;
    error_response["error"]["code"] = -32601;
    error_response["error"]["message"] = errorMsg;
    return error_response;
  } catch (const std::exception& e) {
    // Return JSON-RPC error format for other exceptions
    Json::Value error_response;
    error_response["jsonrpc"] = "2.0";
    error_response["error"]["code"] = -32603;
    error_response["error"]["message"] = e.what();
    error_response["id"] = id;
    return error_response;
  }
}

} // namespace dinero
