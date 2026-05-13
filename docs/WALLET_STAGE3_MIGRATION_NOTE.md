// Week 5: Wallet Stage 3 Handlers Migration Plan
//
// Status: wallet_stage3_handlers.cpp appears to be legacy/unused code
// - Only referenced in rpc_v2.cpp.disabled (disabled file)
// - Functions take Json::Value params (legacy style), not ExecutionContext
// - Not registered in active RPC registry
//
// Options:
// 1. Migrate to ExecutionContext pattern (if still needed)
// 2. Mark as deprecated/legacy and leave for now
// 3. Remove if confirmed unused
//
// For now: These handlers are NOT blocking bridge removal since they're not
// registered in the active RPC system. MultiAccountHandlers.cpp migration
// is complete, which was the only active handler using g_wallet_manager.

