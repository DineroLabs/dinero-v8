#pragma once

// Wallet RPC Method Registration (vNext RpcRegistry)
// Registers core wallet RPC methods in global RpcRegistry
// Batch 1: Read-only methods (getbalance, getnewaddress, listaddresses, listunspent, getwalletinfo, validateaddress)
void registerWalletRPC();
