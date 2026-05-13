#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning IPC Server (Phase 8.4: IPC Transport)
// ═══════════════════════════════════════════════════════════════════════════
// Unix domain socket server for receiving L1 events.
//
// WIRE PROTOCOL (text-based, newline-delimited):
// - BLOCK_CONNECTED height=<uint64> hash=<hex>
// - BLOCK_DISCONNECTED height=<uint64> hash=<hex>
// - TX_CONFIRMED txid=<hex> height=<uint64>
//
// RESPONSE:
// - ACK (event processed successfully)
// - ERROR <message> (event rejected or persistence failed)
//
// ARCHITECTURAL CONTRACT:
// - NO L1 headers (daemon/chainstate/wallet)
// - Receives events, calls ILightningEventSink
// - Returns ACK/ERROR synchronously
// - Single-threaded event delivery
//
// Phase 8.4: Transport only, no Lightning logic.
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_event_sink.h"
#include <string>
#include <atomic>

namespace dinero {
namespace lightning {

/**
 * @class LightningIPCServer
 * @brief Unix domain socket server for Lightning event injection
 *
 * Responsibilities:
 * - Listen on Unix socket
 * - Parse incoming messages
 * - Call ILightningEventSink methods
 * - Return ACK/ERROR responses
 *
 * Non-responsibilities:
 * - Lightning logic (delegate to event sink)
 * - Message ordering (caller's responsibility)
 * - Retry policy (caller's responsibility)
 * - Persistence (event sink's responsibility)
 */
class LightningIPCServer {
public:
    /**
     * @brief Construct IPC server
     * @param event_sink Event sink to receive parsed events
     * @param socket_path Path to Unix domain socket (e.g., "/tmp/lightningd.sock")
     */
    explicit LightningIPCServer(ILightningEventSink* event_sink, const std::string& socket_path);
    ~LightningIPCServer();

    /**
     * @brief Start IPC server (creates socket, listens)
     * @return true if server started successfully
     */
    bool start();

    /**
     * @brief Stop IPC server (closes socket)
     */
    void stop();

    /**
     * @brief Process incoming IPC messages (blocking)
     * Runs in event loop, returns when stop() is called.
     */
    void run();

private:
    /**
     * @brief Parse and handle a single IPC message
     * @param message Raw message from socket
     * @return Response to send (ACK or ERROR)
     */
    std::string handleMessage(const std::string& message);

    /**
     * @brief Parse BLOCK_CONNECTED message
     */
    bool parseBlockConnected(const std::string& message, uint64_t& height, std::string& hash);

    /**
     * @brief Parse BLOCK_DISCONNECTED message
     */
    bool parseBlockDisconnected(const std::string& message, uint64_t& height, std::string& hash);

    /**
     * @brief Parse TX_CONFIRMED message
     */
    bool parseTxConfirmed(const std::string& message, std::string& txid, uint64_t& height);

    ILightningEventSink* m_event_sink;  // Not owned
    std::string m_socket_path;
    int m_listen_fd;
    int m_client_fd;
    std::atomic<bool> m_running{false};
};

} // namespace lightning
} // namespace dinero
