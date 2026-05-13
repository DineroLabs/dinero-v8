#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Chain Oracle Client (Phase 9.2: Production Oracles)
// ═══════════════════════════════════════════════════════════════════════════
// Thin IPC adapter that forwards chain events from L1 to lightningd.
//
// ARCHITECTURE CONTRACT:
// - This is an ADAPTER, not a service
// - Does NO logic
// - Does NO validation
// - Does NO state (except socket FD)
// - Does NO retries
// - Does NO queries
//
// Responsibilities:
// - Serialize block events → IPC format
// - Send via Unix socket
// - Return success/failure
//
// Data flow:
//   L1 Chainstate → ChainOracleClient → IPC → lightningd
//
// If this class contains logic, the architecture is broken.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace dinero {
namespace ipc {

/**
 * @class ChainOracleClient
 * @brief Thin adapter forwarding chain events to lightningd via IPC
 *
 * Usage (L1 side):
 * ```cpp
 * auto oracle = std::make_unique<ChainOracleClient>("/tmp/lightningd.sock");
 *
 * // On block connected:
 * oracle->sendBlockConnected(height, block_hash);
 *
 * // On block disconnected:
 * oracle->sendBlockDisconnected(height, block_hash);
 * ```
 *
 * Wire Protocol (existing):
 * - BLOCK_CONNECTED height=<uint64> hash=<hex>
 * - BLOCK_DISCONNECTED height=<uint64> hash=<hex>
 * - Response: ACK or ERROR <msg>
 */
class ChainOracleClient {
public:
    /**
     * @brief Construct oracle client with socket path
     * @param socket_path Path to lightningd Unix socket
     */
    explicit ChainOracleClient(const std::string& socket_path);

    /**
     * @brief Destructor - closes socket if connected
     */
    ~ChainOracleClient();

    // Disable copy and move
    ChainOracleClient(const ChainOracleClient&) = delete;
    ChainOracleClient& operator=(const ChainOracleClient&) = delete;
    ChainOracleClient(ChainOracleClient&&) = delete;
    ChainOracleClient& operator=(ChainOracleClient&&) = delete;

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
    // Chain Event Forwarding (Facts Only)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Forward block connected event to lightningd
     * @param height Block height
     * @param block_hash Block hash (hex string)
     * @return true if sent successfully, false otherwise
     *
     * Serializes: BLOCK_CONNECTED height=<height> hash=<block_hash>
     *
     * Semantics:
     * - Fire-and-forget (no retry)
     * - Idempotent (Lightning handles duplicates)
     * - Non-blocking (returns immediately)
     *
     * Does NOT:
     * - Validate height
     * - Validate block_hash
     * - Check for duplicates
     * - Retry on failure
     * - Buffer messages
     */
    bool sendBlockConnected(uint64_t height, const std::string& block_hash);

    /**
     * @brief Forward block disconnected event to lightningd
     * @param height Block height being disconnected
     * @param block_hash Block hash (hex string)
     * @return true if sent successfully, false otherwise
     *
     * Serializes: BLOCK_DISCONNECTED height=<height> hash=<block_hash>
     *
     * Semantics: Same as sendBlockConnected()
     */
    bool sendBlockDisconnected(uint64_t height, const std::string& block_hash);

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State (Minimal)
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_socket_path;  // lightningd socket path
    int m_socket_fd;            // Socket file descriptor (-1 = not connected)

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
