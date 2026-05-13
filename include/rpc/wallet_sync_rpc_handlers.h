#pragma once

namespace dinero {
namespace rpc {

/**
 * @brief Phase W.2.6: Wallet Sync UX RPC Handlers
 *
 * Exposes wallet sync status, ETA, reorg info, and slow reason analysis to UI.
 *
 * RPC Methods:
 * - getsyncstatus: Get complete wallet sync state (phase, progress, ETA, slow reason)
 * - getreorginfo: Get recent chain reorganizations
 * - getslowreason: Get detailed analysis of why sync is slow
 *
 * Design Principles:
 * - Read-only: No state modification
 * - Safe: Works even without wallet loaded
 * - UI-friendly: Rich JSON responses with human-readable strings
 * - Complete: All W.2 UX features accessible via RPC
 */

void registerWalletSyncMethods();

} // namespace rpc
} // namespace dinero
