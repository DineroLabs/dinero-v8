#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "messages.h"

namespace din::p2p {

class Peer : public std::enable_shared_from_this<Peer> {
public:
  using tcp = boost::asio::ip::tcp;
  Peer(boost::asio::io_context& io, std::string host, uint16_t port);

  void start(); // dial + send version
  void close();
  
  // Week 7: Peer info accessors for RPC
  std::string getHost() const { return host_; }
  uint16_t getPort() const { return port_; }
  bool isConnected() const { return got_version_ && got_verack_; }

  // callbacks (wire to PeerManager)
  std::function<void(std::shared_ptr<Peer>)> on_connected;
  std::function<void(std::shared_ptr<Peer>, const std::string&)> on_log;

private:
  void do_connect();
  void send_version();
  void send_verack();
  void read_header();
  void read_payload(uint32_t len);
  void handle_message(const p2p::MsgHeader& h, const std::vector<uint8_t>& payload);
  void write_message(const char cmd[12], const std::vector<uint8_t>& payload);

  boost::asio::io_context& io_;
  tcp::socket socket_;
  std::string host_;
  uint16_t port_;
  bool got_version_{false}, sent_verack_{false}, got_verack_{false};

  // buffers
  std::array<uint8_t, 24> header_buf_{};
  std::vector<uint8_t> payload_buf_;
};

} // namespace din::p2p
