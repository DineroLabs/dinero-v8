#include "p2p/p2p_wire_protocol.h"
#include <iostream>
#include <chrono>
#include <random>

using namespace din::p2p;

int main() {
    try {
        // Initialize network configuration for regtest
        init_network_config("regtest");
        
        std::cout << "=== Dinero P2P Wire Protocol Example ===" << std::endl;
        std::cout << "Network magic: 0x" << std::hex << net_magic() << std::dec << std::endl;
        
        // Generate random nonce
        std::random_device rd;
        std::mt19937_64 gen(rd());
        uint64_t nonce = gen();
        
        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        
        // 1. Create version message
        std::cout << "\n--- Creating Version Message ---" << std::endl;
        auto version_msg = create_version_message(timestamp, nonce, "/dinerod:0.6.0-example/", 12345, true);
        std::cout << "Version message size: " << version_msg.size() << " bytes" << std::endl;
        
        // Parse the header to show structure
        MsgHeader header;
        if (parse_header(version_msg.data(), header)) {
            std::cout << "Command: " << extract_command(header.command) << std::endl;
            std::cout << "Payload length: " << header.length << " bytes" << std::endl;
            std::cout << "Checksum: 0x" << std::hex << header.checksum << std::dec << std::endl;
        }
        
        // Extract and parse payload
        std::vector<uint8_t> payload(version_msg.begin() + 24, version_msg.end());
        Version parsed_version = parse_version(payload);
        std::cout << "Parsed version:" << std::endl;
        std::cout << "  Protocol: " << parsed_version.protocol << std::endl;
        std::cout << "  Services: " << parsed_version.services << std::endl;
        std::cout << "  User agent: " << parsed_version.user_agent << std::endl;
        std::cout << "  Start height: " << parsed_version.start_height << std::endl;
        std::cout << "  Relay: " << (parsed_version.relay ? "true" : "false") << std::endl;
        
        // 2. Create verack message
        std::cout << "\n--- Creating Verack Message ---" << std::endl;
        auto verack_msg = create_verack_message();
        std::cout << "Verack message size: " << verack_msg.size() << " bytes" << std::endl;
        
        // 3. Create ping/pong messages
        std::cout << "\n--- Creating Ping/Pong Messages ---" << std::endl;
        uint64_t ping_nonce = gen();
        auto ping_msg = create_ping_message(ping_nonce);
        auto pong_msg = create_pong_message(ping_nonce);
        
        std::cout << "Ping message size: " << ping_msg.size() << " bytes" << std::endl;
        std::cout << "Pong message size: " << pong_msg.size() << " bytes" << std::endl;
        
        // Parse ping nonce to verify
        std::vector<uint8_t> ping_payload(ping_msg.begin() + 24, ping_msg.end());
        uint64_t parsed_nonce = parse_ping_pong_nonce(ping_payload);
        std::cout << "Ping nonce: " << ping_nonce << std::endl;
        std::cout << "Parsed nonce: " << parsed_nonce << std::endl;
        std::cout << "Nonce match: " << (ping_nonce == parsed_nonce ? "✓" : "✗") << std::endl;
        
        // 4. Test DoS protection
        std::cout << "\n--- Testing DoS Protection ---" << std::endl;
        
        // Test user agent capping
        std::string long_user_agent(500, 'A');  // 500 chars, should be capped to 256
        Version v_test;
        v_test.user_agent = long_user_agent;
        auto capped_payload = serialize_version(v_test);
        Version parsed_capped = parse_version(capped_payload);
        std::cout << "Original user agent length: " << long_user_agent.size() << std::endl;
        std::cout << "Capped user agent length: " << parsed_capped.user_agent.size() << std::endl;
        std::cout << "DoS protection working: " << (parsed_capped.user_agent.size() <= 256 ? "✓" : "✗") << std::endl;
        
        // 5. Test message validation
        std::cout << "\n--- Testing Message Validation ---" << std::endl;
        std::vector<uint8_t> test_payload(version_msg.begin() + 24, version_msg.end());
        bool is_valid = validate_message(version_msg.data(), test_payload);
        std::cout << "Message validation: " << (is_valid ? "✓" : "✗") << std::endl;
        
        // Test with corrupted checksum
        std::vector<uint8_t> corrupted_msg = version_msg;
        corrupted_msg[20] ^= 0xFF;  // Flip bits in checksum
        bool is_invalid = validate_message(corrupted_msg.data(), test_payload);
        std::cout << "Corrupted message rejected: " << (!is_invalid ? "✓" : "✗") << std::endl;
        
        std::cout << "\n=== All tests completed successfully! ===" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
