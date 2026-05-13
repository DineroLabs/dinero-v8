#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Time Oracle Client (Phase 9.2: Production Oracles)
// ═══════════════════════════════════════════════════════════════════════════
// Thin IPC adapter that forwards block height updates from L1 to lightningd.
//
// ARCHITECTURE CONTRACT:
// - This is an ADAPTER, not a service
// - Does NO logic
// - Does NO validation
// - Does NO state (except socket FD and current height)
// - Does NO retries
// - Does NO queries
//
// Responsibilities:
// - Serialize block height updates → IPC format
// - Send via Unix socket
// - Return success/failure
//
// Data flow:
//   L1 Chainstate → TimeOracleClient → IPC → lightningd
//
// If this class contains logic, the architecture is broken.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace dinero {
namespace ipc {

/**
 * @class TimeOracleClient
 * @brief Thin adapter forwarding block height updates to lightningd via IPC
 *
 * Usage (L1 side):
 * ```cpp
 * auto oracle = std::make_unique<TimeOracleClient>("/tmp/lightningd.sock");
 *
 * // On block connected:
 * oracle->sendBlockHeight(height, timestamp);
 * ```
 *
 * Wire Protocol (new):
 * - TIME_UPDATE height=<uint64> timestamp=<uint64>
 * - Response: ACK or ERROR <msg>
 *
 * Purpose:
 * - Lightning needs current block height for HTLC timeouts
 * - Lightning needs approximate timestamp for time-based logic
 * - This oracle provides both facts without Lightning querying chainstate
 */
class TimeOracleClient {
public:
    /**
     * @brief Construct oracle client with socket path
     * @param socket_path Path to lightningd Unix socket
     */
    explicit TimeOracleClient(const std::string& socket_path);

    /**
     * @brief Destructor - closes socket if connected
     */
    ~TimeOracleClient();

    // Disable copy and move
    TimeOracleClient(const TimeOracleClient&) = delete;
    TimeOracleClient& operator=(const TimeOracleClient&) = delete;
    TimeOracleClient(TimeOracleClient&&) = delete;
    TimeOracleClient& operator=(TimeOracleClient&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // Connection Management
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Connect to lightningd socket
     * @return true if connected, false otherwise
     *
     * Idempotent: Can be called multiple times.
     * Non-blocking: Returns immediately with success/failure.
     */
    bool connect();

    /**
     * @brief Check if connected to lightningd
     * @return true if socket is connected
     */
    bool isConnected() const { return m_socket_fd >= 0; }

    /**
     * @brief Disconnect from lightningd
     *
     * Idempotent: Safe to call multiple times.
     */
    void disconnect();

    // ═══════════════════════════════════════════════════════════════════════
    // Time Event Forwarding (Facts Only)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Forward block height update to lightningd
     * @param height Current block height
     * @param timestamp Block timestamp (Unix epoch)
     * @return true if sent successfully, false otherwise
     *
     * Serializes: TIME_UPDATE height=<height> timestamp=<timestamp>
     *
     * Semantics:
     * - Fire-and-forget (no retry)
     * - Idempotent (Lightning uses latest value)
     * - Non-blocking (returns immediately)
     *
     * Does NOT:
     * - Validate height
     * - Validate timestamp
     * - Check for duplicates
     * - Retry on failure
     * - Buffer messages
     */
    bool sendBlockHeight(uint64_t height, uint64_t timestamp);

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State (Minimal)
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_socket_path;  // lightningd socket path
    int m_socket_fd;            // Socket file descriptor (-1 = not connected)
    uint64_t m_last_height;     // Last height sent (for deduplication)

    // ═══════════════════════════════════════════════════════════════════════
    // Helper Methods (Pure Transport)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Send message to lightningd and wait for ACK
     * @param message Message to send (without newline)
     * @return true if ACK received, false otherwise
     *
     * Wire format:
     * - Send: "<message>\n"
     * - Recv: "ACK\n" or "ERROR <msg>\n"
     *
     * Timeout: 1 second (hard-coded)
     * Retry: None
     */
    bool sendMessage(const std::string& message);

    /**
     * @brief Read response line from socket
     * @param response Output buffer
     * @return true if line read, false on error/timeout
     *
     * Reads until '\n' or timeout.
     */
    bool readResponse(std::string& response);
};

} // namespace ipc
} // namespace dinero
