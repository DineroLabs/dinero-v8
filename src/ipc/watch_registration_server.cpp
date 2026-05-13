// ═══════════════════════════════════════════════════════════════════════════
// Watch Registration Server Implementation (Phase 9.3: Bidirectional Oracle Communication)
// ═══════════════════════════════════════════════════════════════════════════

#include "ipc/watch_registration_server.h"
#include "ipc/oracles/transaction_oracle_client.h"

#ifndef _WIN32  // Unix domain socket IPC — not available on Windows

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace dinero {
namespace ipc {

WatchRegistrationServer::WatchRegistrationServer(
    const std::string& socket_path,
    std::shared_ptr<TransactionOracleClient> tx_oracle
)
    : m_socket_path(socket_path)
    , m_tx_oracle(tx_oracle)
    , m_server_fd(-1)
    , m_running(false)
{
}

WatchRegistrationServer::~WatchRegistrationServer() {
    stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// Lifecycle Management
// ═══════════════════════════════════════════════════════════════════════════

bool WatchRegistrationServer::start() {
    if (m_running.load()) {
        return true;  // Already running
    }

    // Create Unix domain socket
    m_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        std::cerr << "[WatchRegistrationServer] Failed to create socket" << std::endl;
        return false;
    }

    // Remove existing socket file if it exists
    unlink(m_socket_path.c_str());

    // Bind to socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[WatchRegistrationServer] Failed to bind socket: " << m_socket_path << std::endl;
        close(m_server_fd);
        m_server_fd = -1;
        return false;
    }

    // Listen for connections
    if (listen(m_server_fd, 5) < 0) {
        std::cerr << "[WatchRegistrationServer] Failed to listen on socket" << std::endl;
        close(m_server_fd);
        m_server_fd = -1;
        unlink(m_socket_path.c_str());
        return false;
    }

    // Start server thread
    m_running.store(true);
    m_thread = std::make_unique<std::thread>(&WatchRegistrationServer::serverLoop, this);

    std::cout << "[WatchRegistrationServer] Started on " << m_socket_path << std::endl;
    return true;
}

void WatchRegistrationServer::stop() {
    if (!m_running.load()) {
        return;  // Already stopped
    }

    // Signal server to stop
    m_running.store(false);

    // Close server socket (will unblock accept)
    if (m_server_fd >= 0) {
        close(m_server_fd);
        m_server_fd = -1;
    }

    // Wait for thread to exit
    if (m_thread && m_thread->joinable()) {
        m_thread->join();
    }

    // Remove socket file
    unlink(m_socket_path.c_str());

    std::cout << "[WatchRegistrationServer] Stopped" << std::endl;
}

WatchRegistrationServer::Stats WatchRegistrationServer::getStats() const {
    Stats stats;
    stats.connections_accepted = m_connections_accepted.load();
    stats.watch_commands_received = m_watch_commands.load();
    stats.unwatch_commands_received = m_unwatch_commands.load();
    stats.clear_commands_received = m_clear_commands.load();
    stats.errors = m_errors.load();
    return stats;
}

// ═══════════════════════════════════════════════════════════════════════════
// Server Implementation
// ═══════════════════════════════════════════════════════════════════════════

void WatchRegistrationServer::serverLoop() {
    while (m_running.load()) {
        // Accept client connection (blocking)
        int client_fd = accept(m_server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (m_running.load()) {
                std::cerr << "[WatchRegistrationServer] Accept failed" << std::endl;
                m_errors.fetch_add(1);
            }
            break;  // Server likely stopped
        }

        m_connections_accepted.fetch_add(1);

        // Handle client (synchronous for simplicity)
        handleClient(client_fd);

        // Close client connection
        close(client_fd);
    }
}

void WatchRegistrationServer::handleClient(int client_fd) {
    // Read command from client
    std::string command;
    if (!readLine(client_fd, command)) {
        writeLine(client_fd, "ERROR Failed to read command");
        m_errors.fetch_add(1);
        return;
    }

    // Process command
    std::string response = processCommand(command);

    // Send response
    if (!writeLine(client_fd, response)) {
        m_errors.fetch_add(1);
    }
}

std::string WatchRegistrationServer::processCommand(const std::string& message) {
    // WATCH_TX txid=<hex>
    if (message.rfind("WATCH_TX ", 0) == 0) {
        std::string txid;
        if (!parseWatchTx(message, txid)) {
            m_errors.fetch_add(1);
            return "ERROR Invalid WATCH_TX format";
        }

        // Forward to transaction oracle
        if (m_tx_oracle) {
            m_tx_oracle->addWatch(txid);
            m_watch_commands.fetch_add(1);
            return "ACK";
        } else {
            m_errors.fetch_add(1);
            return "ERROR Transaction oracle not configured";
        }
    }
    // UNWATCH_TX txid=<hex>
    else if (message.rfind("UNWATCH_TX ", 0) == 0) {
        std::string txid;
        if (!parseUnwatchTx(message, txid)) {
            m_errors.fetch_add(1);
            return "ERROR Invalid UNWATCH_TX format";
        }

        // Forward to transaction oracle
        if (m_tx_oracle) {
            m_tx_oracle->removeWatch(txid);
            m_unwatch_commands.fetch_add(1);
            return "ACK";
        } else {
            m_errors.fetch_add(1);
            return "ERROR Transaction oracle not configured";
        }
    }
    // CLEAR_WATCHES
    else if (message == "CLEAR_WATCHES") {
        // Forward to transaction oracle
        if (m_tx_oracle) {
            m_tx_oracle->clearWatches();
            m_clear_commands.fetch_add(1);
            return "ACK";
        } else {
            m_errors.fetch_add(1);
            return "ERROR Transaction oracle not configured";
        }
    }
    else {
        m_errors.fetch_add(1);
        return "ERROR Unknown command";
    }
}

bool WatchRegistrationServer::parseWatchTx(const std::string& message, std::string& txid) {
    // Format: WATCH_TX txid=<hex>
    std::istringstream iss(message);
    std::string cmd, txid_str;

    if (!(iss >> cmd >> txid_str)) {
        return false;
    }

    // Extract txid=<value>
    size_t eq_pos = txid_str.find('=');
    if (eq_pos == std::string::npos) {
        return false;
    }

    txid = txid_str.substr(eq_pos + 1);

    // Basic validation: txid should be 64 hex chars
    if (txid.length() != 64) {
        return false;
    }

    return true;
}

bool WatchRegistrationServer::parseUnwatchTx(const std::string& message, std::string& txid) {
    // Format: UNWATCH_TX txid=<hex>
    std::istringstream iss(message);
    std::string cmd, txid_str;

    if (!(iss >> cmd >> txid_str)) {
        return false;
    }

    // Extract txid=<value>
    size_t eq_pos = txid_str.find('=');
    if (eq_pos == std::string::npos) {
        return false;
    }

    txid = txid_str.substr(eq_pos + 1);

    // Basic validation: txid should be 64 hex chars
    if (txid.length() != 64) {
        return false;
    }

    return true;
}

bool WatchRegistrationServer::readLine(int fd, std::string& line) {
    line.clear();

    // Read until newline or timeout (5 seconds)
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    char buffer[4096];
    while (true) {
        // Wait for data (500ms chunks, up to 5 seconds total)
        int poll_result = poll(&pfd, 1, 500);
        if (poll_result < 0) {
            return false;  // Poll error
        }
        if (poll_result == 0) {
            continue;  // Timeout - keep waiting
        }

        // Data available - read
        ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            return false;  // Read error or EOF
        }

        buffer[n] = '\0';
        line += buffer;

        // Check for newline
        size_t newline_pos = line.find('\n');
        if (newline_pos != std::string::npos) {
            // Found complete line - trim newline and return
            line = line.substr(0, newline_pos);
            return true;
        }

        // No newline yet - continue reading (up to 10 iterations = 5 seconds)
        static int iterations = 0;
        if (++iterations > 10) {
            iterations = 0;
            return false;  // Timeout
        }
    }
}

