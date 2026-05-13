#pragma once

#include "daemon/daemon_context.h"  // Need full definition for function parameters

// Phase 29B: Centralized RPC Registration Layer
// This file provides a single entry point for registering ALL RPC methods.
// Each subsystem registers exactly once through these functions.
// Uses the global g_rpcRegistry declared in rpc_registry.h

// Master registration function - called once during daemon initialization
// Takes DaemonContext to access services (chaindb, wallet, etc.)
void RegisterAllRPCMethods(DaemonContext& ctx);

// Subsystem-specific registration functions
// Each takes DaemonContext to access required services
void RegisterBlockchainRPC(DaemonContext& ctx);
void RegisterMiningRPC(DaemonContext& ctx);
void RegisterWalletRPC(DaemonContext& ctx);
void RegisterP2PRPC(DaemonContext& ctx);
void RegisterNetworkRPC(DaemonContext& ctx);
void RegisterDiagnosticsRPC(DaemonContext& ctx);  // Phase E.3.1: CPU stats & resource monitoring
void RegisterDpiRPC(DaemonContext& ctx);          // DPI: Dinero Payment Intent protocol
