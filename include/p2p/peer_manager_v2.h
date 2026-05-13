#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include "peer_v2.h"

namespace din::p2p {

struct Seed { std::string host; uint16_t port; };

class PeerManager {
public:
  explicit PeerManager(boost::asio::io_context& io);
  void add_seed(std::string host, uint16_t port);
  void start(unsigned max_outbound);
  size_t connected() const { return peers_.size(); }
  
  // Week 7: Get peer information for RPC
  std::vector<std::pair<std::string, uint16_t>> getPeerAddresses() const;

private:
  void dial_next();

  boost::asio::io_context& io_;
  std::vector<Seed> seeds_;
  std::vector<std::shared_ptr<Peer>> peers_;
  unsigned max_outbound_{8};
  size_t next_seed_{0};
};

} // namespace din::p2p
