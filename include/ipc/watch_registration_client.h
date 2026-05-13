#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Watch Registration Client (Phase 9.3: Bidirectional Oracle Communication)
// ═══════════════════════════════════════════════════════════════════════════
// Client for lightningd to register transaction watches with dinerod.
//
// ARCHITECTURE:
// - Runs in lightningd process
// - Connects to dinerod's WatchRegistrationServer
// - Sends WATCH_TX/UNWATCH_TX/CLEAR_WATCHES commands
// - Receives ACK/ERROR responses
//
// Data flow:
//   Lightning needs to watch funding_txid
//       ↓
//   WatchRegistrationClient::watchTx(txid)
//       ↓ IPC → /tmp/dinerod.sock
//   dinerod WatchRegistrationServer receives command
//       ↓
//   TransactionOracleClient.addWatch(txid)
//       ↓ (later, when TX confirms)
//   dinerod → Lightning: TX_CONFIRMED txid=<hex> height=<uint64>
//
// This completes the bidirectional oracle loop.
// ═══════════════════════════════════════════════════════════════════════════

#include <string>

namespace dinero {
namespace ipc {

/**
 * @class WatchRegistrationClient
 * @brief Client for lightningd to register transaction watches with dinerod
 *
 * Usage (lightningd side):
 * ```cpp
 * auto watch_client = std::make_unique<WatchRegistrationClient>("/tmp/dinerod.sock");
 *
 * // When opening channel, watch funding TX:
 * if (watch_client->watchTx(funding_txid)) {
 *     // Watch registered successfully
 * }
 *
 * // When channel closes, stop watching:
 * watch_client->unwatchTx(funding_txid);
 * ```
 *
 * Wire Protocol:
 * - WATCH_TX txid=<hex> → Registers txid
 * - UNWATCH_TX txid=<hex> → Unregisters txid
 * - CLEAR_WATCHES → Clears all watches
 * - Response: ACK or ERROR <msg>
 */
class WatchRegistrationClient {
public:
    /**
     * @brief Construct watch registration client
     * @param socket_path Path to dinerod watch registration socket (e.g., /tmp/dinerod.sock)
     */
    explicit WatchRegistrationClient(const std::string& socket_path);

    /**
     * @brief Destructor
     */
    ~WatchRegistrationClient();

    // Disable copy and move
    WatchRegistrationClient(const WatchRegistrationClient&) = delete;
    WatchRegistrationClient& operator=(const WatchRegistrationClient&) = delete;
    WatchRegistrationClient(WatchRegistrationClient&&) = delete;
    WatchRegistrationClient& operator=(WatchRegistrationClient&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // Watch Registration Operations
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Register a transaction to watch
     * @param txid Transaction ID (hex string, 64 chars)
     * @return true if registered successfully, false otherwise
     *
     * Sends WATCH_TX command to dinerod.
     * dinerod will notify via TX_CONFIRMED when this txid confirms.
     *
     * Use cases:
     * - Channel funding TX (watch for confirmation)
     * - Commitment TX (watch for force-close)
     * - Sweep TX (watch for confirmation)
     * - Justice TX (watch for confirmation)
     */
    bool watchTx(const std::string& txid);

    /**
     * @brief Unregister a transaction watch
     * @param txid Transaction ID (hex string, 64 chars)
     * @return true if unregistered successfully, false otherwise
     *
     * Sends UNWATCH_TX command to dinerod.
     * No more TX_CONFIRMED notifications for this txid.
     */
    bool unwatchTx(const std::string& txid);

    /**
     * @brief Clear all transaction watches
     * @return true if cleared successfully, false otherwise
     *
     * Sends CLEAR_WATCHES command to dinerod.
     * Useful for testing or full reset.
     */
    bool clearWatches();

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_socket_path;  // dinerod watch registration socket path

    // ═══════════════════════════════════════════════════════════════════════
    // Helper Methods
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Send command to dinerod and wait for ACK
     * @param command Command to send (without newline)
     * @return true if ACK received, false otherwise
     *
     * Wire format:
     * - Send: "<command>\n"
     * - Recv: "ACK\n" or "ERROR <msg>\n"
     *
     * Creates new connection for each command (stateless).
     * Timeout: 5 seconds.
     */
    bool sendCommand(const std::string& command);

    /**
     * @brief Connect to dinerod watch registration server
     * @return Socket file descriptor, or -1 on error
     *
     * Creates Unix socket connection to dinerod.
     * Timeout: 1 second.
     */
    int connect();

    /**
     * @brief Read response line from socket
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
