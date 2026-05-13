#include "p2p/peer.hpp"
#include <iostream>
#include <thread>

int main() {
    try {
        // Initialize network configuration for regtest
        din::p2p::init_network_config("regtest");
        
        std::cout << "=== Dinero P2P Networking Example ===" << std::endl;
        std::cout << "Network magic: 0x" << std::hex << din::p2p::net_magic() << std::dec << std::endl;
        
        boost::asio::io_context io;
        
        // Create peer connection to localhost (assuming dinerod is running)
        auto peer = std::make_shared<din::p2p::Peer>(io, "127.0.0.1", 21000);
        
        std::cout << "Connecting to peer at 127.0.0.1:21000..." << std::endl;
        peer->start();
        
        // Run for 10 seconds then exit
        std::thread timer_thread([&io]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            io.stop();
        });
        
        // Run the event loop
        io.run();
        
        timer_thread.join();
        std::cout << "P2P networking example completed." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
