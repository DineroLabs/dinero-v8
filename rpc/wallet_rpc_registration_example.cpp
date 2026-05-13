// SPDX-License-Identifier: MIT
// Dinero - RPC Registration Example for New Wallet Methods
// Add these registrations to your existing RPC server initialization

#include "daemon/rpc/wallet_rpc_extras.h"
#include "rpc/rpc_server.hpp"

// Example of how to register the new wallet RPC methods
// This should be added to your existing RPC server setup code

void register_extended_wallet_rpcs(RpcServer& server) {
    // Wallet lifecycle management
    server.registerMethod("wallet.create",            rpc_wallet_create);
    server.registerMethod("wallet.load",              rpc_wallet_load);
    server.registerMethod("wallet.encrypt",           rpc_wallet_encrypt);
    server.registerMethod("wallet.lock",              rpc_wallet_lock);
    server.registerMethod("wallet.unlock",            rpc_wallet_unlock);
    server.registerMethod("wallet.change_passphrase", rpc_wallet_change_passphrase);

    // Wallet information and queries
    server.registerMethod("wallet.info",              rpc_wallet_info);
    server.registerMethod("wallet.balance",           rpc_wallet_balance);
    server.registerMethod("wallet.addresses",         rpc_wallet_addresses);
    server.registerMethod("wallet.utxos",             rpc_wallet_utxos);
    server.registerMethod("wallet.history",           rpc_wallet_history);
    server.registerMethod("wallet.label",             rpc_wallet_label);

    // Enhanced address operations
    server.registerMethod("wallet.getnewaddress",     rpc_wallet_getnewaddress);
    server.registerMethod("wallet.validateaddress",   rpc_wallet_validateaddress);

    // Transaction operations
    server.registerMethod("tx.send",                  rpc_tx_send);

    // Mining runtime control
    server.registerMethod("mining.info",              rpc_mining_info);
    server.registerMethod("mining.start",             rpc_mining_start);
    server.registerMethod("mining.stop",              rpc_mining_stop);
    server.registerMethod("mining.setaddress",        rpc_mining_setaddress);
    server.registerMethod("mining.getaddress",        rpc_mining_getaddress);

    // Node diagnostics
    server.registerMethod("node.info",                rpc_node_info);
    server.registerMethod("rpc.methods",              rpc_rpc_methods);

    // Legacy aliases for backward compatibility
    server.registerMethod("getnewaddress",            rpc_wallet_getnewaddress);
    server.registerMethod("validateaddress",          rpc_wallet_validateaddress);
    server.registerMethod("getbalance",               rpc_wallet_balance);
    server.registerMethod("listtransactions",         rpc_wallet_history);
    server.registerMethod("listunspent",              rpc_wallet_utxos);
    server.registerMethod("sendtoaddress",            rpc_tx_send);
    server.registerMethod("getmininginfo",            rpc_mining_info);
    server.registerMethod("setminingaddress",         rpc_mining_setaddress);
    server.registerMethod("getminingaddress",         rpc_mining_getaddress);
}

// Example expected JSON-RPC request/response formats:

/*
wallet.create:
  Request:  {"method": "wallet.create", "params": {"name": "my_wallet"}}
  Response: {"result": {"name": "my_wallet", "created": true, "seed_words": ["word1", "word2", ...]}}

wallet.balance:
  Request:  {"method": "wallet.balance", "params": {"minconf": 1}}
  Response: {"result": {"confirmed": 1.5, "unconfirmed": 0.25, "total": 1.75}}

wallet.history:
  Request:  {"method": "wallet.history", "params": {"limit": 10, "since_height": 1000}}
  Response: {"result": {"transactions": [...], "count": 5, "has_more": false}}

tx.send:
  Request:  {"method": "tx.send", "params": {"to": "addr123", "amount": 1.0, "subtract_fee": true}}
  Response: {"result": {"txid": "abc123", "fee": 0.001, "sent": 1.0}}

mining.info:
  Request:  {"method": "mining.info", "params": {}}
  Response: {"result": {"mining": true, "threads": 4, "hashrate": 1234, "address": "addr123"}}
*/
