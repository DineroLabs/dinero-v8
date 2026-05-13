// ═══════════════════════════════════════════════════════════════════════════
// Lightning IPC Client Implementation (L1 → lightningd)
// ═══════════════════════════════════════════════════════════════════════════

#include "ipc/lightning_ipc_client.h"
#include "common/logger.h"

#ifndef _WIN32  // Unix domain socket IPC — not available on Windows

#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace dinero {
namespace ipc {

LightningIPCClient::LightningIPCClient(const std::string& socket_path)
    : m_socket_path(socket_path)
    , m_fd(-1)
{
}

LightningIPCClient::~LightningIPCClient() {
    disconnect();
}

bool LightningIPCClient::connect() {
    if (m_fd >= 0) {
        // Already connected
        return true;
    }

    // Create Unix domain socket
    m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) {
        g_logger.error("[IPC Client] Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }

    // Connect to lightningd
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        g_logger.warning("[IPC Client] Failed to connect to " + m_socket_path + ": " +
                        std::string(strerror(errno)));
        close(m_fd);
        m_fd = -1;
        return false;
    }

    g_logger.info("[IPC Client] Connected to lightningd at " + m_socket_path);
    return true;
}

void LightningIPCClient::disconnect() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        g_logger.info("[IPC Client] Disconnected from lightningd");
    }
}

bool LightningIPCClient::isConnected() const {
    return m_fd >= 0;
}

bool LightningIPCClient::sendBlockConnected(uint64_t height, const std::string& block_hash) {
    // Format: BLOCK_CONNECTED height=<uint64> hash=<hex>
    std::string message = "BLOCK_CONNECTED height=" + std::to_string(height) +
                         " hash=" + block_hash;
    return sendMessage(message);
}

bool LightningIPCClient::sendBlockDisconnected(uint64_t height, const std::string& block_hash) {
    // Format: BLOCK_DISCONNECTED height=<uint64> hash=<hex>
    std::string message = "BLOCK_DISCONNECTED height=" + std::to_string(height) +
                         " hash=" + block_hash;
    return sendMessage(message);
}

bool LightningIPCClient::sendTxConfirmed(const std::string& txid, uint64_t height) {
    // Format: TX_CONFIRMED txid=<hex> height=<uint64>
    std::string message = "TX_CONFIRMED txid=" + txid +
                         " height=" + std::to_string(height);
    return sendMessage(message);
}

bool LightningIPCClient::sendMessage(const std::string& message) {
    // Ensure connected
    if (!isConnected()) {
        if (!connect()) {
            // Connection failed - this is OK, Lightning is optional
            g_logger.debug("[IPC Client] Cannot send message - not connected");
            return false;
        }
    }

    // Send message (newline-terminated)
    std::string full_message = message + "\n";
    ssize_t sent = write(m_fd, full_message.c_str(), full_message.size());

    if (sent < 0) {
        g_logger.error("[IPC Client] Write failed: " + std::string(strerror(errno)));
        disconnect();  // Disconnect on error
        return false;
    }

    if (static_cast<size_t>(sent) != full_message.size()) {
        g_logger.error("[IPC Client] Partial write: sent " + std::to_string(sent) +
                      " of " + std::to_string(full_message.size()) + " bytes");
        disconnect();
        return false;
    }

    // Read response
    std::string response = readResponse();
    if (response.rfind("ACK", 0) == 0) {
        g_logger.debug("[IPC Client] Message acknowledged: " + message);
        return true;
    } else if (response.rfind("ERROR", 0) == 0) {
        g_logger.warning("[IPC Client] Message rejected: " + response);
        return false;
    } else {
        g_logger.error("[IPC Client] Invalid response: " + response);
        disconnect();
        return false;
    }
}

std::string LightningIPCClient::readResponse() {
    if (!isConnected()) {
        return "ERROR Not connected";
    }

    // Read response (newline-terminated)
    char buffer[1024];
    std::string response;

    while (true) {
        ssize_t n = read(m_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            if (n < 0) {
                g_logger.error("[IPC Client] Read failed: " + std::string(strerror(errno)));
            } else {
                g_logger.warning("[IPC Client] Connection closed by peer");
            }
            disconnect();
            return "ERROR Connection closed";
        }

        buffer[n] = '\0';
        response += buffer;

        // Check for newline
        size_t pos = response.find('\n');
        if (pos != std::string::npos) {
            // Return response without newline
            return response.substr(0, pos);
        }

        // Prevent infinite buffering
        if (response.size() > 4096) {
            g_logger.error("[IPC Client] Response too large");
            disconnect();
            return "ERROR Response too large";
        }
    }
}

} // namespace ipc
} // namespace dinero

#endif // !_WIN32
