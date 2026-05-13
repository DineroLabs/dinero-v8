// Copyright (c) 2024 The Dinero Core developers
// Distributed under the MIT software license

#ifndef DINERO_LIGHTNING_TRANSPORT_H
#define DINERO_LIGHTNING_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dinero {
namespace lightning {

/**
 * Lightning Transport Abstraction (Bitcoin Core Style)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Purpose: Decouple Lightning IPC from gRPC dependency
 *
 * Architecture:
 *   Development builds:  GrpcLightningTransport (fast iteration)
 *   Release builds:      SocketLightningTransport (zero dependencies)
 *
 * This is how Bitcoin Core would implement Lightning IPC:
 * - Development: Use whatever is convenient (gRPC, ZeroMQ, etc.)
 * - Release: Raw sockets with custom protocol
 * - Zero dependency on external frameworks
 *
 * Binary compatibility:
 *   Dev build:   Links gRPC/protobuf/abseil (80+ Homebrew dylibs)
 *   Release:     Links nothing (fully static, exchange-ready)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Message envelope for Lightning IPC
 *
 * Wire format (Bitcoin-style):
 *   [4 bytes] message_type (uint32_t, network byte order)
 *   [4 bytes] payload_size (uint32_t, network byte order)
 *   [N bytes] payload (serialized data)
 *
 * Message types:
 *   0x0001 = WALLET_BALANCE_REQUEST
 *   0x0002 = WALLET_BALANCE_RESPONSE
 *   0x0003 = UTXO_REQUEST
 *   0x0004 = UTXO_RESPONSE
 *   0x0005 = SEND_TRANSACTION_REQUEST
 *   0x0006 = SEND_TRANSACTION_RESPONSE
 *   ... (extend as needed)
 */
struct LightningMessage {
    uint32_t message_type;           // Message type ID
    std::vector<uint8_t> payload;    // Serialized payload

    // Convenience constructors
    LightningMessage() : message_type(0) {}
    LightningMessage(uint32_t type, std::vector<uint8_t> data)
        : message_type(type), payload(std::move(data)) {}
};

/**
 * Transport abstraction interface
 *
 * Implementation note:
 *   - send() returns true on success, false on error
 *   - recv() blocks until message arrives or connection fails
 *   - Both methods are thread-safe (implementations must ensure this)
 */
class LightningTransport {
public:
    virtual ~LightningTransport() = default;

    /**
     * Send a message to the peer
     *
     * @param msg Message to send
     * @return true on success, false on error
     *
     * Thread-safety: Must be safe to call from multiple threads
     */
    virtual bool send(const LightningMessage& msg) = 0;

    /**
     * Receive a message from the peer
     *
     * @param msg [out] Received message
     * @return true on success, false on connection error
     *
     * Thread-safety: Must be safe to call from multiple threads
     * Blocking: This call blocks until a message arrives
     */
    virtual bool recv(LightningMessage& msg) = 0;

    /**
     * Check if connection is alive
     *
     * @return true if connected, false otherwise
     *
     * Thread-safety: Must be safe to call from multiple threads
     */
    virtual bool is_connected() const = 0;

    /**
     * Close the connection
     *
     * Thread-safety: Must be safe to call from multiple threads
     */
    virtual void close() = 0;

    /**
     * Accept incoming connection (server-side only)
     *
     * @return true on success, false on error
     *
     * Thread-safety: Must be safe to call from multiple threads
     * Blocking: This call blocks until a connection arrives
     *
     * Note: This is a no-op for client-side transports
     */
    virtual bool accept() { return true; }  // Default: no-op for clients
};

/**
 * Factory for creating transport instances
 *
 * Usage:
 *   auto transport = LightningTransportFactory::create(endpoint);
 */
class LightningTransportFactory {
public:
    /**
     * Create a transport instance
     *
     * @param endpoint Connection endpoint (format depends on build mode)
     *                 Dev mode:     "localhost:50051" (gRPC)
     *                 Release mode: "unix:///tmp/dinerod.sock" or "tcp://127.0.0.1:8332"
     * @return Transport instance (nullptr on error)
     *
     * Implementation selection:
     *   DISABLE_GRPC defined: Returns SocketLightningTransport
     *   DISABLE_GRPC not defined: Returns GrpcLightningTransport
     */
    static std::unique_ptr<LightningTransport> create(const std::string& endpoint);

    /**
     * Create a server-side transport listener
     *
     * @param endpoint Bind endpoint
     * @return Transport instance (nullptr on error)
     */
    static std::unique_ptr<LightningTransport> create_server(const std::string& endpoint);
};

} // namespace lightning
} // namespace dinero

#endif // DINERO_LIGHTNING_TRANSPORT_H
