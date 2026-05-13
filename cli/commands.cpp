#include "cli/commands.h"
#include "common/rpc_client.h"
#include "common/utils.h"
#include "common/logger.h"
#include "common/json_utils.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include <json/json.h>

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
    
    // Wallet commands
    auto newaddress_cmd = app.add_subcommand("getnewaddress", "Generate a new wallet address");
    newaddress_cmd->callback([&rpc_client]() {
        executeGetNewAddress(rpc_client);
    });
    
    auto validateaddress_cmd = app.add_subcommand("validateaddress", "Validate an address");
    std::string address_to_validate;
    validateaddress_cmd->add_option("address", address_to_validate, "Address to validate")->required();
    validateaddress_cmd->callback([&rpc_client, &address_to_validate]() {
        executeValidateAddress(rpc_client, address_to_validate);
    });
    
    // Mining commands
    auto setminingaddress_cmd = app.add_subcommand("setminingaddress", "Set mining payout address");
    std::string mining_address;
    setminingaddress_cmd->add_option("address", mining_address, "Mining payout address")->required();
    setminingaddress_cmd->callback([&rpc_client, &mining_address]() {
        executeSetMiningAddress(rpc_client, mining_address);
    });
    
    auto getminingaddress_cmd = app.add_subcommand("getminingaddress", "Get current mining payout address");
    getminingaddress_cmd->callback([&rpc_client]() {
        executeGetMiningAddress(rpc_client);
    });
    
    auto generatetoaddress_cmd = app.add_subcommand("generatetoaddress", "Generate blocks to an address (regtest only)");
    int num_blocks = 1;
    std::string generate_address;
    generatetoaddress_cmd->add_option("numblocks", num_blocks, "Number of blocks to generate")->required();
    generatetoaddress_cmd->add_option("address", generate_address, "Address to receive block rewards")->required();
    generatetoaddress_cmd->callback([&rpc_client, &num_blocks, &generate_address]() {
        executeGenerateToAddress(rpc_client, num_blocks, generate_address);
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
    
    // ===== NEW ESSENTIAL COMMANDS =====
    
    // height - Quick blockchain status
    auto height_cmd = app.add_subcommand("height", "Get current block height");
    height_cmd->callback([&rpc_client]() {
        executeHeight(rpc_client);
    });
    
    // getblockhash <n> - Block exploration
    auto getblockhash_cmd = app.add_subcommand("getblockhash", "Get block hash by height");
    int block_height;
    getblockhash_cmd->add_option("height", block_height, "Block height")->required();
    getblockhash_cmd->callback([&rpc_client, &block_height]() {
        executeGetBlockHash(rpc_client, block_height);
    });
    
    // miner start --threads N - Clean mining start
    auto miner_start_cmd = app.add_subcommand("miner", "Mining control commands");
    auto start_subcmd = miner_start_cmd->add_subcommand("start", "Start mining");
    int threads = 4; // default
    start_subcmd->add_option("--threads", threads, "Number of threads (default: 4)");
    start_subcmd->callback([&rpc_client, &threads]() {
        executeMinerStart(rpc_client, threads);
    });
    
    // miner stop - Clean mining stop
    auto stop_subcmd = miner_start_cmd->add_subcommand("stop", "Stop mining");
    stop_subcmd->callback([&rpc_client]() {
        executeMinerStop(rpc_client);
    });
    
    // addr new [--type] - Address generation
    auto addr_cmd = app.add_subcommand("addr", "Address management");
    auto addr_new_cmd = addr_cmd->add_subcommand("new", "Generate new address");
    std::string addr_type;
    addr_new_cmd->add_option("--type", addr_type, "Address type (bech32, p2pkh)");
    addr_new_cmd->callback([&rpc_client, &addr_type]() {
        executeAddrNew(rpc_client, addr_type);
    });
    
    // listunspent - UTXO management
    auto listunspent_cmd = app.add_subcommand("listunspent", "List unspent transaction outputs");
    listunspent_cmd->callback([&rpc_client]() {
        executeListUnspent(rpc_client);
    });
    
    // stop - Daemon control
    auto stop_cmd = app.add_subcommand("stop", "Stop the Dinero daemon");
    stop_cmd->callback([&rpc_client]() {
        executeStop(rpc_client);
    });
}

void executePing(Dinero::Common::RPCClient& rpc_client) {
    std::cout << "🔍 Testing RPC connection..." << std::endl;
    
    Json::Json::Value response = rpc_client.call("ping", Json::Json::Value());
    
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
    Json::Json::Value ping_response = rpc_client.call("ping", Json::Json::Value());
    if (isRpcError(ping_response)) {
        std::cout << "   ❌ RPC connection failed" << std::endl;
        return;
    } else {
        std::cout << "   ✅ RPC connection successful" << std::endl;
    }
    
    // Test 2: Blockchain Info
    std::cout << "\n2. Testing Blockchain Info..." << std::endl;
    Json::Json::Value info_response = rpc_client.call("getblockchaininfo", Json::Json::Value());
    if (isRpcError(info_response)) {
        std::cout << "   ❌ Failed to get blockchain info" << std::endl;
    } else {
        std::cout << "   ✅ Blockchain info retrieved" << std::endl;
        Json::Json::Value result = info_response["result"];
        std::cout << "   Chain: " << getStringField(result, "chain", "unknown") << std::endl;
        std::cout << "   Blocks: " << getIntField(result, "blocks", 0) << std::endl;
        std::cout << "   Headers: " << getIntField(result, "headers", 0) << std::endl;
    }
    
    // Test 3: Mining Info
    std::cout << "\n3. Testing Mining Info..." << std::endl;
    Json::Json::Value mining_response = rpc_client.call("getmininginfo", Json::Json::Value());
    if (isRpcError(mining_response)) {
        std::cout << "   ❌ Failed to get mining info" << std::endl;
    } else {
        std::cout << "   ✅ Mining info retrieved" << std::endl;
        Json::Json::Value result = mining_response["result"];
        std::cout << "   Generating: " << (getBoolField(result, "generate", false) ? "Yes" : "No") << std::endl;
        std::cout << "   Hash Rate: " << getIntField(result, "hashrate", 0) << " H/s" << std::endl;
    }
    
    // Test 4: Network Info
    std::cout << "\n4. Testing Network Info..." << std::endl;
    Json::Json::Value network_response = rpc_client.call("getnetworkinfo", Json::Json::Value());
    if (isRpcError(network_response)) {
        std::cout << "   ❌ Failed to get network info" << std::endl;
    } else {
        std::cout << "   ✅ Network info retrieved" << std::endl;
        Json::Json::Value result = network_response["result"];
        std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
        std::cout << "   Connections: " << getIntField(result, "connections", 0) << std::endl;
    }
    
    std::cout << "\n🎉 Self-test completed!" << std::endl;
}

void executeGetInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("getblockchaininfo", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get blockchain info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::cout << "📊 Blockchain Information:" << std::endl;
    std::cout << "   Chain: " << getStringField(result, "chain", "unknown") << std::endl;
    std::cout << "   Blocks: " << getIntField(result, "blocks", 0) << std::endl;
    std::cout << "   Headers: " << getIntField(result, "headers", 0) << std::endl;
    std::cout << "   Best Block Hash: " << getStringField(result, "bestblockhash", "unknown") << std::endl;
    std::cout << "   Difficulty: " << getStringField(result, "difficulty", "unknown") << std::endl;
    std::cout << "   Verification Progress: " << getStringField(result, "verificationprogress", "unknown") << std::endl;
}

void executeGetMiningInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("getmininginfo", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mining info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::cout << "⛏️  Mining Information:" << std::endl;
    std::cout << "   Generating: " << (getBoolField(result, "generate", false) ? "Yes" : "No") << std::endl;
    std::cout << "   Hash Rate: " << getIntField(result, "hashrate", 0) << " H/s" << std::endl;
    std::cout << "   Pooled Transactions: " << getIntField(result, "pooledtx", 0) << std::endl;
    std::cout << "   Difficulty: " << getStringField(result, "difficulty", "unknown") << std::endl;
}

void executeGetNetworkInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("getnetworkinfo", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get network info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::cout << "🌐 Network Information:" << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
    std::cout << "   Subversion: " << getStringField(result, "subversion", "unknown") << std::endl;
    std::cout << "   Connections: " << getIntField(result, "connections", 0) << std::endl;
    std::cout << "   Network Active: " << (getBoolField(result, "networkactive", false) ? "Yes" : "No") << std::endl;
}

void executeGetPeerInfo(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("getpeerinfo", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get peer info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
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
    Json::Json::Value response = rpc_client.call("getmempoolinfo", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mempool info: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::cout << "📋 Mempool Information:" << std::endl;
    std::cout << "   Size: " << getIntField(result, "size", 0) << " transactions" << std::endl;
    std::cout << "   Bytes: " << getIntField(result, "bytes", 0) << " bytes" << std::endl;
    std::cout << "   Usage: " << getIntField(result, "usage", 0) << " bytes" << std::endl;
    std::cout << "   Max Mem Pool: " << getIntField(result, "maxmempool", 0) << " bytes" << std::endl;
}

void executeGetBlock(Dinero::Common::RPCClient& rpc_client, const std::string& block_hash) {
    Json::Json::Value params;
    params.push_back(block_hash);
    params.push_back(2); // verbosity level
    
    Json::Json::Value response = rpc_client.call("getblock", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
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
    Json::Json::Value params;
    params.push_back(tx_hash);
    params.push_back(true); // verbose
    
    Json::Json::Value response = rpc_client.call("getrawtransaction", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get transaction: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
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
    Json::Json::Value response = rpc_client.call("getbalance", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get balance: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    double balance = response["result"].asDouble();
    std::cout << "💰 Wallet Balance: " << balance << " DIN" << std::endl;
}

void executeSendToAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address, double amount) {
    Json::Json::Value params;
    params.push_back(address);
    params.push_back(amount);
    
    Json::Json::Value response = rpc_client.call("sendtoaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to send transaction: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string txid = response["result"].asString();
    std::cout << "✅ Transaction sent successfully!" << std::endl;
    std::cout << "   Transaction ID: " << txid << std::endl;
}

void executeSetGenerate(Dinero::Common::RPCClient& rpc_client, bool generate, int genproclimit) {
    Json::Json::Value params;
    params.push_back(generate);
    params.push_back(genproclimit);
    
    Json::Json::Value response = rpc_client.call("setgenerate", params);
    
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
    Json::Json::Value response = rpc_client.call("getgenerate", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mining status: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    bool generating = response["result"].asBool();
    std::cout << "⛏️  Mining Status: " << (generating ? "Active" : "Inactive") << std::endl;
}

void executeGetBlockTemplate(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value params;
    params.push_back(Json::Json::Value(Json::objectValue)); // template request
    
    Json::Json::Value response = rpc_client.call("getblocktemplate", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block template: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::cout << "📋 Block Template:" << std::endl;
    std::cout << "   Previous Block Hash: " << getStringField(result, "previousblockhash", "unknown") << std::endl;
    std::cout << "   Coinbase Json::Value: " << getIntField(result, "coinbasevalue", 0) << std::endl;
    std::cout << "   Bits: " << getStringField(result, "bits", "unknown") << std::endl;
    std::cout << "   Height: " << getIntField(result, "height", 0) << std::endl;
    std::cout << "   Version: " << getIntField(result, "version", 0) << std::endl;
}

void executeSubmitBlock(Dinero::Common::RPCClient& rpc_client, const std::string& block_data) {
    Json::Json::Value params;
    params.push_back(block_data);
    
    Json::Json::Value response = rpc_client.call("submitblock", params);
    
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

// ===== ESSENTIAL COMMANDS (6 most impactful additions) =====

void executeHeight(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("getblockcount", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block height: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    int height = response["result"].asInt();
    std::cout << "📏 Current Block Height: " << height << std::endl;
}

void executeGetBlockHash(Dinero::Common::RPCClient& rpc_client, int height) {
    Json::Json::Value params;
    params.push_back(height);
    
    Json::Json::Value response = rpc_client.call("getblockhash", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get block hash: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string hash = response["result"].asString();
    std::cout << "🔗 Block " << height << " Hash: " << hash << std::endl;
}

void executeMinerStart(Dinero::Common::RPCClient& rpc_client, int threads) {
    Json::Json::Value params;
    params.push_back(true);  // generate = true
    params.push_back(threads);  // genproclimit
    
    Json::Json::Value response = rpc_client.call("setgenerate", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to start miner: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::cout << "🚀 Miner started successfully!" << std::endl;
    std::cout << "   Using " << threads << " thread(s)" << std::endl;
    std::cout << "   Use 'dinero-cli miner stop' to stop mining" << std::endl;
}

void executeMinerStop(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value params;
    params.push_back(false);  // generate = false
    
    Json::Json::Value response = rpc_client.call("setgenerate", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to stop miner: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::cout << "🛑 Miner stopped successfully!" << std::endl;
}

void executeAddrNew(Dinero::Common::RPCClient& rpc_client, const std::string& type) {
    Json::Json::Value params;
    if (!type.empty()) {
        params.push_back("");  // label (empty)
        params.push_back(type);  // address type
    }
    
    Json::Json::Value response = rpc_client.call("getnewaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to generate new address: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string address = response["result"].asString();
    std::cout << "🏠 New Address Generated: " << address << std::endl;
    if (!type.empty()) {
        std::cout << "   Type: " << type << std::endl;
    }
}

void executeListUnspent(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("listunspent", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to list unspent outputs: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value utxos = response["result"];
    if (!utxos.isArray() || utxos.empty()) {
        std::cout << "💰 No unspent outputs found" << std::endl;
        return;
    }
    
    std::cout << "💰 Unspent Outputs (" << utxos.size() << "):" << std::endl;
    
    for (const auto& utxo : utxos) {
        std::cout << "  TXID: " << utxo["txid"].asString() << std::endl;
        std::cout << "  Vout: " << utxo["vout"].asInt() << std::endl;
        std::cout << "  Amount: " << utxo["amount"].asDouble() << " DIN" << std::endl;
        std::cout << "  Address: " << utxo["address"].asString() << std::endl;
        std::cout << "  Confirmations: " << utxo["confirmations"].asInt() << std::endl;
        std::cout << "  ---" << std::endl;
    }
}

// New wallet and mining command implementations
void executeGetNewAddress(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("wallet.getnewaddress", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to generate new address: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::string address = response["result"].asString();
    std::cout << "🏠 New Address Generated: " << address << std::endl;
}

void executeValidateAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address) {
    Json::Json::Value params;
    params.push_back(address);
    
    Json::Json::Value response = rpc_client.call("wallet.validateaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to validate address: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    bool isValid = result["isvalid"].asBool();
    bool isMine = result["ismine"].asBool();
    
    std::cout << "🔍 Address Validation Results:" << std::endl;
    std::cout << "  Address: " << address << std::endl;
    std::cout << "  Valid: " << (isValid ? "✅ Yes" : "❌ No") << std::endl;
    std::cout << "  Mine: " << (isMine ? "✅ Yes" : "❌ No") << std::endl;
    
    if (result.contains("account")) {
        std::cout << "  Account: " << result["account"].asString() << std::endl;
    }
}

void executeSetMiningAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address) {
    Json::Json::Value params;
    params.push_back(address);
    
    Json::Json::Value response = rpc_client.call("mining.setaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to set mining address: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::cout << "⛏️ Mining address set successfully: " << address << std::endl;
}

void executeGetMiningAddress(Dinero::Common::RPCClient& rpc_client) {
    Json::Json::Value response = rpc_client.call("mining.getaddress", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to get mining address: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    std::string address = result["address"].asString();
    bool isMine = result["ismine"].asBool();
    std::string source = result["source"].asString();
    
    std::cout << "⛏️ Current Mining Address:" << std::endl;
    std::cout << "  Address: " << address << std::endl;
    std::cout << "  Mine: " << (isMine ? "✅ Yes" : "❌ No") << std::endl;
    std::cout << "  Source: " << source << std::endl;
}

void executeGenerateToAddress(Dinero::Common::RPCClient& rpc_client, int numBlocks, const std::string& address) {
    Json::Json::Value params;
    params.push_back(numBlocks);
    params.push_back(address);
    
    Json::Json::Value response = rpc_client.call("mining.generatetoaddress", params);
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to generate blocks: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    Json::Json::Value result = response["result"];
    if (result.isArray()) {
        std::cout << "⛏️ Generated " << numBlocks << " blocks:" << std::endl;
        for (const auto& hash : result) {
            std::cout << "  Block: " << hash.asString() << std::endl;
        }
    } else {
        std::cout << "⛏️ Block generation completed" << std::endl;
    }
    double total = 0.0;
    
    for (const auto& utxo : utxos) {
        std::string txid = getStringField(utxo, "txid", "unknown");
        int vout = getIntField(utxo, "vout", 0);
        double amount = getDoubleField(utxo, "amount", 0.0);
        int confirmations = getIntField(utxo, "confirmations", 0);
        std::string address = getStringField(utxo, "address", "unknown");
        
        std::cout << "   📦 " << txid.substr(0, 16) << "..." << ":" << vout << std::endl;
        std::cout << "      💵 Amount: " << std::fixed << std::setprecision(8) << amount << " DIN" << std::endl;
        std::cout << "      ✅ Confirmations: " << confirmations << std::endl;
        std::cout << "      🏠 Address: " << address << std::endl;
        std::cout << "      ---" << std::endl;
        
        total += amount;
    }
    
    std::cout << "💎 Total Unspent: " << std::fixed << std::setprecision(8) << total << " DIN" << std::endl;
}

void executeStop(Dinero::Common::RPCClient& rpc_client) {
    std::cout << "🛑 Stopping Dinero daemon..." << std::endl;
    
    Json::Json::Value response = rpc_client.call("stop", Json::Json::Value());
    
    if (isRpcError(response)) {
        std::cout << "❌ Failed to stop daemon: " << getRpcErrorMessage(response) << std::endl;
        return;
    }
    
    std::cout << "✅ Daemon shutdown initiated" << std::endl;
    std::cout << "   The daemon will stop gracefully" << std::endl;
}

} // namespace dinero 