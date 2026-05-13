// ═══════════════════════════════════════════════════════════════════════════
// Watch Registration Client Implementation (Phase 9.3: Bidirectional Oracle Communication)
// ═══════════════════════════════════════════════════════════════════════════

#include "ipc/watch_registration_client.h"

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

WatchRegistrationClient::WatchRegistrationClient(const std::string& socket_path)
    : m_socket_path(socket_path)
{
}

WatchRegistrationClient::~WatchRegistrationClient() {
    // No persistent connection - nothing to clean up
}

// ═══════════════════════════════════════════════════════════════════════════
// Watch Registration Operations
// ═══════════════════════════════════════════════════════════════════════════

bool WatchRegistrationClient::watchTx(const std::string& txid) {
    // Serialize: WATCH_TX txid=<txid>
    std::ostringstream oss;
    oss << "WATCH_TX txid=" << txid;

    return sendCommand(oss.str());
}

bool WatchRegistrationClient::unwatchTx(const std::string& txid) {
    // Serialize: UNWATCH_TX txid=<txid>
    std::ostringstream oss;
    oss << "UNWATCH_TX txid=" << txid;

    return sendCommand(oss.str());
}

bool WatchRegistrationClient::clearWatches() {
    return sendCommand("CLEAR_WATCHES");
}

// ═══════════════════════════════════════════════════════════════════════════
// Helper Methods
// ═══════════════════════════════════════════════════════════════════════════

bool WatchRegistrationClient::sendCommand(const std::string& command) {
    // Connect to dinerod
    int fd = connect();
    if (fd < 0) {
        return false;
    }

    // Send command
    if (!writeLine(fd, command)) {
        close(fd);
        return false;
    }

    // Read response
    std::string response;
    if (!readLine(fd, response)) {
        close(fd);
        return false;
    }

    // Close connection
    close(fd);

    // Check for ACK
    return response == "ACK";
}

int WatchRegistrationClient::connect() {
    // Create Unix domain socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    // Set non-blocking for connect timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Connect to dinerod socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    // Wait for connection (timeout 1 second)
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int poll_result = poll(&pfd, 1, 1000);  // 1 second timeout
    if (poll_result <= 0) {
        close(fd);
        return -1;
    }

    // Check if connected
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        close(fd);
        return -1;
    }

    // Restore blocking mode for send/recv
    fcntl(fd, F_SETFL, flags);

    return fd;
}

bool WatchRegistrationClient::readLine(int fd, std::string& line) {
    line.clear();

    // Read until newline or timeout (5 seconds)
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    char buffer[4096];
    int iterations = 0;
    while (true) {
        // Wait for data (500ms chunks, up to 5 seconds total)
        int poll_result = poll(&pfd, 1, 500);
        if (poll_result < 0) {
            return false;  // Poll error
        }
        if (poll_result == 0) {
            // Timeout - keep waiting (up to 10 iterations = 5 seconds)
            if (++iterations > 10) {
                return false;  // Timeout
            }
            continue;
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

        // No newline yet - continue reading
    }
}

bool WatchRegistrationClient::writeLine(int fd, const std::string& line) {
    std::string wire_line = line + "\n";
    ssize_t sent = write(fd, wire_line.c_str(), wire_line.size());
    return sent >= 0 && static_cast<size_t>(sent) == wire_line.size();
}

} // namespace ipc
} // namespace dinero

#endif // !_WIN32
