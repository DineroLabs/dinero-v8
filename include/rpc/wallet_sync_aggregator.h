#pragma once

#include <cstdint>

// Forward declarations
struct ExecutionContext;  // Global ExecutionContext from rpc_registry.h

namespace dinero {
    struct WalletSyncStatus;
}

namespace dinero {

/**
 * @brief Build WalletSyncStatus from live execution context
 *
 * Single aggregation point - all sync state queries go through here.
 * Read-only, snapshot semantics, no side effects.
 *
 * @param ctx Execution context with component access
 * @return Populated and validated WalletSyncStatus
 * @throws std::runtime_error if critical components unavailable
 */
WalletSyncStatus BuildWalletSyncStatusFromContext(const ::ExecutionContext& ctx);

/**
 * @brief Get current timestamp in milliseconds
 *
 * @return Milliseconds since epoch
 */
uint64_t GetCurrentTimeMs();

} // namespace dinero
