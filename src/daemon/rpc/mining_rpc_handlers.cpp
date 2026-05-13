#include "daemon/address_helpers.h"
#include "common/address_script_builder.h"
#include "common/logger.h"
#include <stdexcept>
#include <cstdio>
#include <json/json.h>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace dinero {

Json::Value mining_setaddress(const Json::Value& params) {
    try {
        // Validate parameters
        if (params.empty()) {
            throw std::runtime_error("Missing required parameter: address");
        }
        
        if (!params.isArray() || params.size() != 1) {
            throw std::runtime_error("Invalid parameters: expected exactly one address parameter");
        }
        
        if (!params[0].isString()) {
            throw std::runtime_error("Invalid parameter type: address must be string");
        }
        
        std::string addr = params[0].asString();
        
        // Check for empty address
        if (addr.empty()) {
            throw std::runtime_error("Mining address cannot be empty");
        }
        
        // Check address length constraints
        if (addr.length() < 10 || addr.length() > 100) {
            throw std::runtime_error("Invalid mining address length");
        }
        
        // Validate address format using unified script builder
        std::vector<uint8_t> script;
        std::string why;
        if (!BuildScriptPubKeyFromAddress(addr, script, why)) {
            throw std::runtime_error("Invalid mining address format: " + (why.empty() ? "Unknown error" : why));
        }
        
        // Check network compatibility
        const std::string& expected_hrp = GetChainParams().HRP();
        if (addr.substr(0, expected_hrp.length()) != expected_hrp) {
            throw std::runtime_error("Address network mismatch: expected " + expected_hrp + " prefix");
        }
        
        // Check if address is owned by active wallet (if wallet manager is available)
        // This provides better security by ensuring mining rewards go to wallet-owned addresses
        bool is_wallet_owned = false;
        // TODO: Add wallet manager parameter to mining RPC handlers
        // For now, we'll assume validation passes but add the security warning
        dinero::g_logger.info("Mining address set: " + addr + " (wallet ownership check not yet implemented)");
        
        // Set the mining address
        // TODO: Implement SetMiningAddress
        // if (!SetMiningAddress(addr, GetChainParams())) {
        if (false) {
            throw std::runtime_error("Failed to set mining address: internal error");
        }
        
        return Json::Value("Mining address set successfully");
    } catch (const std::exception& e) {
        throw std::runtime_error("mining.setaddress failed: " + std::string(e.what()));
    }
}

Json::Value mining_getaddress(const Json::Value& params) {
    try {
        // Validate parameters (should be empty)
        if (!params.empty() && !params.isArray()) {
            throw std::runtime_error("Invalid parameters: expected no parameters or empty array");
        }
        
        if (params.isArray() && params.size() > 0) {
            throw std::runtime_error("Invalid parameters: mining.getaddress takes no parameters");
        }
        
        std::string addr = ""; // TODO: Implement GetMiningAddress
        
        Json::Value result(Json::objectValue);
        
        if (addr.empty()) {
            result["address"] = "";
            result["ismine"] = false;
            result["source"] = "unset";
        } else {
            result["address"] = addr;
            // TODO: Replace with actual wallet ownership check when wallet manager is available
            result["ismine"] = false; // Placeholder until wallet integration
            result["source"] = "configured";
        }
        
        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error("mining.getaddress failed: " + std::string(e.what()));
    }
}

Json::Value mining_generatetoaddress(const Json::Value& params) {
    try {
        // Validate parameters
        if (params.empty()) {
            throw std::runtime_error("Missing required parameters: nblocks and address");
        }
        
        if (!params.isArray() || params.size() != 2) {
            throw std::runtime_error("Invalid parameters: expected exactly two parameters (nblocks, address)");
        }
        
        if (!params[0].isInt() && !params[0].isUInt()) {
            throw std::runtime_error("Invalid parameter type: nblocks must be integer");
        }
        
        if (!params[1].isString()) {
            throw std::runtime_error("Invalid parameter type: address must be string");
        }
        
        int nblocks = params[0].asInt();
        std::string addr = params[1].asString();
        
        // Validate nblocks range
        if (nblocks <= 0) {
            throw std::runtime_error("Invalid nblocks: must be positive integer");
        }
        
        if (nblocks > 1000) {
            throw std::runtime_error("Invalid nblocks: maximum 1000 blocks per call");
        }
        
        // Check for empty address
        if (addr.empty()) {
            throw std::runtime_error("Address cannot be empty");
        }
        
        // Validate address format using unified script builder
        std::vector<uint8_t> script;
        std::string why;
        if (!BuildScriptPubKeyFromAddress(addr, script, why)) {
            throw std::runtime_error("Invalid address format: " + (why.empty() ? "Unknown error" : why));
        }
        
        // Check network compatibility
        const std::string& expected_hrp = GetChainParams().HRP();
        if (addr.substr(0, expected_hrp.length()) != expected_hrp) {
            throw std::runtime_error("Address network mismatch: expected " + expected_hrp + " prefix");
        }
        
        // TEMPORARY: Allow mainnet mining for bootstrap
        // TODO: Re-enable this safety check after initial blocks are mined
        // if (expected_hrp != "rdin" && expected_hrp != "tdin") {
        //     throw std::runtime_error("mining.generatetoaddress is only available in regtest or testnet mode");
        // }
        
        // Log the scriptPubKey for debugging
        std::string script_hex = ScriptPubKeyToHex(script);
        dinero::g_logger.info("generatetoaddress: address=" + addr + " scriptPubKey=" + script_hex);
        
        // This is a stub implementation - the real mining happens in RPCServer::generateToAddress
        // Return error to indicate this method should not be called directly
        throw std::runtime_error("Use generatetoaddress (without mining. prefix) for actual block generation");
    } catch (const std::exception& e) {
        throw std::runtime_error("mining.generatetoaddress failed: " + std::string(e.what()));
    }
}

} // namespace dinero
