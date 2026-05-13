#include "cli/commands.h"
#include "common/logger.h"
#include <iostream>
#include <iomanip>

namespace dinero {

// CLI quality-of-life improvements for v0.6.0

void PrintCompactJson(const Json::Json::Value& json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact output
    builder["commentStyle"] = "None";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(json, &std::cout);
    std::cout << std::endl;
}

void PrintEndpointInfo(const NodeInfo& nodeinfo) {
    std::cout << "\n📡 Discovered Endpoints:\n";
    std::cout << "  RPC:       " << nodeinfo.rpc_url << "\n";
    std::cout << "  WebSocket: " << nodeinfo.ws_url << "\n";
    std::cout << "  Cookie:    " << nodeinfo.cookie_path << "\n";
    std::cout << "  Network:   " << nodeinfo.network << "\n";
    if (!nodeinfo.mining_address.empty()) {
        std::cout << "  Mining:    " << nodeinfo.mining_address << "\n";
    }
    std::cout << std::endl;
}

// Alias for getnewaddress -> wallet.getnewaddress
int CommandNewAddress(const std::vector<std::string>& args) {
    std::cout << "💡 Tip: Use 'wallet.getnewaddress' for the canonical method\n";
    
    // Forward to canonical method
    std::vector<std::string> canonical_args = {"wallet.getnewaddress"};
    canonical_args.insert(canonical_args.end(), args.begin() + 1, args.end());
    
    return CommandWalletGetNewAddress(canonical_args);
}

// Enhanced help command with method categories
int CommandHelpEnhanced(const std::vector<std::string>& args) {
    if (args.size() > 1) {
        // Show help for specific method
        return CommandHelp(args);
    }
    
    std::cout << "Dinero CLI v0.6.0 - Available Commands:\n\n";
    
    std::cout << "🏦 Wallet Methods:\n";
    std::cout << "  wallet.create <name> <password>     - Create new wallet\n";
    std::cout << "  wallet.load <name> <password>       - Load existing wallet\n";
    std::cout << "  wallet.getnewaddress               - Generate new address\n";
    std::cout << "  wallet.validateaddress <address>   - Validate address ownership\n";
    std::cout << "  wallet.listaddresses               - List all wallet addresses\n";
    std::cout << "  newaddress                         - Alias for wallet.getnewaddress\n\n";
    
    std::cout << "⛏️  Mining Methods:\n";
    std::cout << "  mining.setaddress <address>        - Set mining payout address\n";
    std::cout << "  mining.getaddress                  - Get current mining address\n";
    std::cout << "  mining.generatetoaddress <n> <addr> - Generate blocks (regtest only)\n\n";
    
    std::cout << "🔗 Network Methods:\n";
    std::cout << "  getbestblockhash                   - Get latest block hash\n";
    std::cout << "  getblockcount                      - Get current block height\n";
    std::cout << "  getnetworkinfo                     - Get network information\n\n";
    
    std::cout << "🛠️  Utility Methods:\n";
    std::cout << "  help [method]                      - Show help for method\n";
    std::cout << "  --print-nodeinfo                   - Show discovered endpoints\n";
    std::cout << "  --compact-json                     - Use compact JSON output\n\n";
    
    std::cout << "💡 Tips:\n";
    std::cout << "  • CLI auto-discovers daemon endpoints from nodeinfo.json\n";
    std::cout << "  • Use --datadir to specify custom data directory\n";
    std::cout << "  • Wallet methods require an active wallet (wallet.load)\n";
    std::cout << "  • Mining address must be wallet-owned for security\n\n";
    
    return 0;
}

// Status command showing comprehensive system state
int CommandStatus(const std::vector<std::string>& args) {
    std::cout << "Dinero System Status\n";
    std::cout << "========================\n\n";
    
    // Try to connect and get basic info
    try {
        NodeInfo nodeinfo = DiscoverNodeInfo();
        PrintEndpointInfo(nodeinfo);
        
        // Test RPC connectivity
        std::cout << "🔌 Testing RPC Connection...\n";
        
        DineroRpcClient client(nodeinfo.rpc_url, nodeinfo.cookie_path);
        
        // Get best block hash
        Json::Json::Value params(Json::Json::Value(Json::arrayJson::Value));
        Json::Json::Value result = client.call("getbestblockhash", params);
        
        if (!result.isNull() && !result["error"].isNull() == false) {
            std::cout << "  ✅ RPC: Connected\n";
            std::cout << "  📦 Latest Block: " << result["result"].asString().substr(0, 16) << "...\n";
        } else {
            std::cout << "  ❌ RPC: Failed to get block info\n";
        }
        
        // Get network info
        result = client.call("getnetworkinfo", params);
        if (!result.isNull() && !result["error"].isNull() == false) {
            Json::Json::Value netinfo = result["result"];
            std::cout << "  🌐 Network: " << netinfo.value("networkid", "unknown") << "\n";
        }
        
        // Get mining info
        result = client.call("mining.getaddress", params);
        if (!result.isNull() && !result["error"].isNull() == false) {
            Json::Json::Value mining = result["result"];
            std::string addr = mining.value("address", "");
            bool ismine = mining.value("ismine", false);
            
            if (!addr.empty()) {
                std::cout << "  ⛏️  Mining: " << (ismine ? "✅ Ready" : "❌ Invalid address") << "\n";
                std::cout << "     Address: " << addr.substr(0, 20) << "...\n";
            } else {
                std::cout << "  ⛏️  Mining: Not configured\n";
            }
        }
        
        std::cout << "\n✅ System operational\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "❌ Connection failed: " << e.what() << "\n";
        std::cout << "\n💡 Make sure dinerod is running with:\n";
        std::cout << "   dinerod -regtest -rpcport=0 -wsport=0 -port=0\n";
        return 1;
    }
}

} // namespace dinero
