#pragma once

#include "din_json.h"
#include "rpc/websocket_server.h"
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>

// Forward declare DaemonContext
struct DaemonContext;

namespace dinero {
namespace rpc {

/**
 * Progress Callback Type
 *
 * Called periodically during long-running operations to report progress.
 *
 * Parameters:
 *   - current: Current progress value (e.g., blocks processed)
 *   - total: Total work to be done (e.g., total blocks)
 *   - message: Human-readable status message
 *   - extra: Optional additional data
 */
using ProgressCallback = std::function<void(
    uint64_t current,
    uint64_t total,
    const std::string& message,
    const din::Json& extra
)>;

/**
 * Streaming RPC Handler Base Class
 *
 * Provides infrastructure for long-running RPC operations that need to report
 * progress via WebSocket events. Examples: walletrescan, blockchain sync,
 * large transaction broadcasts, batch operations.
 *
 * Features:
 * - Real-time progress updates via WebSocket
 * - Cancellation support
 * - Thread-safe operation
 * - Automatic cleanup
 *
 * Usage:
 *   1. Create handler with operation ID and WebSocket server
 *   2. Call start_operation() with work function and client_id
 *   3. Work function receives progress callback
 *   4. Handler automatically sends progress events to client
 */
class StreamingRpcHandler {
public:
    /**
     * Constructor
     * @param operation_id Unique identifier for this operation (e.g., "walletrescan")
     * @param daemon_ctx DaemonContext for accessing services (chainstate, wallet, etc.)
     */
    StreamingRpcHandler(const std::string& operation_id, DaemonContext* daemon_ctx);

    virtual ~StreamingRpcHandler();

    /**
     * Start a streaming operation
     *
     * @param client_id WebSocket client ID to send progress updates to
     * @param work_fn Function that performs the actual work
     * @param params Optional parameters for the operation
     * @return Operation ID for tracking/cancellation
     */
    std::string start_operation(
        const std::string& client_id,
        std::function<din::Json(ProgressCallback)> work_fn,
        const din::Json& params = din::Json()
    );

    /**
     * Cancel a running operation
     * @param op_id Operation ID returned by start_operation
     * @return true if operation was cancelled, false if not found
     */
    bool cancel_operation(const std::string& op_id);

    /**
     * Check if operation is running
     * @param op_id Operation ID
     */
    bool is_running(const std::string& op_id) const;

    /**
     * Get operation status
     * @param op_id Operation ID
     * @return Status JSON with current progress
     */
    din::Json get_status(const std::string& op_id) const;

protected:
    /**
     * Send progress update to client
     * Called automatically by progress callback
     */
    void send_progress(
        const std::string& client_id,
        const std::string& op_id,
        uint64_t current,
        uint64_t total,
        const std::string& message,
        const din::Json& extra
    );

    /**
     * Send completion message to client
     */
    void send_complete(
        const std::string& client_id,
        const std::string& op_id,
        const din::Json& result
    );

    /**
     * Send error message to client
     */
    void send_error(
        const std::string& client_id,
        const std::string& op_id,
        const std::string& error_message,
        int error_code = -1
    );

private:
    struct OperationState {
        std::string client_id;
        std::atomic<bool> cancelled{false};
        std::atomic<uint64_t> current_progress{0};
        std::atomic<uint64_t> total_work{0};
        std::string last_message;
        std::chrono::system_clock::time_point start_time;
        bool completed{false};
    };

    std::string operation_id_;
    DaemonContext* daemon_ctx_;  // Access to all services
    WebSocketServer* ws_server_;  // Extracted from daemon_ctx

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<OperationState>> operations_;

    std::string generate_operation_id() const;
};

/**
 * Wallet Rescan Streaming Handler
 *
 * Implements wallet blockchain rescan with real-time progress updates.
 * Scans the blockchain from a given height, checks for relevant transactions,
 * and updates the wallet's UTXO set.
 */
class WalletRescanHandler : public StreamingRpcHandler {
public:
    WalletRescanHandler(DaemonContext* daemon_ctx);

    /**
     * Start wallet rescan operation
     * @param client_id WebSocket client for progress updates
     * @param start_height Block height to start scanning from
     * @return Operation status JSON
     */
    din::Json start_rescan(const std::string& client_id, uint64_t start_height = 0);

private:
    din::Json perform_rescan(uint64_t start_height, ProgressCallback progress_cb);
};

} // namespace rpc
} // namespace dinero
