#pragma once
#include <boost/asio.hpp>
#include <string>
#include <vector>

namespace din::net {
  inline void socks5_connect(boost::asio::ip::tcp::socket& s,
                             const std::string& proxy_host, const std::string& proxy_port,
                             const std::string& dest_host,  uint16_t dest_port) {
    using boost::asio::ip::tcp;
    boost::asio::io_context& ioc = static_cast<boost::asio::io_context&>(s.get_executor().context());

    // 1) TCP connect to proxy
    tcp::resolver r(ioc);
    auto ep = r.resolve(proxy_host, proxy_port);
    boost::asio::connect(s, ep);

    // 2) greeting: no auth
    std::vector<uint8_t> req{0x05, 0x01, 0x00};
    boost::asio::write(s, boost::asio::buffer(req));
    uint8_t rep[2]; boost::asio::read(s, boost::asio::buffer(rep,2));
    if (rep[0]!=0x05 || rep[1]!=0x00) throw std::runtime_error("SOCKS5 no-auth not accepted");

    // 3) CONNECT dest_host:dest_port (domain form)
    std::vector<uint8_t> c{0x05,0x01,0x00,0x03,(uint8_t)dest_host.size()};
    c.insert(c.end(), dest_host.begin(), dest_host.end());
    c.push_back(uint8_t(dest_port>>8)); c.push_back(uint8_t(dest_port&0xFF));
    boost::asio::write(s, boost::asio::buffer(c));

    // 4) reply
    uint8_t hdr[4]; boost::asio::read(s, boost::asio::buffer(hdr,4));
    if (hdr[1]!=0x00) throw std::runtime_error("SOCKS5 connect failed");
    // skip bind addr
    size_t skip = 0;
    if (hdr[3]==0x01) skip=4; else if (hdr[3]==0x03){ uint8_t l; boost::asio::read(s,boost::asio::buffer(&l,1)); skip=l; } else if (hdr[3]==0x04) skip=16;
    std::vector<uint8_t> sink(skip+2); boost::asio::read(s, boost::asio::buffer(sink));
  }
}
