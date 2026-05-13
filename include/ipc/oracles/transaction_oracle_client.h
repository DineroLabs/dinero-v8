#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Transaction Oracle Client (Phase 9.2: Production Oracles)
// ═══════════════════════════════════════════════════════════════════════════
// Thin IPC adapter that watches for specific transactions and notifies lightningd
// when they're confirmed.
//
// ARCHITECTURE CONTRACT:
// - This is an ADAPTER, not a service
// - Does NO transaction logic
// - Does NO validation
// - Does MINIMAL state (watch list only)
// - Does NO retries
// - Does NO block scanning (caller provides transactions)
//
// Responsibilities:
// - Maintain watch list of txids
// - Check if transaction is in watch list
// - Serialize transaction confirmations → IPC format
// - Send via Unix socket
//
// Data flow:
//   L1 Chainstate detects block → extracts TXs → TransactionOracleClient checks watches
//   → sends TX_CONFIRMED for matches → IPC → lightningd
//
// If this class contains logic beyond "is txid in set?", the architecture is broken.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <unordered_set>
#include <mutex>

namespace dinero {
namespace ipc {

/**
 * @class TransactionOracleClient
 * @brief Thin adapter watching specific transactions and notifying lightningd
 *
 * Usage (L1 side):
 * ```cpp
 * auto oracle = std::make_unique<TransactionOracleClient>("/tmp/lightningd.sock");
 *
 * // Lightning registers interest in specific txids:
 * oracle->addWatch("funding_txid");
 * oracle->addWatch("commitment_txid");
 *
 * // On block connected, check each transaction:
 * for (const auto& tx : block.transactions) {
 *     if (oracle->checkAndNotify(tx.GetHash(), height)) {
 *         // Match found, TX_CONFIRMED sent
 *     }
 * }
 * ```
 *
 * Wire Protocol (existing):
 * - TX_CONFIRMED txid=<hex> height=<uint64>
 * - Response: ACK or ERROR <msg>
 *
 * Purpose:
 * - Lightning watches for funding TX confirmations
 * - Lightning watches for commitment TX confirmations (force-close detection)
 * - Lightning watches for sweep TX confirmations
 * - Lightning watches for justice TX confirmations
 */
class TransactionOracleClient {
public:
    /**
     * @brief Construct oracle client with socket path
     * @param socket_path Path to lightningd Unix socket
     */
    explicit TransactionOracleClient(const std::string& socket_path);

    /**
     * @brief Destructor - closes socket if connected
     */
    ~TransactionOracleClient();

    // Disable copy and move
    TransactionOracleClient(const TransactionOracleClient&) = delete;
    TransactionOracleClient& operator=(const TransactionOracleClient&) = delete;
    TransactionOracleClient(TransactionOracleClient&&) = delete;
    TransactionOracleClient& operator=(TransactionOracleClient&&) = delete;

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
    // Watch List Management (Minimal State)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Add transaction to watch list
     * @param txid Transaction ID (hex string, 64 chars)
     *
     * Thread-safe: Can be called from Lightning IPC handler
     * Idempotent: Adding same txid multiple times is no-op
     */
    void addWatch(const std::string& txid);

    /**
     * @brief Remove transaction from watch list
     * @param txid Transaction ID (hex string, 64 chars)
     *
     * Thread-safe: Can be called from Lightning IPC handler
     * Idempotent: Removing non-existent txid is no-op
     */
    void removeWatch(const std::string& txid);

    /**
     * @brief Clear all watches (for testing/reset)
     */
    void clearWatches();

    /**
     * @brief Get number of transactions being watched
     * @return size_t Watch list size
     */
    size_t getWatchCount() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Transaction Confirmation Forwarding (Facts Only)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Check if transaction is watched, and notify if match
     * @param txid Transaction ID (hex string)
     * @param height Block height where confirmed
     * @return true if txid was watched and notification sent
     *
     * Algorithm:
     * 1. Check if txid in watch set (O(1) hash lookup)
     * 2. If yes, send TX_CONFIRMED message via IPC
     * 3. Return true if match found
     *
     * Does NOT:
     * - Validate txid format
     * - Validate height
     * - Retry on failure
     * - Remove from watch list (Lightning decides when to unwatch)
     * - Interpret meaning of transaction
     */
    bool checkAndNotify(const std::string& txid, uint64_t height);

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State (Minimal)
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_socket_path;              // lightningd socket path
    int m_socket_fd;                        // Socket file descriptor (-1 = not connected)
    std::unordered_set<std::string> m_watched_txids;  // Txids being watched
    mutable std::mutex m_watch_mutex;       // Protects m_watched_txids

    // ═══════════════════════════════════════════════════════════════════════
    // Helper Methods (Pure Transport)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Send TX_CONFIRMED message to lightningd
     * @param txid Transaction ID
     * @param height Block height
     * @return true if ACK received, false otherwise
     */
    bool sendTxConfirmed(const std::string& txid, uint64_t height);

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
