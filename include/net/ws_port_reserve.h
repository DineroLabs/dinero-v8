#pragma once
#include <cstdint>
#include <system_error>

#include <asio.hpp>

namespace dinero {
namespace net {

/**
 * Reserve a free ephemeral port on localhost.
 * This is useful for WebSocket servers that need to know the actual port
 * before binding, especially when using port 0 for auto-selection.
 * 
 * @param io ASIO io_context
 * @param ec Error code output
 * @return The reserved port number, or 0 on error
 */
inline uint16_t reserve_loopback_ephemeral_port(asio::io_context& io, std::error_code& ec) {
    using tcp = asio::ip::tcp;
    ec = {};
    tcp::acceptor acc(io);
    tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    acc.open(ep.protocol(), ec);          
    if (ec) return 0;
    
    acc.set_option(tcp::acceptor::reuse_address(true), ec); // avoid TIME_WAIT collisions
    if (ec) return 0;
    
    acc.bind(ep, ec);                      
    if (ec) return 0;
    
    auto assigned = acc.local_endpoint(ec); 
    if (ec) return 0;
    
    acc.close(ec); // free the reservation; websocketpp will bind next
    return assigned.port();
}

} // namespace net
} // namespace dinero
