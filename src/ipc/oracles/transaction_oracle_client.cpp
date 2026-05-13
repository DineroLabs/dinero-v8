// ═══════════════════════════════════════════════════════════════════════════
// Transaction Oracle Client Implementation (Phase 9.2: Production Oracles)
// ═══════════════════════════════════════════════════════════════════════════

#include "ipc/oracles/transaction_oracle_client.h"

#ifndef _WIN32  // Unix domain socket IPC — not available on Windows

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>

namespace dinero {
namespace ipc {

TransactionOracleClient::TransactionOracleClient(const std::string& socket_path)
    : m_socket_path(socket_path)
    , m_socket_fd(-1)
{
}

TransactionOracleClient::~TransactionOracleClient() {
    disconnect();
}

// ═══════════════════════════════════════════════════════════════════════════
// Connection Management
// ═══════════════════════════════════════════════════════════════════════════

bool TransactionOracleClient::connect() {
    // Idempotent: If already connected, return success
    if (m_socket_fd >= 0) {
        return true;
    }

    // Create Unix domain socket
    m_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_socket_fd < 0) {
        return false;
    }

    // Set non-blocking for connect timeout
    int flags = fcntl(m_socket_fd, F_GETFL, 0);
    fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK);

    // Connect to lightningd socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(m_socket_fd, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(m_socket_fd);
        m_socket_fd = -1;
        return false;
    }

    // Wait for connection (timeout 1 second)
    struct pollfd pfd;
    pfd.fd = m_socket_fd;
    pfd.events = POLLOUT;

    int poll_result = poll(&pfd, 1, 1000);  // 1 second timeout
    if (poll_result <= 0) {
        close(m_socket_fd);
        m_socket_fd = -1;
        return false;
    }

    // Check if connected
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(m_socket_fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        close(m_socket_fd);
        m_socket_fd = -1;
        return false;
    }

    // Restore blocking mode for send/recv
    fcntl(m_socket_fd, F_SETFL, flags);

    return true;
}

void TransactionOracleClient::disconnect() {
    if (m_socket_fd >= 0) {
        close(m_socket_fd);
        m_socket_fd = -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Watch List Management (Minimal State)
// ═══════════════════════════════════════════════════════════════════════════

void TransactionOracleClient::addWatch(const std::string& txid) {
    std::lock_guard<std::mutex> lock(m_watch_mutex);
    m_watched_txids.insert(txid);
}

void TransactionOracleClient::removeWatch(const std::string& txid) {
    std::lock_guard<std::mutex> lock(m_watch_mutex);
    m_watched_txids.erase(txid);
}

void TransactionOracleClient::clearWatches() {
    std::lock_guard<std::mutex> lock(m_watch_mutex);
    m_watched_txids.clear();
}

size_t TransactionOracleClient::getWatchCount() const {
    std::lock_guard<std::mutex> lock(m_watch_mutex);
    return m_watched_txids.size();
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Confirmation Forwarding (Facts Only)
// ═══════════════════════════════════════════════════════════════════════════

bool TransactionOracleClient::checkAndNotify(const std::string& txid, uint64_t height) {
    // Check if txid is in watch list
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        if (m_watched_txids.find(txid) == m_watched_txids.end()) {
            return false;  // Not watched
        }
    }

    // Txid is watched - send TX_CONFIRMED notification
    return sendTxConfirmed(txid, height);
}

// ═══════════════════════════════════════════════════════════════════════════
// Helper Methods (Pure Transport)
// ═══════════════════════════════════════════════════════════════════════════

bool TransactionOracleClient::sendTxConfirmed(const std::string& txid, uint64_t height) {
    // Serialize: TX_CONFIRMED txid=<txid> height=<height>
    std::ostringstream oss;
    oss << "TX_CONFIRMED txid=" << txid << " height=" << height;

    return sendMessage(oss.str());
}

bool TransactionOracleClient::sendMessage(const std::string& message) {
    // Not connected - fail immediately
    if (m_socket_fd < 0) {
        return false;
    }

    // Send message with newline
    std::string wire_message = message + "\n";
    ssize_t sent = write(m_socket_fd, wire_message.c_str(), wire_message.size());
    if (sent < 0 || static_cast<size_t>(sent) != wire_message.size()) {
        // Write failed - disconnect
        disconnect();
        return false;
    }

    // Read response
    std::string response;
    if (!readResponse(response)) {
        // Read failed - disconnect
        disconnect();
        return false;
    }

    // Check for ACK
    if (response == "ACK") {
        return true;
    }

    // Error response or unexpected response - fail but don't disconnect
    // (Lightning may reject event but still be alive)
    return false;
}

bool TransactionOracleClient::readResponse(std::string& response) {
    response.clear();

    // Read until newline or timeout (1 second)
    struct pollfd pfd;
    pfd.fd = m_socket_fd;
    pfd.events = POLLIN;

    char buffer[4096];
    int iterations = 0;
    while (true) {
        // Wait for data (100ms chunks, up to 1 second total)
        int poll_result = poll(&pfd, 1, 100);
        if (poll_result < 0) {
            return false;  // Poll error
        }
        if (poll_result == 0) {
            // Timeout - keep waiting (up to 10 iterations = 1 second)
            if (++iterations > 10) {
                return false;  // Timeout
            }
            continue;
        }

        // Data available - read
        ssize_t n = read(m_socket_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            return false;  // Read error or EOF
        }

        buffer[n] = '\0';
        response += buffer;

        // Check for newline
        size_t newline_pos = response.find('\n');
        if (newline_pos != std::string::npos) {
            // Found complete line - trim newline and return
            response = response.substr(0, newline_pos);
            return true;
        }

        // No newline yet - continue reading
    }
}

} // namespace ipc
} // namespace dinero

#else // _WIN32 — stub implementations (no Unix domain sockets on Windows)

namespace dinero {
namespace ipc {
TransactionOracleClient::TransactionOracleClient(const std::string&) : m_socket_fd(-1) {}
TransactionOracleClient::~TransactionOracleClient() {}
bool TransactionOracleClient::connect() { return false; }
void TransactionOracleClient::disconnect() {}
void TransactionOracleClient::addWatch(const std::string&) {}
void TransactionOracleClient::removeWatch(const std::string&) {}
void TransactionOracleClient::clearWatches() {}
size_t TransactionOracleClient::getWatchCount() const { return 0; }
bool TransactionOracleClient::checkAndNotify(const std::string&, uint64_t) { return false; }
bool TransactionOracleClient::sendTxConfirmed(const std::string&, uint64_t) { return false; }
bool TransactionOracleClient::sendMessage(const std::string&) { return false; }
bool TransactionOracleClient::readResponse(std::string&) { return false; }
} // namespace ipc
} // namespace dinero

#endif // !_WIN32
