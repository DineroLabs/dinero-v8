#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Watch Registration Server (Phase 9.3: Bidirectional Oracle Communication)
// ═══════════════════════════════════════════════════════════════════════════
// IPC server that allows lightningd to register transaction watches with dinerod.
//
// ARCHITECTURE:
// - Runs in dinerod process
// - Listens on Unix socket (e.g., /tmp/dinerod.sock)
// - Accepts commands from lightningd
// - Forwards watch operations to TransactionOracleClient
// - Returns ACK/ERROR responses
//
// Data flow:
//   lightningd → IPC → WatchRegistrationServer → TransactionOracleClient
//
// Wire Protocol:
//   Request:
//     WATCH_TX txid=<hex>
//     UNWATCH_TX txid=<hex>
//     CLEAR_WATCHES
//   Response:
//     ACK
//     ERROR <message>
//
// This completes the bidirectional oracle loop:
//   Lightning → dinerod: WATCH_TX (register interest)
//   dinerod → Lightning: TX_CONFIRMED (notify when matched)
// ═══════════════════════════════════════════════════════════════════════════

#include <string>
#include <thread>
#include <atomic>
#include <memory>

// Forward declarations
namespace dinero {
namespace ipc {
    class TransactionOracleClient;
}
}

namespace dinero {
namespace ipc {

/**
 * @class WatchRegistrationServer
 * @brief IPC server for Lightning to register transaction watches
 *
 * Usage (dinerod side):
 * ```cpp
 * auto tx_oracle = std::make_shared<TransactionOracleClient>(...);
 * auto watch_server = std::make_unique<WatchRegistrationServer>(
 *     "/tmp/dinerod.sock",
 *     tx_oracle
 * );
 * watch_server->start();  // Runs in background thread
 *
 * // Later:
 * watch_server->stop();
 * ```
 *
 * Wire Protocol:
 * - WATCH_TX txid=<hex> → Adds txid to watch list
 * - UNWATCH_TX txid=<hex> → Removes txid from watch list
 * - CLEAR_WATCHES → Clears all watches
 * - Response: ACK or ERROR <msg>
 */
class WatchRegistrationServer {
public:
    /**
     * @brief Construct watch registration server
     * @param socket_path Path to Unix socket (e.g., /tmp/dinerod.sock)
     * @param tx_oracle Transaction oracle client to forward watch commands to
     */
    WatchRegistrationServer(
        const std::string& socket_path,
        std::shared_ptr<TransactionOracleClient> tx_oracle
    );

    /**
     * @brief Destructor - stops server and cleans up
     */
    ~WatchRegistrationServer();

    // Disable copy and move
    WatchRegistrationServer(const WatchRegistrationServer&) = delete;
    WatchRegistrationServer& operator=(const WatchRegistrationServer&) = delete;
    WatchRegistrationServer(WatchRegistrationServer&&) = delete;
    WatchRegistrationServer& operator=(WatchRegistrationServer&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // Lifecycle Management
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Start IPC server (runs in background thread)
     * @return true if started successfully, false otherwise
     *
     * Creates Unix socket, binds, listens, and spawns accept thread.
     * Non-blocking: Returns immediately after thread spawn.
     */
    bool start();

    /**
     * @brief Stop IPC server
     *
     * Signals server thread to exit and joins.
     * Closes all client connections.
     * Idempotent: Safe to call multiple times.
     */
    void stop();

    /**
     * @brief Check if server is running
     * @return true if server thread is active
     */
    bool isRunning() const { return m_running.load(); }

    // ═══════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════

    struct Stats {
        uint64_t connections_accepted = 0;
        uint64_t watch_commands_received = 0;
        uint64_t unwatch_commands_received = 0;
        uint64_t clear_commands_received = 0;
        uint64_t errors = 0;
    };

    Stats getStats() const;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_socket_path;
    std::shared_ptr<TransactionOracleClient> m_tx_oracle;  // Shared ownership with daemon

    int m_server_fd;                        // Server socket file descriptor
    std::atomic<bool> m_running;            // Server running flag
    std::unique_ptr<std::thread> m_thread;  // Accept thread

    // Statistics (atomic for thread safety)
    std::atomic<uint64_t> m_connections_accepted{0};
    std::atomic<uint64_t> m_watch_commands{0};
    std::atomic<uint64_t> m_unwatch_commands{0};
    std::atomic<uint64_t> m_clear_commands{0};
    std::atomic<uint64_t> m_errors{0};

    // ═══════════════════════════════════════════════════════════════════════
    // Server Implementation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Server accept loop (runs in background thread)
     *
     * Algorithm:
     * 1. Accept client connection
     * 2. Read message from client
     * 3. Parse command
     * 4. Forward to TransactionOracleClient
     * 5. Send ACK/ERROR response
     * 6. Close client connection
     * 7. Repeat
     */
    void serverLoop();

    /**
     * @brief Handle single client connection
     * @param client_fd Client socket file descriptor
     *
     * Reads one command, processes it, sends response, closes connection.
     */
    void handleClient(int client_fd);

    /**
     * @brief Parse and execute watch command
     * @param message Command message (WATCH_TX txid=..., etc.)
     * @return Response string (ACK or ERROR <msg>)
     */
    std::string processCommand(const std::string& message);

    /**
     * @brief Parse WATCH_TX command
     * @param message Command message
     * @param txid Output: extracted txid
     * @return true if parsed successfully
     */
    bool parseWatchTx(const std::string& message, std::string& txid);

    /**
     * @brief Parse UNWATCH_TX command
     * @param message Command message
     * @param txid Output: extracted txid
     * @return true if parsed successfully
     */
    bool parseUnwatchTx(const std::string& message, std::string& txid);

    /**
     * @brief Read line from socket
     * @param fd Socket file descriptor
     * @param line Output: line read (without newline)
     * @return true if read successfully
     */
    bool readLine(int fd, std::string& line);

    /**
     * @brief Write line to socket
     * @param fd Socket file descriptor
     * @param line Line to write (newline added automatically)
     * @return true if written successfully
     */
    bool writeLine(int fd, const std::string& line);
};

} // namespace ipc
} // namespace dinero
