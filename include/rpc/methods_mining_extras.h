#pragma once

#include "http_rpc_server.h"
#include "transaction_pool.h"
#include "storage/chain_db.h"
#include "rpc/methods_mining.h"  // For MiningState
#include <json/json.h>
#include <memory>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════
// Mining Extras Configuration Helper Struct
// ═══════════════════════════════════════════════════════════

struct MiningExtrasConfig {
    bool regtest;
    bool testnet;
    std::string datadir;
    int rpc_port;
};

// ═══════════════════════════════════════════════════════════
// Mining Extras RPC Method Registrations
// ═══════════════════════════════════════════════════════════

/**
 * Register mining extras RPC methods with the HTTP RPC server.
 *
 * Methods registered:
 * - getblocktemplate: Get block template for mining (complex, ~220 lines)
 * - generatetoaddress: Mine blocks to a specified address - regtest only (complex, ~310 lines)
 *
 * @param server RPC server to register methods with
 * @param tx_pool Transaction pool for selecting transactions
 * @param chain_db Chain database reference
 * @param config Mining extras configuration
 * @param mining_state Mining state (for generatetoaddress)
 */
void registerMiningExtrasMethods(
    HttpRpcServer* server,
    mempool::TransactionPool* tx_pool,
    dinero::ChainDB* chain_db,
    const MiningExtrasConfig& config,
    std::shared_ptr<dinero::rpc::MiningState> mining_state
);

} // namespace rpc
} // namespace dinero