bool WatchRegistrationServer::writeLine(int fd, const std::string& line) {
    std::string wire_line = line + "\n";
    ssize_t sent = write(fd, wire_line.c_str(), wire_line.size());
    return sent >= 0 && static_cast<size_t>(sent) == wire_line.size();
}

} // namespace ipc
} // namespace dinero

#else // _WIN32 — stub implementations (no Unix domain sockets on Windows)

namespace dinero {
namespace ipc {
WatchRegistrationServer::WatchRegistrationServer(const std::string&, std::shared_ptr<TransactionOracleClient>) : m_server_fd(-1), m_running(false) {}
WatchRegistrationServer::~WatchRegistrationServer() {}
bool WatchRegistrationServer::start() { return false; }
void WatchRegistrationServer::stop() {}
WatchRegistrationServer::Stats WatchRegistrationServer::getStats() const { return {}; }
void WatchRegistrationServer::serverLoop() {}
void WatchRegistrationServer::handleClient(int) {}
std::string WatchRegistrationServer::processCommand(const std::string&) { return "ERROR Not supported on Windows"; }
bool WatchRegistrationServer::parseWatchTx(const std::string&, std::string&) { return false; }
bool WatchRegistrationServer::parseUnwatchTx(const std::string&, std::string&) { return false; }
bool WatchRegistrationServer::readLine(int, std::string&) { return false; }
bool WatchRegistrationServer::writeLine(int, const std::string&) { return false; }
} // namespace ipc
} // namespace dinero

#endif // !_WIN32
