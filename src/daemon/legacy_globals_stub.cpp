// Legacy Global Bridge (DEPRECATED - November 7, 2025)
//
// ⚠️  BRIDGE REMOVED: ChainstateService no longer sets these globals!
//
// MIGRATION STATUS:
// ✅ RPC HANDLERS: 100% migrated to DaemonContext (no longer use globals)
// ✅ MINING CODE: 100% migrated to constructor injection (BlockAssembler, MiningTemplateValidator)
// ✅ CONSENSUS CODE: DatabaseUTXOProvider now accepts ChainDB* via constructor
// ✅ CHAINSTATE SERVICE: No longer sets g_chain_db_direct or g_utxo_set_direct
//
// ⚠️  REMAINING USAGE (LEGACY CODE):
// - src/wallet/wallet_worker.cpp: Uses g_utxo_set_direct for wallet scanning
//   Has defensive null checks, will skip scanning if nullptr
//   TODO: Refactor WalletWorker to accept UTXOIndex* via constructor/context
//
// GLOBALS BELOW ARE NOW ALWAYS nullptr (unless set by legacy code)
//
// NEXT STEPS:
// 1. Refactor WalletWorker to use DaemonContext (medium priority)
// 2. Delete this file entirely (low priority)
//
// DO NOT ADD NEW GLOBALS HERE - use DaemonContext instead!

#include "storage/chain_db.h"
#include "wallet/utxo_index.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include "common/config_manager.h"
#include <string>
#include <memory>
#include <vector>
#include <map>

// Data directory global (legacy, not in namespace)
// USAGE: Minimal (config uses ctx.config)
// ACTION: Safe to remove once all config code migrated
std::string g_data_dir = "";

// Wallet manager global (legacy, not in namespace)
// USAGE: Accessor function below (GetWalletManagerForIndexing)
// ACTION: Remove once wallet indexing uses ctx.wallet
dinero::WalletManager* g_wallet_manager = nullptr;

// WebSocket subscriptions global (legacy) - defined in ws_globals.cpp
// std::map<std::string, std::vector<int>> g_subscriptions;

namespace dinero {

// Logger global (legacy) - defined in logger.cpp
// Logger g_logger;

// Chain database global (STILL USED BY NON-RPC CODE)
// USAGE: block_assembler.cpp, template_validator.cpp, transaction_validator.cpp
// ACTION: Refactor mining code to accept ChainDB* via constructor
ChainDB* g_chain_db_direct = nullptr;

// UTXO index global (legacy)
// USAGE: Paired with g_chain_db_direct
// ACTION: Refactor mining code to accept UTXOIndex* via constructor
UTXOIndex* g_utxo_set_direct = nullptr;

// P2P manager global (legacy)
// USAGE: Minimal (RPC uses ctx.p2p)
// ACTION: Remove once p2p_globals.h refactored
class P2PManager;
P2PManager* g_p2p = nullptr;

// Config manager global (legacy) - defined in config_manager.cpp
// ConfigManager g_config;

// Legacy wallet manager accessor function
// USAGE: Wallet indexing code
// ACTION: Remove once indexing code uses ctx.wallet
WalletManager* GetWalletManagerForIndexing() {
    return ::g_wallet_manager;
}

} // namespace dinero
