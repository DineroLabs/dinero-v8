// SPDX-License-Identifier: MIT
// Dinero - Extended Wallet RPC Handler Signatures
// These are the missing RPC methods needed for production CLI

#pragma once
#include "compat/jsoncpp_compat.h"

// Forward declarations
namespace dinero {
    class RPCServer;
    class WalletManager;
}

// Wallet management lifecycle
Json::Value rpc_wallet_create           (dinero::RPCServer&, const Json::Value& params);  // {name}
Json::Value rpc_wallet_load             (dinero::RPCServer&, const Json::Value& params);  // {name}
Json::Value rpc_wallet_encrypt          (dinero::RPCServer&, const Json::Value& params);  // {passphrase}
Json::Value rpc_wallet_lock             (dinero::RPCServer&, const Json::Value& params);  // {}
Json::Value rpc_wallet_unlock           (dinero::RPCServer&, const Json::Value& params);  // {passphrase, timeout?}
Json::Value rpc_wallet_change_passphrase(dinero::RPCServer&, const Json::Value& params);  // {old_passphrase, new_passphrase}

// Wallet information and queries
Json::Value rpc_wallet_info             (dinero::RPCServer&, const Json::Value& params);  // {} - enhanced with balance info
Json::Value rpc_wallet_balance          (dinero::RPCServer&, const Json::Value& params);  // {minconf?}
Json::Value rpc_wallet_addresses        (dinero::RPCServer&, const Json::Value& params);  // {label_filter?}
Json::Value rpc_wallet_utxos            (dinero::RPCServer&, const Json::Value& params);  // {minconf?, max?, label?}
Json::Value rpc_wallet_history          (dinero::RPCServer&, const Json::Value& params);  // {limit?, since_height?, since_time?}
Json::Value rpc_wallet_label            (dinero::RPCServer&, const Json::Value& params);  // {address, label}

// Address operations (enhanced existing)
Json::Value rpc_wallet_getnewaddress    (dinero::RPCServer&, const Json::Value& params);  // {label?}
Json::Value rpc_wallet_validateaddress  (dinero::RPCServer&, const Json::Value& params);  // {address}

// Descriptor wallet operations (Phase 1: Read-only descriptor RPCs)
Json::Value rpc_wallet_listdescriptors  (dinero::RPCServer&, const Json::Value& params);  // {private?}
Json::Value rpc_wallet_getdescriptorinfo(dinero::RPCServer&, const Json::Value& params);  // {descriptor}
Json::Value rpc_wallet_deriveaddresses  (dinero::RPCServer&, const Json::Value& params);  // {descriptor, range?}

// Transaction operations
Json::Value rpc_tx_send                 (dinero::RPCServer&, const Json::Value& params);  // {to, amount, fee_rate?, subtract_fee?, utxos?[]}

// Mining runtime control
Json::Value rpc_mining_info             (dinero::RPCServer&, const Json::Value& params);  // {}
Json::Value rpc_mining_start            (dinero::RPCServer&, const Json::Value& params);  // {threads?}
Json::Value rpc_mining_stop             (dinero::RPCServer&, const Json::Value& params);  // {}
Json::Value rpc_mining_setaddress       (dinero::RPCServer&, const Json::Value& params);  // {address}
Json::Value rpc_mining_getaddress       (dinero::RPCServer&, const Json::Value& params);  // {}

// Node diagnostics and meta
Json::Value rpc_node_info               (dinero::RPCServer&, const Json::Value& params);  // {}
Json::Value rpc_rpc_methods             (dinero::RPCServer&, const Json::Value& params);  // {}

// Confidential transaction operations
Json::Value rpc_getnewconfidentialaddress (dinero::RPCServer&, const Json::Value& params);  // {account?, label?}
Json::Value rpc_getconfidentialbalance    (dinero::RPCServer&, const Json::Value& params);  // {account?, minconf?}
Json::Value rpc_sendconfidential          (dinero::RPCServer&, const Json::Value& params);  // {address, amount, fee_rate?, subtract_fee?}
Json::Value rpc_gettotalbalance           (dinero::RPCServer&, const Json::Value& params);  // {minconf?}

// Helper functions for parameter validation
namespace RpcValidation {
    void require_string(const Json::Value& params, const std::string& field);
    void require_number(const Json::Value& params, const std::string& field);
    std::string get_string(const Json::Value& params, const std::string& field, const std::string& default_val = "");
    double get_number(const Json::Value& params, const std::string& field, double default_val = 0.0);
    int get_int(const Json::Value& params, const std::string& field, int default_val = 0);
    bool get_bool(const Json::Value& params, const std::string& field, bool default_val = false);
}
