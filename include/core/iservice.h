/**
 * IService - Base interface for all daemon services
 *
 * Provides lifecycle management (Init → Start → Stop) for deterministic
 * startup/shutdown sequences and dependency injection via DaemonContext.
 */

#pragma once

#include <string>
#include <memory>

namespace dinero {

// Forward declaration
struct DaemonContext;

/**
 * Base interface for all daemon services
 *
 * Services are initialized and started in a controlled order:
 * 1. Init() - Set up dependencies, allocate resources
 * 2. Start() - Begin operation (threads, network, etc.)
 * 3. Stop() - Gracefully shutdown (reverse order)
 */
class IService {
public:
    virtual ~IService() = default;

    /**
     * Service name for logging and diagnostics
     */
    virtual std::string Name() const = 0;

    /**
     * Initialize service with daemon context
     *
     * @param ctx Reference to daemon context containing all services
     * @return true on success, false on failure
     *
     * Called during daemon initialization before any services start.
     * Use this to:
     * - Store references to dependencies from ctx
     * - Validate configuration
     * - Allocate resources
     * - Open databases/files
     */
    virtual bool Init(DaemonContext& ctx) = 0;

    /**
     * Start service operation
     *
     * @return true on success, false on failure
     *
     * Called after all services are initialized.
     * Use this to:
     * - Start worker threads
     * - Begin network operations
     * - Start timers/callbacks
     */
    virtual bool Start() = 0;

    /**
     * Stop service operation
     *
     * Called in reverse order during shutdown.
     * Use this to:
     * - Stop threads gracefully
     * - Close connections
     * - Flush buffers
     * - Release resources
     *
     * Must be idempotent (safe to call multiple times).
     */
    virtual void Stop() = 0;

    /**
     * Get service health status
     *
     * @return true if service is healthy, false if degraded/failing
     */
    virtual bool IsHealthy() const { return true; }

    /**
     * Get service metrics for monitoring
     *
     * @return JSON object with service-specific metrics
     */
    virtual std::string GetMetrics() const { return "{}"; }
};

} // namespace dinero
