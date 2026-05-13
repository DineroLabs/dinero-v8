#pragma once

// Forward declaration
class HttpRpcServer;

/**
 * Register hardware wallet RPC handlers (vNext RpcRegistry)
 * - hwallet.exportpsbttofile: Export PSBT to file for air-gapped signing
 * - hwallet.importpsbtfromfile: Import signed PSBT from file
 * - hwallet.analyzepsbt: Analyze PSBT signing status and metadata
 * - hwallet.enumeratehwdevices: Enumerate connected USB hardware wallets
 */
void registerHardwareWalletRPC();

/**
 * Register hardware wallet RPC handlers (HttpRpcServer bridge)
 *
 * This is the active registration function used by the daemon.
 * It registers methods with HttpRpcServer, which is what the daemon actually uses.
 */
void registerHardwareWalletHandlers(HttpRpcServer* rpc_server);
