// SPDX-License-Identifier: MIT
// Dinero - Descriptor Wallet RPC Registration
// Add these registrations to your existing RPC server initialization

#include "daemon/rpc/wallet_rpc_extras.h"
#include "daemon/rpc_server.h"

/**
 * @brief Register descriptor wallet RPC methods
 *
 * This function registers Phase 1 descriptor wallet RPCs:
 * - wallet.listdescriptors: List active descriptors
 * - wallet.getdescriptorinfo: Parse and analyze descriptors
 * - wallet.deriveaddresses: Derive addresses from descriptors
 *
 * These RPCs provide Bitcoin Core compatibility and enable:
 * - Wallet auditability (users can inspect address derivation)
 * - Hardware wallet support (export descriptors for offline signing)
 * - Watch-only wallet preparation (Phase 2)
 *
 * To integrate, call this function after initializing your RPCServer:
 *
 * Example:
 *   dinero::RPCServer rpc_server;
 *   rpc_server.initialize(8332);
 *   // ... register other RPCs ...
 *   register_descriptor_wallet_rpcs(rpc_server);
 *   rpc_server.start();
 */
void register_descriptor_wallet_rpcs(dinero::RPCServer& server) {
    // Phase 1: Read-only descriptor RPCs
    server.registerMethod("wallet.listdescriptors",
        [&server](const std::string& json_params) {
            Json::Value params;
            Json::Reader reader;
            reader.parse(json_params, params);
            Json::Value result = rpc_wallet_listdescriptors(server, params);
            Json::StreamWriterBuilder builder;
            return Json::writeString(builder, result);
        },
        "List active wallet descriptors",
        "wallet");

    server.registerMethod("wallet.getdescriptorinfo",
        [&server](const std::string& json_params) {
            Json::Value params;
            Json::Reader reader;
            reader.parse(json_params, params);
            Json::Value result = rpc_wallet_getdescriptorinfo(server, params);
            Json::StreamWriterBuilder builder;
            return Json::writeString(builder, result);
        },
        "Parse and analyze a descriptor string",
        "wallet");

    server.registerMethod("wallet.deriveaddresses",
        [&server](const std::string& json_params) {
            Json::Value params;
            Json::Reader reader;
            reader.parse(json_params, params);
            Json::Value result = rpc_wallet_deriveaddresses(server, params);
            Json::StreamWriterBuilder builder;
            return Json::writeString(builder, result);
        },
        "Derive addresses from a descriptor",
        "wallet");
}

// Example usage documentation:
/*

wallet.listdescriptors:
  Request:  {"method": "wallet.listdescriptors", "params": {"private": false}}
  Response: {
    "wallet_name": "default",
    "descriptors": [
      {
        "desc": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)#abcd1234",
        "timestamp": 0,
        "active": true,
        "internal": false,
        "range": [0, 1000],
        "next": 5
      },
      {
        "desc": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../1/*)#efgh5678",
        "timestamp": 0,
        "active": true,
        "internal": true,
        "range": [0, 1000],
        "next": 3
      }
    ]
  }

wallet.getdescriptorinfo:
  Request:  {"method": "wallet.getdescriptorinfo", "params": {"descriptor": "wpkh([...]/0/*)"}}
  Response: {
    "descriptor": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)",
    "checksum": "abcd1234",
    "isrange": true,
    "issolvable": true,
    "hasprivatekeys": false,
    "fingerprint": "8a2b3c4d",
    "derivation_path": "m/84h/1447h/0h",
    "type": "wpkh"
  }

wallet.deriveaddresses:
  Request:  {"method": "wallet.deriveaddresses", "params": {"descriptor": "wpkh([...]/0/*)", "range": [0, 5]}}
  Response: {
    "addresses": [
      "din1q...",  // Index 0
      "din1q...",  // Index 1
      "din1q...",  // Index 2
      "din1q...",  // Index 3
      "din1q...",  // Index 4
      "din1q..."   // Index 5
    ]
  }

*/
