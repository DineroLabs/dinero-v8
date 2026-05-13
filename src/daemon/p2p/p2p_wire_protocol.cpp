#include "p2p/p2p_wire_protocol.h"
#include "p2p_message.h"
#include "common/sha256d.h"
#include "version.h"
#include <array>

namespace din::p2p {

// Global network configuration instance
NetworkConfig g_network_config;

// SHA256d wrapper for P2P wire protocol checksum calculation
std::array<uint8_t,32> sha256d_checksum(const uint8_t* data, size_t len) {
    Dinero::Common::sha256 hasher;
    
    // First SHA256
    hasher.update(data, len);
    std::vector<uint8_t> hash1 = hasher.finalize();
    
    // Second SHA256 (double hash)
    hasher.reset();
    hasher.update(hash1.data(), hash1.size());
    std::vector<uint8_t> hash2 = hasher.finalize();
    
    // Convert to fixed-size array
    std::array<uint8_t,32> result;
    std::copy(hash2.begin(), hash2.end(), result.begin());
    return result;
}

// Convenience function for building messages with integrated SHA256d
std::vector<uint8_t> build_message_with_checksum(const char* command, const std::vector<uint8_t>& payload) {
    return build_message(command, payload, sha256d_checksum);
}

// Network configuration initialization for different networks
void init_network_config(const std::string& network) {
    if (network == "mainnet") {
        g_network_config.magic = 0xD1A0C0DE;  // Dinero mainnet magic
        g_network_config.user_agent_prefix = DineroUserAgent();
    } else if (network == "testnet") {
        g_network_config.magic = 0xDAB5BFFA;  // Dinero testnet magic
        g_network_config.user_agent_prefix = DineroUserAgent("-testnet");
    } else if (network == "regtest") {
        g_network_config.magic = 0xFABFB5DA;  // Dinero regtest magic
        g_network_config.user_agent_prefix = DineroUserAgent("-regtest");
    } else {
        throw std::runtime_error("Unknown network: " + network);
    }

    // Also initialize p2p_message.h's magic for Qt-based peer connections
    p2p::init_p2p_network(network);
}

// Validate message integrity (header + checksum verification)
bool validate_message(const uint8_t* header_buf, const std::vector<uint8_t>& payload) {
    MsgHeader header;
    if (!parse_header(header_buf, header)) {
        return false;
    }
    
    // Verify payload length matches header
    if (payload.size() != header.length) {
        return false;
    }
    
    // Verify checksum
    uint32_t calculated_checksum = checksum4(payload, sha256d_checksum);
    return calculated_checksum == header.checksum;
}

// Create a complete version message for handshake
std::vector<uint8_t> create_version_message(int64_t timestamp, uint64_t nonce, 
                                           const std::string& user_agent, int32_t start_height, bool relay) {
    Version v;
    v.protocol = g_network_config.protocol_version;
    v.services = g_network_config.services;
    v.timestamp = timestamp;
    v.nonce = nonce;
    v.user_agent = user_agent.empty() ? g_network_config.user_agent_prefix : user_agent;
    v.start_height = start_height;
    v.relay = relay;
    
    auto payload = serialize_version(v);
    return build_message_with_checksum("version", payload);
}

// Create verack message
std::vector<uint8_t> create_verack_message() {
    std::vector<uint8_t> empty_payload;
    return build_message_with_checksum("verack", empty_payload);
}

// Create ping message
std::vector<uint8_t> create_ping_message(uint64_t nonce) {
    std::vector<uint8_t> payload;
    put_u64(payload, nonce);
    return build_message_with_checksum("ping", payload);
}

// Create pong message
std::vector<uint8_t> create_pong_message(uint64_t nonce) {
    std::vector<uint8_t> payload;
    put_u64(payload, nonce);
    return build_message_with_checksum("pong", payload);
}

// Parse ping/pong nonce
uint64_t parse_ping_pong_nonce(const std::vector<uint8_t>& payload) {
    if (payload.size() != 8) {
        throw std::runtime_error("Invalid ping/pong payload size");
    }
    BytesReader r{payload};
    return r.get_u64();
}

} // namespace din::p2p
