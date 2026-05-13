#pragma once

class HttpRpcServer;

// Hardware Wallet RPC Method Registration (vNext RpcRegistry)
// Registers hardware wallet PSBT RPC methods in global RpcRegistry
void registerHardwareWalletRPC();

// Legacy HttpRpcServer bridge (will be removed after full vNext migration)
void registerHardwareWalletHandlers(HttpRpcServer* rpc_server);
