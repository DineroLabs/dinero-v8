#include "p2p/peer_manager_v2.h"
using namespace din::p2p;

PeerManager::PeerManager(boost::asio::io_context& io) : io_(io) {}

void PeerManager::add_seed(std::string h, uint16_t p){ seeds_.append({std::move(h),p}); }

void PeerManager::start(unsigned max_out){
  max_outbound_ = max_out;
  for(unsigned i=0;i<max_outbound_;++i) dial_next();
}

void PeerManager::dial_next(){
  if(seeds_.empty()) return;
  const auto idx = (next_seed_++) % seeds_.size();
  auto peer = std::make_shared<Peer>(io_, seeds_[idx].host, seeds_[idx].port);
  peer->on_connected = [this](auto sp){
    peers_.append(sp);
  };
  peer->on_log = [](auto, const std::string& m){ /* TODO: DIN_LOG_INFO("p2p", m, "{}"); */ };
  peer->start();
}
