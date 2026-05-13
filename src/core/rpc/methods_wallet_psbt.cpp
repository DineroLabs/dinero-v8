/*
 * Deprecated split-brain PSBT RPC implementation.
 *
 * Canonical implementation lives in:
 *   src/rpc/methods_wallet_psbt.cpp
 *
 * This TU is intentionally symbol-empty to prevent behavioral drift.
 * If legacy RPC is explicitly enabled, fail-fast here until/unless a
 * separate legacy-only implementation is intentionally restored.
 */

#ifndef DIN_ENABLE_LEGACY_RPC
#define DIN_ENABLE_LEGACY_RPC 0
#endif

#if DIN_ENABLE_LEGACY_RPC
#error "Legacy core PSBT RPC TU retired. Use src/rpc/methods_wallet_psbt.cpp"
#endif
