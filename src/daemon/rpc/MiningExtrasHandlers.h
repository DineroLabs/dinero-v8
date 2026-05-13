#pragma once

// Forward declarations
class RpcRegistry;
namespace dinero {
    class ChainDB;
    class WalletManager;
}
class SimpleBlockchain;  // For legacy compatibility

// ═══════════════════════════════════════════════════════════════════════════
// LEGACY DAEMON MINING HANDLERS (DEPRECATED - NOT IN USE)
// ═══════════════════════════════════════════════════════════════════════════
// registerMiningExtras() is DEPRECATED and NOT called in rpc_context_wiring.cpp
// Only registerMiningExtrasMethodsVNext() is active (see methods_mining_extras.cpp)
//
// To re-enable: uncomment #define DIN_ENABLE_LEGACY_DAEMON_MINING in .cpp file
// ═══════════════════════════════════════════════════════════════════════════

#ifdef DIN_ENABLE_LEGACY_DAEMON_MINING
/**
 * [DEPRECATED] Register legacy mining-related RPC handlers
 * This function is NOT called in the active codebase.
 * Use methods_mining_extras.cpp (VNext) instead.
 */
void registerMiningExtras(
    RpcRegistry& registry,
    dinero::ChainDB* chaindb,
    dinero::WalletManager* wallet
);
#endif

void registerWalletMnemonic(
    RpcRegistry& registry,
    dinero::WalletManager* wallet
);

void registerMultiAccount(
    RpcRegistry& registry,
    dinero::WalletManager* wallet
);
