#include "cli/commands.h"
#include "common/rpc_client.h"
#include "common/utils.h"
#include "common/logger.h"
#include "common/json_utils.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include "compat/jsoncpp_compat.h"

namespace dinero {

void registerCommands(CLI::App& app, Dinero::Common::RPCClient& rpc_client) {
    // Ping command
    auto ping_cmd = app.add_subcommand("ping", "Test RPC connection");
    ping_cmd->callback([&rpc_client]() {
        executePing(rpc_client);
    });
    
    // Self-test command
    auto selftest_cmd = app.add_subcommand("selftest", "Run comprehensive self-test");
    selftest_cmd->callback([&rpc_client]() {
        executeSelfTest(rpc_client);
    });
    
    // Get blockchain info command
    auto getinfo_cmd = app.add_subcommand("getinfo", "Get blockchain information");
    getinfo_cmd->callback([&rpc_client]() {
        executeGetInfo(rpc_client);
    });
    
    // Get mining info command
    auto getmininginfo_cmd = app.add_subcommand("getmininginfo", "Get mining information");
    getmininginfo_cmd->callback([&rpc_client]() {
        executeGetMiningInfo(rpc_client);
    });
    
    // Get network info command
    auto getnetworkinfo_cmd = app.add_subcommand("getnetworkinfo", "Get network information");
    getnetworkinfo_cmd->callback([&rpc_client]() {
        executeGetNetworkInfo(rpc_client);
    });
    
    // Get peer info command
    auto getpeerinfo_cmd = app.add_subcommand("getpeerinfo", "Get peer information");
    getpeerinfo_cmd->callback([&rpc_client]() {
        executeGetPeerInfo(rpc_client);
    });
    
    // Get mempool info command
    auto getmempoolinfo_cmd = app.add_subcommand("getmempoolinfo", "Get mempool information");
    getmempoolinfo_cmd->callback([&rpc_client]() {
        executeGetMempoolInfo(rpc_client);
    });
    
    // Get block command
    auto getblock_cmd = app.add_subcommand("getblock", "Get block information");
    std::string block_hash;
    getblock_cmd->add_option("hash", block_hash, "Block hash")->required();
    getblock_cmd->callback([&rpc_client, &block_hash]() {
        executeGetBlock(rpc_client, block_hash);
    });
    
    // Get transaction command
    auto gettx_cmd = app.add_subcommand("gettx", "Get transaction information");
    std::string tx_hash;
    gettx_cmd->add_option("hash", tx_hash, "Transaction hash")->required();
    gettx_cmd->callback([&rpc_client, &tx_hash]() {
        executeGetTransaction(rpc_client, tx_hash);
    });
    
    // Get balance command
    auto getbalance_cmd = app.add_subcommand("getbalance", "Get wallet balance");
    getbalance_cmd->callback([&rpc_client]() {
        executeGetBalance(rpc_client);
    });
    
    // Send to address command
    auto sendtoaddress_cmd = app.add_subcommand("sendtoaddress", "Send coins to address");
    std::string address;
    double amount;
    sendtoaddress_cmd->add_option("address", address, "Destination address")->required();
    sendtoaddress_cmd->add_option("amount", amount, "Amount to send")->required();
    sendtoaddress_cmd->callback([&rpc_client, &address, &amount]() {
        executeSendToAddress(rpc_client, address, amount);
    });
    
    // Mining commands
    auto setgenerate_cmd = app.add_subcommand("setgenerate", "Set mining generation");
    bool generate;
    int genproclimit = 1;
    setgenerate_cmd->add_option("generate", generate, "Enable/disable mining")->required();
    setgenerate_cmd->add_option("genproclimit", genproclimit, "Number of processors");
    setgenerate_cmd->callback([&rpc_client, &generate, &genproclimit]() {
        executeSetGenerate(rpc_client, generate, genproclimit);
    });
    
    auto getgenerate_cmd = app.add_subcommand("getgenerate", "Get mining generation status");
    getgenerate_cmd->callback([&rpc_client]() {
        executeGetGenerate(rpc_client);
    });
    
    auto getblocktemplate_cmd = app.add_subcommand("getblocktemplate", "Get block template for mining");
    getblocktemplate_cmd->callback([&rpc_client]() {
        executeGetBlockTemplate(rpc_client);
    });
    
    auto submitblock_cmd = app.add_subcommand("submitblock", "Submit mined block");
    std::string block_data;
    submitblock_cmd->add_option("blockdata", block_data, "Block data")->required();
    submitblock_cmd->callback([&rpc_client, &block_data]() {
        executeSubmitBlock(rpc_client, block_data);
    });
}

void executePing(Dinero::Common::RPCClient& rpc_client) {
    std::cout << "🔍 Testing RPC connection..." << std::endl;
    
    Json::Value response = rpc_client.call("core.ping", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ RPC connection: ❌ Failed" << std::endl;
        std::cout << "   Error: " << getRpcErrorMessage(response) << std::endl;
        std::cout << "   Code: " << getRpcErrorCode(response) << std::endl;
    } else {
        std::cout << "✅ RPC connection: ✅ Success" << std::endl;
        std::cout << "   Response: " << toJsonString(response["result"], true) << std::endl;
    }
}

void executeSelfTest(Dinero::Common::RPCClient& rpc_client) {
    std::cout << "🧪 Running comprehensive self-test..." << std::endl;
    
    // Test 1: RPC Connection
    std::cout << "\n1. Testing RPC Connection..." << std::endl;
    Json::Value ping_response = rpc_client.call("core.ping", Json::Value());
    if (isRpcError(ping_response)) {
        std::cout << "   ❌ RPC connection failed" << std::endl;
        return;
    } else {
        std::cout << "   ✅ RPC connection successful" << std::endl;
    }
    
    // Test 2: Blockchain Info
    std::cout << "\n2. Testing Blockchain Info..." << std::endl;
    Json::Value info_response = rpc_client.call("blockchain.getinfo", Json::Value());
    if (isRpcError(info_response)) {
        std::cout << "   ❌ Failed to get blockchain info" << std::endl;
    } else {
        std::cout << "   ✅ Blockchain info retrieved" << std::endl;
        Json::Value result = info_response["result"];
        std::cout << "   Chain: " << getStringField(result, "chain", "unknown") << std::endl;
        std::cout << "   Blocks: " << getIntField(result, "blocks", 0) << std::endl;
        std::cout << "   Headers: " << getIntField(result, "headers", 0) << std::endl;
    }
    
    // Test 3: Mining Info
    std::cout << "\n3. Testing Mining Info..." << std::endl;
    Json::Value mining_response = rpc_client.call("blockchain.getmininginfo", Json::Value());
    if (isRpcError(mining_response)) {
        std::cout << "   ❌ Failed to get mining info" << std::endl;
    } else {
        std::cout << "   ✅ Mining info retrieved" << std::endl;
        Json::Value result = mining_response["result"];
        std::cout << "   Generating: " << (getBoolField(result, "generate", false) ? "Yes" : "No") << std::endl;
        std::cout << "   Hash Rate: " << getIntField(result, "hashrate", 0) << " H/s" << std::endl;
    }
    
    // Test 4: Network Info
    std::cout << "\n4. Testing Network Info..." << std::endl;
    Json::Value network_response = rpc_client.call("network.getinfo", Json::Value());
    if (isRpcError(network_response)) {
        std::cout << "   ❌ Failed to get network info" << std::endl;
    } else {
        std::cout << "   ✅ Network info retrieved" << std::endl;
        Json::Value result = network_response["result"];
        std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
        std::cout << "   Connections: " << getIntField(result, "connections", 0) << std::endl;
    }
    
    std::cout << "\n🎉 Self-test completed!" << std::endl;
}

void executeGetInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("blockchain.getinfo", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get blockchain info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "📊 Blockchain Information:" << std::endl;
    std::cout << "   Chain: " << getStringField(result, "chain", "unknown") << std::endl;
    std::cout << "   Blocks: " << getIntField(result, "blocks", 0) << std::endl;
    std::cout << "   Headers: " << getIntField(result, "headers", 0) << std::endl;
    std::cout << "   Best Block Hash: " << getStringField(result, "bestblockhash", "unknown") << std::endl;
    std::cout << "   Difficulty: " << getStringField(result, "difficulty", "unknown") << std::endl;
    std::cout << "   Verification Progress: " << getStringField(result, "verificationprogress", "unknown") << std::endl;
}

void executeGetMiningInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("blockchain.getmininginfo", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mining info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "⛏️  Mining Information:" << std::endl;
    std::cout << "   Generating: " << (getBoolField(result, "generate", false) ? "Yes" : "No") << std::endl;
    std::cout << "   Hash Rate: " << getIntField(result, "hashrate", 0) << " H/s" << std::endl;
    std::cout << "   Pooled Transactions: " << getIntField(result, "pooledtx", 0) << std::endl;
    std::cout << "   Difficulty: " << getStringField(result, "difficulty", "unknown") << std::endl;
}

void executeGetNetworkInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("network.getinfo", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get network info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "🌐 Network Information:" << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
    std::cout << "   Subversion: " << getStringField(result, "subversion", "unknown") << std::endl;
    std::cout << "   Connections: " << getIntField(result, "connections", 0) << std::endl;
    std::cout << "   Network Active: " << (getBoolField(result, "networkactive", false) ? "Yes" : "No") << std::endl;
}

void executeGetPeerInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("network.getpeerinfo", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get peer info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "👥 Peer Information (" << result.size() << " peers):" << std::endl;
    
    for (const auto& peer : result) {
        std::cout << "   Address: " << getStringField(peer, "addr", "unknown") << std::endl;
        std::cout << "   Services: " << getStringField(peer, "services", "unknown") << std::endl;
        std::cout << "   Last Send: " << getIntField(peer, "lastsend", 0) << std::endl;
        std::cout << "   Last Recv: " << getIntField(peer, "lastrecv", 0) << std::endl;
        std::cout << "   Bytes Sent: " << getIntField(peer, "bytessent", 0) << std::endl;
        std::cout << "   Bytes Recv: " << getIntField(peer, "bytesrecv", 0) << std::endl;
        std::cout << "   ---" << std::endl;
    }
}

void executeGetMempoolInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("mempool.getinfo", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mempool info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "📋 Mempool Information:" << std::endl;
    std::cout << "   Size: " << getIntField(result, "size", 0) << " transactions" << std::endl;
    std::cout << "   Bytes: " << getIntField(result, "bytes", 0) << " bytes" << std::endl;
    std::cout << "   Usage: " << getIntField(result, "usage", 0) << " bytes" << std::endl;
    std::cout << "   Max Mem Pool: " << getIntField(result, "maxmempool", 0) << " bytes" << std::endl;
}

void executeGetBlock(Dinero::Common::RPCClient& rpc_client, const std::string& block_hash) {
    Json::Value params;
    params.append(block_hash);
    params.push_back(2); // verbosity level
    
    Json::Value response = rpc_client.call("blockchain.getblock", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "📦 Block Information:" << std::endl;
    std::cout << "   Hash: " << getStringField(result, "hash", "unknown") << std::endl;
    std::cout << "   Height: " << getIntField(result, "height", 0) << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
    std::cout << "   Time: " << getIntField(result, "time", 0) << std::endl;
    std::cout << "   Nonce: " << getIntField(result, "nonce", 0) << std::endl;
    std::cout << "   Difficulty: " << getStringField(result, "difficulty", "unknown") << std::endl;
    std::cout << "   Transactions: " << getIntField(result, "nTx", 0) << std::endl;
}

void executeGetTransaction(Dinero::Common::RPCClient& rpc_client, const std::string& tx_hash) {
    Json::Value params;
    params.append(tx_hash);
    params.append(true); // verbose
    
    Json::Value response = rpc_client.call("wallet.getrawtransaction", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get transaction: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "💸 Transaction Information:" << std::endl;
    std::cout << "   Hash: " << getStringField(result, "hash", "unknown") << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
    std::cout << "   Size: " << getIntField(result, "size", 0) << " bytes" << std::endl;
    std::cout << "   VSize: " << getIntField(result, "vsize", 0) << " bytes" << std::endl;
    std::cout << "   Weight: " << getIntField(result, "weight", 0) << std::endl;
    std::cout << "   Locktime: " << getIntField(result, "locktime", 0) << std::endl;
    std::cout << "   Confirmations: " << getIntField(result, "confirmations", 0) << std::endl;
}

void executeGetBalance(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("wallet.getbalance", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get balance: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    double balance = response["result"].asDouble();
    std::cout << "💰 Wallet Balance: " << balance << " DIN" << std::endl;
}

void executeSendToAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address, double amount) {
    Json::Value params;
    params.append(address);
    params.append(amount);
    
    Json::Value response = rpc_client.call("wallet.sendtoaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to send transaction: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string txid = response["result"].asString();
    std::cout << "✅ Transaction sent successfully!" << std::endl;
    std::cout << "   Transaction ID: " << txid << std::endl;
}

void executeSetGenerate(Dinero::Common::RPCClient& rpc_client, bool generate, int genproclimit) {
    Json::Value params;
    params.append(generate);
    params.append(genproclimit);
    
    Json::Value response = rpc_client.call("mining.setgenerate", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to set mining: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::cout << "✅ Mining " << (generate ? "started" : "stopped") << " successfully!" << std::endl;
    if (generate) {
        std::cout << "   Using " << genproclimit << " processor(s)" << std::endl;
    }
}

void executeGetGenerate(Dinero::Common::RPCClient& rpc_client) {
    Json::Value response = rpc_client.call("mining.getgenerate", Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mining status: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    bool generating = response["result"].asBool();
    std::cout << "⛏️  Mining Status: " << (generating ? "Active" : "Inactive") << std::endl;
}

void executeGetBlockTemplate(Dinero::Common::RPCClient& rpc_client) {
    Json::Value params;
    params.append(Json::Value(Json::objectValue)); // template request
    
    Json::Value response = rpc_client.call("mining.gettemplate", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block template: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Value result = response["result"];
    std::cout << "📋 Block Template:" << std::endl;
    std::cout << "   Previous Block Hash: " << getStringField(result, "previousblockhash", "unknown") << std::endl;
    std::cout << "   Coinbase Json::Value: " << getIntField(result, "coinbasevalue", 0) << std::endl;
    std::cout << "   Bits: " << getStringField(result, "bits", "unknown") << std::endl;
    std::cout << "   Height: " << getIntField(result, "height", 0) << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
}

void executeSubmitBlock(Dinero::Common::RPCClient& rpc_client, const std::string& block_data) {
    Json::Value params;
    params.append(block_data);
    
    Json::Value response = rpc_client.call("blockchain.submitblock", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to submit block: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string result = response["result"].asString();
    if (result.empty()) {
        std::cout << "✅ Block submitted successfully!" << std::endl;
    } else {
        std::cout << "⚠️  Block submission result: " << result << std::endl;
    }
}

} // namespace dinero 