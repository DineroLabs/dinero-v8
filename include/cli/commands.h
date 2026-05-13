#pragma once

#include "common/rpc_client.h"
#include <CLI/CLI.hpp>
#include <string>

namespace dinero {

// Register all CLI commands with the CLI11 app
void registerCommands(CLI::App& app, Dinero::Common::RPCClient& rpc_client);

// Existing command implementations
void executePing(Dinero::Common::RPCClient& rpc_client);
void executeSelfTest(Dinero::Common::RPCClient& rpc_client);
void executeGetInfo(Dinero::Common::RPCClient& rpc_client);
void executeGetMiningInfo(Dinero::Common::RPCClient& rpc_client);
void executeGetNetworkInfo(Dinero::Common::RPCClient& rpc_client);
void executeGetPeerInfo(Dinero::Common::RPCClient& rpc_client);
void executeGetMempoolInfo(Dinero::Common::RPCClient& rpc_client);
void executeGetBlock(Dinero::Common::RPCClient& rpc_client, const std::string& hash);
void executeGetTx(Dinero::Common::RPCClient& rpc_client, const std::string& txid);
void executeGetTransaction(Dinero::Common::RPCClient& rpc_client, const std::string& tx_hash);
void executeGetBalance(Dinero::Common::RPCClient& rpc_client);
void executeSendToAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address, double amount);
void executeSetGenerate(Dinero::Common::RPCClient& rpc_client, bool generate, int genproclimit);
void executeGetGenerate(Dinero::Common::RPCClient& rpc_client);
void executeGetBlockTemplate(Dinero::Common::RPCClient& rpc_client);
void executeSubmitBlock(Dinero::Common::RPCClient& rpc_client, const std::string& block_data);

// ===== NEW ESSENTIAL COMMANDS =====
void executeHeight(Dinero::Common::RPCClient& rpc_client);
void executeGetBlockHash(Dinero::Common::RPCClient& rpc_client, int height);
void executeMinerStart(Dinero::Common::RPCClient& rpc_client, int threads);
void executeMinerStop(Dinero::Common::RPCClient& rpc_client);
void executeAddrNew(Dinero::Common::RPCClient& rpc_client, const std::string& type = "");
void executeListUnspent(Dinero::Common::RPCClient& rpc_client);
void executeStop(Dinero::Common::RPCClient& rpc_client);

// New wallet and mining commands
void executeGetNewAddress(Dinero::Common::RPCClient& rpc_client);
void executeValidateAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address);
void executeSetMiningAddress(Dinero::Common::RPCClient& rpc_client, const std::string& address);
void executeGetMiningAddress(Dinero::Common::RPCClient& rpc_client);
void executeGenerateToAddress(Dinero::Common::RPCClient& rpc_client, int numBlocks, const std::string& address);

} // namespace dinero
