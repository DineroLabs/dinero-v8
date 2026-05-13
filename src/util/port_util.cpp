#include "util/port_util.h"
#include "compat/net_compat.h"
#include <cstring>

namespace PortUtil {

bool isPortFree(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    
    int result = bind(sock, (sockaddr*)&addr, sizeof(addr));
    COMPAT_CLOSE_SOCKET(sock);
    
    return result == 0;
}

uint16_t findFreePort(uint16_t suggested) {
    // If suggested port is provided and free, use it
    if (suggested > 0 && isPortFree(suggested)) {
        return suggested;
    }
    
    // Otherwise, ask OS for a free ephemeral port
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Let OS choose
    
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        COMPAT_CLOSE_SOCKET(sock);
        return 0;
    }
    
    socklen_t len = sizeof(addr);
    if (getsockname(sock, (sockaddr*)&addr, &len) != 0) {
        COMPAT_CLOSE_SOCKET(sock);
        return 0;
    }
    
    uint16_t port = ntohs(addr.sin_port);
    COMPAT_CLOSE_SOCKET(sock);
    
    return port;
}

} // namespace PortUtil
