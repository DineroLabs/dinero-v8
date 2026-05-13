#pragma once

#include "lightning/lightning_transport.h"
#include "lightning/wallet_wire_protocol.h"
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <sys/types.h>  // ssize_t on POSIX + MinGW
// MSVC's <sys/types.h> does NOT define ssize_t. Use the project's net_compat
// shim, which provides `typedef ptrdiff_t ssize_t` under _WIN32.
#ifdef _WIN32
#  include "compat/net_compat.h"
#endif

// poll.h is POSIX-only; the socket-server implementation uses poll() on the
// recv path, but only the .cpp actually calls poll(). The header's only
// poll-related declaration is `ssize_t pollRecv(...)`, which doesn't need
// poll.h itself. On Windows we'd swap to WSAPoll() in the implementation if
// the gRPC server were ever ported (DISABLE_GRPC keeps it out of release
// builds for now).
#ifndef _WIN32
#include <poll.h>
#endif

// Forward declarations
struct DaemonContext;

namespace dinero {
namespace grpc_server {

/**
 * SocketWalletServer - Socket-based wallet service for Lightning
 *
 * Phase 5: Production Readiness - Server-side socket transport
 *
 * Implements Bitcoin-style socket server for wallet operations.
 * Handles wire protocol messages from WalletClient (socket mode).
 *
 * Architecture:
 *   lightningd (WalletClient) --[socket]--> dinerod (SocketWalletServer)
 *
 * Wire Protocol:
 *   - 12 message types (request/response pairs)
 *   - Bitcoin-style binary serialization
 *   - Variable-length encoding
 *   - Zero protobuf dependency
 *
 * Features:
 *   - Unix socket support (primary)
 *   - TCP socket support (fallback)
 *   - Multi-client handling
 *   - Thread-safe operation
 *   - Graceful shutdown
 *
 * Build Modes:
 *   - Dev mode: Socket server + gRPC server (both running)
 *   - Release mode: Socket server only (gRPC disabled)
 *
 * SECURITY: Binds to localhost Unix socket by default (trusted connection).
 */
class SocketWalletServer {
public:
    /**
     * Construct SocketWalletServer with DaemonContext
     *
     * @param daemon_ctx  DaemonContext with wallet and network state
     * @param address     Socket address (Unix path or TCP host:port)
     *                    Default: "127.0.0.1:50051" (same port as gRPC for compatibility)
     */
    explicit SocketWalletServer(
        DaemonContext* daemon_ctx,
        const std::string& address = "127.0.0.1:50051"
    );

    ~SocketWalletServer();

    /**
     * Start the socket server
     *
     * Binds socket, begins listening for connections.
     * Spawns acceptor thread for handling clients.
     *
     * @return true if server started successfully, false otherwise
     */
    bool Start();

    /**
     * Stop the socket server
     *
     * Gracefully shuts down server and disconnects all clients.
     * Waits for acceptor thread to finish.
     */
    void Stop();

    /**
     * Check if server is running
     *
     * @return true if server is running, false otherwise
     */
    bool IsRunning() const { return m_running; }

    /**
     * Get server address
     *
     * @return Server address string (e.g., "127.0.0.1:50051" or "/tmp/dinerod.sock")
     */
    std::string GetAddress() const { return m_address; }

    /**
     * Get last startup error
     *
     * Populated when Start() returns false.
     */
    std::string GetLastError() const { return m_last_error; }

private:
    DaemonContext* m_daemon_ctx;  // Access to wallet and network state
    std::string m_address;         // Socket address (Unix or TCP)
    std::string m_last_error;      // Last startup error (for diagnostics)
    std::atomic<bool> m_running;   // Server running flag
    int m_server_socket;           // Server socket file descriptor

    // Acceptor thread (accepts new client connections)
    std::unique_ptr<std::thread> m_acceptor_thread;

    // Client handler threads
    std::vector<std::unique_ptr<std::thread>> m_client_threads;

    // Track connected client sockets for graceful shutdown
    std::mutex m_client_sockets_mutex;
    std::vector<int> m_client_sockets;

    /**
     * Acceptor thread function
     *
     * Continuously accepts new client connections and spawns handler threads.
     */
    void acceptorLoop();

    /**
     * Client handler thread function
     *
     * Handles a single client connection:
     * 1. Receive request message
     * 2. Dispatch to handler
     * 3. Send response message
     * 4. Repeat until client disconnects
     *
     * @param client_socket  Client socket file descriptor
     */
    void handleClient(int client_socket);

    /**
     * Dispatch message to handler
     *
     * Routes incoming message to appropriate handler based on message type.
     *
     * @param message  Incoming message
     * @return Response message
     */
    lightning::LightningMessage dispatchMessage(const lightning::LightningMessage& message);

    // ===== Wire Protocol Message Handlers =====

    /**
     * Handle GetNetworkHRP request
     *
     * Returns network HRP (Human Readable Part) for bech32 addresses.
     */
    std::vector<uint8_t> handleGetNetworkHRP(const std::vector<uint8_t>& request_payload);

    /**
     * Handle ListUnspentUTXOs request
     *
     * Lists unspent UTXOs with confirmation filter.
     */
    std::vector<uint8_t> handleListUnspentUTXOs(const std::vector<uint8_t>& request_payload);

    /**
     * Handle DeriveLightningKey request
     *
     * Derives Lightning-specific keys from HD wallet.
     */
    std::vector<uint8_t> handleDeriveLightningKey(const std::vector<uint8_t>& request_payload);

    /**
     * Handle ComputeTaprootSighash request
     *
     * Computes BIP-341 Taproot sighash for transaction signing.
     */
    std::vector<uint8_t> handleComputeTaprootSighash(const std::vector<uint8_t>& request_payload);

    /**
     * Handle GetNewChangeAddress request
     *
     * Gets new change address from HD wallet.
     */
    std::vector<uint8_t> handleGetNewChangeAddress(const std::vector<uint8_t>& request_payload);

    /**
     * Handle DeriveKeyForScriptPubKey request
     *
     * Derives private key for a given scriptPubKey.
     */
    std::vector<uint8_t> handleDeriveKeyForScriptPubKey(const std::vector<uint8_t>& request_payload);

    /**
     * Poll-guarded recv — replaces raw recv(MSG_WAITALL) for clean shutdown.
     * Uses poll() with 1s timeout, re-checks m_running between polls.
     * Returns total bytes read, 0 on clean close, -1 on error/shutdown.
     */
    ssize_t pollRecv(int fd, void* buf, size_t len);

    /**
     * Create error response
     *
     * @param error_message  Error message string
     * @return Error response message
     */
    lightning::LightningMessage createErrorResponse(const std::string& error_message);
};

} // namespace grpc_server
} // namespace dinero
