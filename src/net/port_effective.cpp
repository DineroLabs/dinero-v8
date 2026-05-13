#include "net/port_effective.h"
#include "compat/net_compat.h"

namespace dinero {
namespace net {

int effective_bound_port(int fd) {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    if (getsockname(fd, (sockaddr*)&sa, &len) != 0) {
        return -1;
    }
    return ntohs(sa.sin_port);
}

} // namespace net
} // namespace dinero
