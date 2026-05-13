// ═══════════════════════════════════════════════════════════════════════════
// Lightning IPC Server Implementation (Phase 8.4: IPC Transport)
// ═══════════════════════════════════════════════════════════════════════════

#include "ipc_server.h"

#ifndef _WIN32  // Unix domain socket IPC — not available on Windows

#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace dinero {
namespace lightning {

LightningIPCServer::LightningIPCServer(ILightningEventSink* event_sink, const std::string& socket_path)
    : m_event_sink(event_sink)
    , m_socket_path(socket_path)
    , m_listen_fd(-1)
    , m_client_fd(-1)
{
}

LightningIPCServer::~LightningIPCServer() {
    stop();
}

bool LightningIPCServer::start() {
    // Remove existing socket file if present
    unlink(m_socket_path.c_str());

    // Create Unix domain socket
    m_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        std::cerr << "[IPC] ERROR: Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Bind socket to path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[IPC] ERROR: Failed to bind socket: " << strerror(errno) << std::endl;
        close(m_listen_fd);
        m_listen_fd = -1;
        return false;
    }

    // Listen for connections (backlog = 1)
    if (listen(m_listen_fd, 1) < 0) {
        std::cerr << "[IPC] ERROR: Failed to listen: " << strerror(errno) << std::endl;
        close(m_listen_fd);
        m_listen_fd = -1;
        return false;
    }

    m_running = true;
    std::cout << "[IPC] Server listening on " << m_socket_path << std::endl;
    return true;
}

void LightningIPCServer::stop() {
    m_running = false;

    if (m_client_fd >= 0) {
        close(m_client_fd);
        m_client_fd = -1;
    }

    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
    }

    unlink(m_socket_path.c_str());
    std::cout << "[IPC] Server stopped" << std::endl;
}

void LightningIPCServer::run() {
    while (m_running) {
        // Accept client connection (blocking)
        std::cout << "[IPC] Waiting for client connection..." << std::endl;
        m_client_fd = accept(m_listen_fd, nullptr, nullptr);
        if (m_client_fd < 0) {
            if (m_running) {
                std::cerr << "[IPC] ERROR: Failed to accept: " << strerror(errno) << std::endl;
            }
            break;
        }

        std::cout << "[IPC] Client connected" << std::endl;

        // Process messages from client
        char buffer[4096];
        std::string message_buffer;

        while (m_running) {
            ssize_t n = read(m_client_fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                if (n < 0) {
                    std::cerr << "[IPC] ERROR: Read failed: " << strerror(errno) << std::endl;
                }
                std::cout << "[IPC] Client disconnected" << std::endl;
                break;
            }

            buffer[n] = '\0';
            message_buffer += buffer;

            // Process complete messages (newline-delimited)
            size_t pos;
            while ((pos = message_buffer.find('\n')) != std::string::npos) {
                std::string message = message_buffer.substr(0, pos);
                message_buffer.erase(0, pos + 1);

                if (!message.empty()) {
                    // Handle message and send response
                    std::string response = handleMessage(message);
                    response += "\n";
                    write(m_client_fd, response.c_str(), response.size());
                }
            }
        }

        close(m_client_fd);
        m_client_fd = -1;
    }
}

std::string LightningIPCServer::handleMessage(const std::string& message) {
    std::cout << "[IPC] Received: " << message << std::endl;

    // Parse message type
    if (message.rfind("BLOCK_CONNECTED ", 0) == 0) {
        uint64_t height;
        std::string hash;
        if (!parseBlockConnected(message, height, hash)) {
            return "ERROR Invalid BLOCK_CONNECTED format";
        }

        BlockConnectedEvent event{height, hash};
        m_event_sink->onBlockConnected(event);
        return "ACK";
    }
    else if (message.rfind("BLOCK_DISCONNECTED ", 0) == 0) {
        uint64_t height;
        std::string hash;
        if (!parseBlockDisconnected(message, height, hash)) {
            return "ERROR Invalid BLOCK_DISCONNECTED format";
        }

        BlockDisconnectedEvent event{height, hash};
        m_event_sink->onBlockDisconnected(event);
        return "ACK";
    }
    else if (message.rfind("TX_CONFIRMED ", 0) == 0) {
        std::string txid;
        uint64_t height;
        if (!parseTxConfirmed(message, txid, height)) {
            return "ERROR Invalid TX_CONFIRMED format";
        }

        TransactionConfirmedEvent event{txid, height};
        m_event_sink->onTransactionConfirmed(event);
        return "ACK";
    }
    else {
        return "ERROR Unknown message type";
    }
}

bool LightningIPCServer::parseBlockConnected(const std::string& message, uint64_t& height, std::string& hash) {
    // Format: BLOCK_CONNECTED height=<uint64> hash=<hex>
    std::istringstream iss(message);
    std::string cmd, height_str, hash_str;

    if (!(iss >> cmd >> height_str >> hash_str)) {
        return false;
    }

    // Parse height=<value>
    if (height_str.rfind("height=", 0) != 0) {
        return false;
    }
    height = std::stoull(height_str.substr(7));

    // Parse hash=<value>
    if (hash_str.rfind("hash=", 0) != 0) {
        return false;
    }
    hash = hash_str.substr(5);

    return true;
}

bool LightningIPCServer::parseBlockDisconnected(const std::string& message, uint64_t& height, std::string& hash) {
    // Format: BLOCK_DISCONNECTED height=<uint64> hash=<hex>
    std::istringstream iss(message);
    std::string cmd, height_str, hash_str;

    if (!(iss >> cmd >> height_str >> hash_str)) {
        return false;
    }

    // Parse height=<value>
    if (height_str.rfind("height=", 0) != 0) {
        return false;
    }
    height = std::stoull(height_str.substr(7));

    // Parse hash=<value>
    if (hash_str.rfind("hash=", 0) != 0) {
        return false;
    }
    hash = hash_str.substr(5);

    return true;
}

bool LightningIPCServer::parseTxConfirmed(const std::string& message, std::string& txid, uint64_t& height) {
    // Format: TX_CONFIRMED txid=<hex> height=<uint64>
    std::istringstream iss(message);
    std::string cmd, txid_str, height_str;

    if (!(iss >> cmd >> txid_str >> height_str)) {
        return false;
    }

    // Parse txid=<value>
    if (txid_str.rfind("txid=", 0) != 0) {
        return false;
    }
    txid = txid_str.substr(5);

    // Parse height=<value>
    if (height_str.rfind("height=", 0) != 0) {
        return false;
    }
    height = std::stoull(height_str.substr(7));

    return true;
}

} // namespace lightning
} // namespace dinero

#endif // !_WIN32
