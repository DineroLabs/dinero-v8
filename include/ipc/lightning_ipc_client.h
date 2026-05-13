#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning IPC Client (L1 → lightningd Communication)
// ═══════════════════════════════════════════════════════════════════════════
// Sends events from L1 services (dinerod, watchtower) to lightningd via IPC.
//
// ARCHITECTURAL CONTRACT:
// - NO Lightning headers (channel_manager, htlc, etc.)
// - Sends facts only (txid, height, hash)
// - No interpretation (watchtower doesn't know what "breach" means)
// - No retry logic (idempotency handles failures)
//
// WIRE PROTOCOL (text-based, newline-delimited):
// - BLOCK_CONNECTED height=<uint64> hash=<hex>
// - BLOCK_DISCONNECTED height=<uint64> hash=<hex>
// - TX_CONFIRMED txid=<hex> height=<uint64>
//
// Phase 9: L1-adjacent services push facts to Lightning via IPC.
// ═══════════════════════════════════════════════════════════════════════════

#include <string>
#include <cstdint>

namespace dinero {
namespace ipc {

/**
 * @class LightningIPCClient
 * @brief IPC client for sending events to lightningd
 *
 * Responsibilities:
 * - Open Unix socket connection to lightningd
 * - Serialize events to wire format
 * - Send messages
 * - Handle connection failures gracefully
 *
 * Non-responsibilities:
 * - Lightning logic (doesn't know about channels, HTLCs, etc.)
 * - Retry policy (caller decides)
 * - Message ordering (caller's responsibility)
 * - Idempotency (Lightning's responsibility)
 */
class LightningIPCClient {
public:
    /**
     * @brief Construct IPC client
     * @param socket_path Path to lightningd Unix socket (e.g., "/tmp/lightningd.sock")
     */
    explicit LightningIPCClient(const std::string& socket_path);
    ~LightningIPCClient();

    /**
     * @brief Connect to lightningd
     * @return true if connected successfully
     */
    bool connect();

    /**
     * @brief Disconnect from lightningd
     */
    void disconnect();

    /**
     * @brief Check if connected
     * @return true if socket is connected
     */
    bool isConnected() const;

    /**
     * @brief Send BLOCK_CONNECTED event
     * @param height Block height
     * @param block_hash Block hash (hex string)
     * @return true if sent successfully
     */
    bool sendBlockConnected(uint64_t height, const std::string& block_hash);

    /**
     * @brief Send BLOCK_DISCONNECTED event
     * @param height Block height
     * @param block_hash Block hash (hex string)
     * @return true if sent successfully
     */
    bool sendBlockDisconnected(uint64_t height, const std::string& block_hash);

    /**
     * @brief Send TX_CONFIRMED event
     * @param txid Transaction ID (hex string)
     * @param height Block height where tx was confirmed
     * @return true if sent successfully
     */
    bool sendTxConfirmed(const std::string& txid, uint64_t height);

private:
    /**
     * @brief Send raw message to lightningd
     * @param message Message to send (will be newline-terminated)
     * @return true if sent successfully
     */
    bool sendMessage(const std::string& message);

    /**
     * @brief Read response from lightningd
     * @return Response string ("ACK" or "ERROR ...")
     */
    std::string readResponse();

    std::string m_socket_path;
    int m_fd;  // Socket file descriptor (-1 if not connected)
};

} // namespace ipc
} // namespace dinero
