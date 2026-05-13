// SPDX-License-Identifier: MIT
// Dinero - RPC Integration for All New Handlers

#include "daemon/rpc/wallet_rpc_extras.h"
#include "rpc/rpc_server.hpp"

// Include all handler implementations
extern json::json rpc_wallet_create(RpcServer&, const json::json&);
extern json::json rpc_wallet_load(RpcServer&, const json::json&);
extern json::json rpc_wallet_encrypt(RpcServer&, const json::json&);
extern json::json rpc_wallet_lock(RpcServer&, const json::json&);
extern json::json rpc_wallet_unlock(RpcServer&, const json::json&);
extern json::json rpc_wallet_change_passphrase(RpcServer&, const json::json&);
extern json::json rpc_wallet_balance(RpcServer&, const json::json&);
extern json::json rpc_wallet_addresses(RpcServer&, const json::json&);
extern json::json rpc_wallet_utxos(RpcServer&, const json::json&);
extern json::json rpc_wallet_history(RpcServer&, const json::json&);
extern json::json rpc_wallet_label(RpcServer&, const json::json&);
extern json::json rpc_tx_send(RpcServer&, const json::json&);
extern json::json rpc_mining_info(RpcServer&, const json::json&);
extern json::json rpc_mining_start(RpcServer&, const json::json&);
extern json::json rpc_mining_stop(RpcServer&, const json::json&);
extern json::json rpc_mining_setaddress(RpcServer&, const json::json&);
extern json::json rpc_mining_getaddress(RpcServer&, const json::json&);
extern json::json rpc_node_info(RpcServer&, const json::json&);
extern json::json rpc_rpc_methods(RpcServer&, const json::json&);

// Register all production RPC methods
void register_production_rpc_methods(RpcServer& server) {
    
    // Wallet lifecycle management
    server.registerMethod("wallet.create", rpc_wallet_create, 
        "Create a new wallet", "wallet");
    server.registerMethod("wallet.load", rpc_wallet_load, 
        "Load an existing wallet", "wallet");
    server.registerMethod("wallet.encrypt", rpc_wallet_encrypt, 
        "Encrypt wallet with passphrase", "wallet");
    server.registerMethod("wallet.lock", rpc_wallet_lock, 
        "Lock encrypted wallet", "wallet");
    server.registerMethod("wallet.unlock", rpc_wallet_unlock, 
        "Unlock encrypted wallet", "wallet");
    server.registerMethod("wallet.change_passphrase", rpc_wallet_change_passphrase, 
        "Change wallet passphrase", "wallet");

    // Wallet information and queries
    server.registerMethod("wallet.balance", rpc_wallet_balance, 
        "Get wallet balance", "wallet");
    server.registerMethod("wallet.addresses", rpc_wallet_addresses, 
        "List wallet addresses", "wallet");
    server.registerMethod("wallet.utxos", rpc_wallet_utxos, 
        "List unspent outputs", "wallet");
    server.registerMethod("wallet.history", rpc_wallet_history, 
        "Get transaction history", "wallet");
    server.registerMethod("wallet.label", rpc_wallet_label, 
        "Set address label", "wallet");

    // Transaction operations
    server.registerMethod("tx.send", rpc_tx_send, 
        "Send transaction with advanced options", "wallet");

    // Mining runtime control
    server.registerMethod("mining.info", rpc_mining_info, 
        "Get mining information", "mining");
    server.registerMethod("mining.start", rpc_mining_start, 
        "Start mining", "mining");
    server.registerMethod("mining.stop", rpc_mining_stop, 
        "Stop mining", "mining");
    server.registerMethod("mining.setaddress", rpc_mining_setaddress, 
        "Set mining payout address", "mining");
    server.registerMethod("mining.getaddress", rpc_mining_getaddress, 
        "Get current mining address", "mining");

    // Node diagnostics
    server.registerMethod("node.info", rpc_node_info, 
        "Get comprehensive node information", "util");
    server.registerMethod("rpc.methods", rpc_rpc_methods, 
        "List all available RPC methods", "util");

    // Legacy aliases for backward compatibility
    server.registerMethod("getbalance", rpc_wallet_balance, 
        "Get wallet balance (legacy)", "wallet");
    server.registerMethod("listtransactions", rpc_wallet_history, 
        "Get transaction history (legacy)", "wallet");
    server.registerMethod("listunspent", rpc_wallet_utxos, 
        "List unspent outputs (legacy)", "wallet");
    server.registerMethod("sendtoaddress", rpc_tx_send, 
        "Send transaction (legacy)", "wallet");
    server.registerMethod("getmininginfo", rpc_mining_info, 
        "Get mining info (legacy)", "mining");
    server.registerMethod("setminingaddress", rpc_mining_setaddress, 
        "Set mining address (legacy)", "mining");
    server.registerMethod("getminingaddress", rpc_mining_getaddress, 
        "Get mining address (legacy)", "mining");
}

// Integration with existing RPC server initialization
// Add this call to your daemon's RPC server setup:
//
// void init_rpc_server() {
//     RpcServer& server = RpcServer::getInstance();
//     
//     // Register existing methods...
//     register_existing_rpc_methods(server);
//     
//     // Register new production methods
//     register_production_rpc_methods(server);
//     
//     server.start();
// }
