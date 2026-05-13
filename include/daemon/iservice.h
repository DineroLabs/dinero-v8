#pragma once
#include <string>

// Forward declaration
struct DaemonContext;

namespace dinero {

/**
 * Common interface for all daemon services
 *
 * Services follow a deterministic lifecycle:
 * 1. Construction (lightweight, no I/O)
 * 2. Init(ctx) - Wire dependencies, validate config
 * 3. Start() - Open resources, spawn threads
 * 4. Stop() - Graceful shutdown, flush data
 * 5. Destruction
 */
class IService {
public:
    virtual ~IService() = default;

    /**
     * Service name for logging and diagnostics
     */
    virtual std::string Name() const = 0;

    /**
     * Initialize service with context dependencies
     * @param ctx Shared daemon context with all services
     * @return true if initialization succeeded
     */
    virtual bool Init(DaemonContext& ctx) = 0;

    /**
     * Start the service (open files, spawn threads, etc.)
     * @return true if startup succeeded
     */
    virtual bool Start() = 0;

    /**
     * Stop the service gracefully (reverse of Start)
     */
    virtual void Stop() = 0;

    /**
     * Get service health status
     * @return true if service is healthy, false if degraded/failing
     */
    virtual bool IsHealthy() const { return true; }

    /**
     * Get service metrics for monitoring
     * @return JSON object with service-specific metrics
     */
    virtual std::string GetMetrics() const { return "{}"; }
};

} // namespace dinero
