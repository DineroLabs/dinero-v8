#pragma once

#include "pool/pool_db.h"
#include "pool/pool_manager.h"
#include <memory>
#include <string>

namespace din {
namespace rpc {

/**
 * Register pool accounting RPC methods
 *
 * Methods registered:
 * - pool.status         - Get pool accounting feature status/gating reason
 * - pool.stats          - Get overall pool statistics
 * - pool.getconfig      - Get pool configuration
 * - pool.setconfig      - Update pool configuration
 * - pool.workers        - Get list of active workers
 * - pool.worker         - Get detailed stats for a worker
 * - pool.blocks         - Get list of blocks found
 * - pool.block          - Get detailed block info
 * - pool.payouts        - Get pending payouts
 * - pool.processpayouts - Process pending payouts
 * - pool.authorizeworker   - Authorize/register a pool worker (mutating)
 * - pool.submitshare       - Submit a share result to accounting (mutating)
 * - pool.disconnectworker  - Mark worker disconnected (mutating)
 */
void registerPoolMethods();

/**
 * Configure pool RPC runtime gate and database wiring.
 *
 * Pool accounting is disabled by default. Operators must explicitly enable it
 * and pass sync-profile gates before methods become active.
 */
void configurePoolRpc(std::shared_ptr<dinero::pool::PoolDB> pool_db,
                      std::shared_ptr<dinero::pool::PoolManager> pool_manager,
                      bool enabled,
                      const std::string& disabled_reason);

// Global pool database pointer (must be set before calling RPC methods)
extern std::shared_ptr<dinero::pool::PoolDB> g_pool_db;
extern std::shared_ptr<dinero::pool::PoolManager> g_pool_manager;

} // namespace rpc
} // namespace din
