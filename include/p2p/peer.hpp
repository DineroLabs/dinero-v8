#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <ctime>
#include <cstring>
#include "p2p/p2p_wire_protocol.h"
#include "p2p/sha256d.h"

namespace din::p2p {

class Peer : public std::enable_shared_from_this<Peer> {
public:
    using tcp = boost::asio::ip::tcp;
    
    explicit Peer(boost::asio::io_context& io, std::string host, uint16_t port)
        : io_(io), socket_(io), timer_(io), host_(std::move(host)), port_(port) {}

    void start() { resolve_and_connect(); }
    void close() { 
        boost::system::error_code ec; 
        socket_.close(ec); 
        timer_.cancel(); 
    }

private:
    void resolve_and_connect() {
        auto self = shared_from_this();
        tcp::resolver resolver(io_);
        resolver.async_resolve(host_, std::to_string(port_),
            [this, self](auto ec, auto results) {
                if (ec) return;
                boost::asio::async_connect(socket_, results,
                    [this, self](auto ec2, auto) {
                        if (ec2) return;
                        send_version();
                        read_header();
                    });
            });
    }

    void send_version() {
        Version v;
        v.timestamp = std::time(nullptr);
        v.nonce = (uint64_t(std::random_device{}()) << 32) ^ std::random_device{}();
        v.user_agent = DineroUserAgent();
        v.start_height = 0; // TODO: get from blockchain
        v.relay = true;
        
        auto payload = serialize_version(v);
        auto msg = build_message("version", payload, sha256d);
        
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(msg),
            [this, self, buf = std::move(msg)](auto ec, std::size_t) { 
                if (ec) close(); 
            });
    }

    void read_header() {
        auto self = shared_from_this();
        
        // 10s header timeout
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([this, self](auto ec) { 
            if (!ec) close(); 
        });

        boost::asio::async_read(socket_, boost::asio::buffer(hdr_),
            [this, self](auto ec, std::size_t n) {
                timer_.cancel();
                if (ec || n != hdr_.size()) { 
                    close(); 
                    return; 
                }
                
                if (!parse_header(hdr_.data(), header_)) { 
                    close(); 
                    return; 
                }
                
                if (header_.length > (8u << 20)) { // 8 MiB cap
                    close(); 
                    return; 
                }
                
                payload_.assign(header_.length, 0);
                read_payload();
            });
    }

    void read_payload() {
        auto self = shared_from_this();
        
        // 30s payload timeout
        timer_.expires_after(std::chrono::seconds(30));
        timer_.async_wait([this, self](auto ec) { 
            if (!ec) close(); 
        });

        boost::asio::async_read(socket_, boost::asio::buffer(payload_),
            [this, self](auto ec, std::size_t n) {
                timer_.cancel();
                if (ec || n != payload_.size()) { 
                    close(); 
                    return; 
                }
                
                // Verify checksum
                auto h = sha256d(payload_.data(), payload_.size());
                uint32_t ck = 0; 
                std::memcpy(&ck, h.data(), 4);
                if (ck != header_.checksum) { 
                    close(); 
                    return; 
                }
                
                handle_message();
                read_header(); // Continue reading next message
            });
    }

    void handle_message() {
        const std::string cmd = extract_command(header_.command);
        
        if (cmd == "version") {
            // Parse peer's version if needed:
            // auto v = parse_version(payload_);
            
            // Respond with verack
            std::vector<uint8_t> empty_payload;
            auto msg = build_message("verack", empty_payload, sha256d);
            
            auto self = shared_from_this();
            boost::asio::async_write(socket_, boost::asio::buffer(msg),
                [this, self, buf = std::move(msg)](auto ec, std::size_t) { 
                    if (ec) close(); 
                });
                
        } else if (cmd == "verack") {
            // Handshake complete - ready for getheaders, etc.
            
        } else if (cmd == "ping") {
            // Echo ping as pong
            auto msg = build_message("pong", payload_, sha256d);
            
            auto self = shared_from_this();
            boost::asio::async_write(socket_, boost::asio::buffer(msg),
                [this, self, buf = std::move(msg)](auto ec, std::size_t) { 
                    if (ec) close(); 
                });
                
        } else if (cmd == "pong") {
            // Handle pong response
            
        } else {
            // Unknown message - ignore for now
        }
    }

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer timer_;
    std::string host_;
    uint16_t port_;

    std::array<uint8_t, 24> hdr_{};
    MsgHeader header_{};
    std::vector<uint8_t> payload_;
};

} // namespace din::p2p
