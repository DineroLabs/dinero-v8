#include "p2p/peer_v2.h"
#include "consensus/chainparams.h"  // dinero::Params().magic — canonical
#include <chrono>
#include <random>
#include <openssl/sha.h> // for checksum; or your own sha256
#include <cstring>
using namespace din::p2p;
using namespace std::chrono;

namespace {
uint32_t checksum4(const std::vector<uint8_t>& v){
  uint8_t h1[SHA256_DIGEST_LENGTH], h2[SHA256_DIGEST_LENGTH];
  SHA256(v.data(), v.size(), h1);
  SHA256(h1, sizeof(h1), h2);
  uint32_t c; std::memcpy(&c, h2, 4); return c;
}
}

Peer::Peer(boost::asio::io_context& io, std::string host, uint16_t port)
: io_(io), socket_(io), host_(std::move(host)), port_(port) {}

void Peer::start(){ do_connect(); }

void Peer::do_connect(){
  auto self = shared_from_this();
  tcp::resolver res(io_);
  res.async_resolve(host_, std::to_string(port_),
    [this,self](auto ec, auto results){
      if(ec){ if(on_log) on_log(self, "resolve failed"); return; }
      boost::asio::async_connect(socket_, results,
        [this,self](auto ec2, auto){
          if(ec2){ if(on_log) on_log(self, "connect failed"); return; }
          if(on_connected) on_connected(self);
          send_version();
          read_header();
        });
    });
}

void Peer::send_version(){
  Version v;
  v.timestamp = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  v.nonce = ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}();
  // TODO: fill start_height from header index
  auto payload = serialize_version(v);
  write_message("version", payload);
}

void Peer::send_verack(){
  std::vector<uint8_t> none;
  write_message("verack", none);
  sent_verack_ = true;
}

void Peer::write_message(const char* command, const std::vector<uint8_t>& payload){
  MsgHeader h;
  h.magic = dinero::Params().magic;
  h.command = cmd(command);
  h.length = (uint32_t)payload.size();
  h.checksum = checksum4(payload);

  std::vector<uint8_t> out; out.reserve(24 + payload.size());
  uint32_t magic_le = boost::endian::native_to_little(h.magic);
  uint32_t len_le   = boost::endian::native_to_little(h.length);
  uint32_t cks_le   = boost::endian::native_to_little(h.checksum);
  out.insert(out.end(), (uint8_t*)&magic_le, (uint8_t*)&magic_le + 4);
  out.insert(out.end(), (uint8_t*)h.command.data(), (uint8_t*)h.command.data() + 12);
  out.insert(out.end(), (uint8_t*)&len_le, (uint8_t*)&len_le + 4);
  out.insert(out.end(), (uint8_t*)&cks_le, (uint8_t*)&cks_le + 4);
  out.insert(out.end(), payload.begin(), payload.end());

  auto self = shared_from_this();
  boost::asio::async_write(socket_, boost::asio::buffer(out),
    [this,self](auto ec, std::size_t){ if(ec){ close(); } });
}

void Peer::read_header(){
  auto self = shared_from_this();
  boost::asio::async_read(socket_, boost::asio::buffer(header_buf_),
    [this,self](auto ec, std::size_t n){
      if(ec || n!=24){ close(); return; }
      // parse
      uint32_t magic = *(uint32_t*)&header_buf_[0];
      uint32_t len   = *(uint32_t*)&header_buf_[16];
      boost::endian::little_to_native_inplace(magic);
      boost::endian::little_to_native_inplace(len);
      if(magic != dinero::Params().magic || len > (8u<<20)) { close(); return; } // 8MB cap
      read_payload(len);
    });
}

void Peer::read_payload(uint32_t len){
  payload_buf_.assign(len, 0);
  auto self = shared_from_this();
  boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_),
    [this,self,len](auto ec, std::size_t n){
      if(ec || n!=len){ close(); return; }
      // command
      char cmdstr[13]; std::memcpy(cmdstr, header_buf_.data()+4, 12); cmdstr[12]=0;
      MsgHeader h{};
      std::memcpy(h.command.data(), cmdstr, 12);
      handle_message(h, payload_buf_);
      read_header(); // loop
    });
}

void Peer::handle_message(const MsgHeader& h, const std::vector<uint8_t>& payload){
  const std::string c(h.command.data(), strnlen(h.command.data(), 12));
  if(c == "version"){
    got_version_=true; if(!sent_verack_) send_verack();
    // Optionally parse and store peer's params; skip in 3A
  } else if(c == "verack"){
    got_verack_=true;
    // Ready for PR-3B: send getheaders next
  } else if(c == "ping"){
    write_message("pong", payload);
  } else {
    // ignore unknowns for 3A
  }
}

void Peer::close(){
  boost::system::error_code ec; socket_.close(ec);
}
