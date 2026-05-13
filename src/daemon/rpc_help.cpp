#include "daemon/rpc_help.h"
#include "compat/jsoncpp_compat.h"

namespace dinero {

Json::Value GetRpcHelp() {
    Json::Value help(Json::objectValue);
    
    // Wallet methods
    Json::Value wallet_methods(Json::objectValue);
    
    wallet_methods["wallet.getnewaddress"] = Json::Value(Json::objectValue);
    wallet_methods["wallet.getnewaddress"]["description"] = "Generate a new HD wallet address";
    wallet_methods["wallet.getnewaddress"]["params"] = Json::Value(Json::arrayValue);
    wallet_methods["wallet.getnewaddress"]["returns"] = "string - New Bech32 address";
    wallet_methods["wallet.getnewaddress"]["example"] = "wallet.getnewaddress";
    
    wallet_methods["wallet.validateaddress"] = Json::Value(Json::objectValue);
    wallet_methods["wallet.validateaddress"]["description"] = "Validate address and check wallet ownership";
    wallet_methods["wallet.validateaddress"]["params"] = Json::Value(Json::arrayValue);
    wallet_methods["wallet.validateaddress"]["params"].append("address (string, required) - Address to validate");
    wallet_methods["wallet.validateaddress"]["returns"] = "object - Validation result with isvalid, ismine, type, etc.";
    wallet_methods["wallet.validateaddress"]["example"] = "wallet.validateaddress \"rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh\"";
    
    // Mining methods
    Json::Value mining_methods(Json::objectValue);
    
    mining_methods["mining.setaddress"] = Json::Value(Json::objectValue);
    mining_methods["mining.setaddress"]["description"] = "Set mining payout address (must be wallet-owned)";
    mining_methods["mining.setaddress"]["params"] = Json::Value(Json::arrayValue);
    mining_methods["mining.setaddress"]["params"].append("address (string, required) - Wallet-owned address for mining payouts");
    mining_methods["mining.setaddress"]["returns"] = "string - Success message";
    mining_methods["mining.setaddress"]["example"] = "mining.setaddress \"rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh\"";
    mining_methods["mining.setaddress"]["security"] = "Address must be owned by active wallet or mining will be disabled";
    
    mining_methods["mining.getaddress"] = Json::Value(Json::objectValue);
    mining_methods["mining.getaddress"]["description"] = "Get current mining payout address";
    mining_methods["mining.getaddress"]["params"] = Json::Value(Json::arrayValue);
    mining_methods["mining.getaddress"]["returns"] = "object - Mining address info with address, ismine, source";
    mining_methods["mining.getaddress"]["example"] = "mining.getaddress";
    
    mining_methods["mining.generatetoaddress"] = Json::Value(Json::objectValue);
    mining_methods["mining.generatetoaddress"]["description"] = "Generate blocks to specified address (regtest only)";
    mining_methods["mining.generatetoaddress"]["params"] = Json::Value(Json::arrayValue);
    mining_methods["mining.generatetoaddress"]["params"].append("nblocks (integer, required) - Number of blocks to generate (1-1000)");
    mining_methods["mining.generatetoaddress"]["params"].append("address (string, required) - Address to receive block rewards");
    mining_methods["mining.generatetoaddress"]["returns"] = "array - Array of generated block hashes";
    mining_methods["mining.generatetoaddress"]["example"] = "mining.generatetoaddress 5 \"rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh\"";
    mining_methods["mining.generatetoaddress"]["restrictions"] = "Only available in regtest mode";
    
    help["wallet"] = wallet_methods;
    help["mining"] = mining_methods;
    
    // Error codes
    Json::Value error_codes(Json::objectValue);
    error_codes["-1"] = "Generic error (see message for details)";
    error_codes["-3"] = "Invalid parameter type";
    error_codes["-8"] = "Invalid parameter value";
    error_codes["-13"] = "Wallet error";
    error_codes["-32"] = "Mining error";
    
    help["error_codes"] = error_codes;
    
    // Network info
    Json::Value networks(Json::objectValue);
    networks["mainnet"]["hrp"] = "din1";
    networks["mainnet"]["rpc_port"] = 20998;
    networks["mainnet"]["ws_port"] = 21001;
    networks["regtest"]["hrp"] = "rdin1";
    networks["regtest"]["rpc_port"] = 20996;
    networks["regtest"]["ws_port"] = 18881;
    
    help["networks"] = networks;
    
    return help;
}

} // namespace dinero
