#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <thread>

static int server_socket = -1;

void signal_handler(int sig) {
    std::cout << "\n🛑 Shutting down P2P listener...\n";
    if (server_socket != -1) {
        close(server_socket);
    }
    exit(0);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << "🚀 **Simple P2P Listener** 🚀\n";
    std::cout << "============================\n";
    std::cout << "Binding to 0.0.0.0:20333 for Node B connections\n\n";
    
    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "❌ Failed to create socket\n";
        return 1;
    }
    
    // Set SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "❌ Failed to set SO_REUSEADDR\n";
        close(server_socket);
        return 1;
    }
    
    // Bind to all interfaces
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0
    server_addr.sin_port = htons(20333);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "❌ Failed to bind to 0.0.0.0:20333\n";
        close(server_socket);
        return 1;
    }
    
    // Listen for connections
    if (listen(server_socket, 5) < 0) {
        std::cerr << "❌ Failed to listen\n";
        close(server_socket);
        return 1;
    }
    
    std::cout << "✅ P2P listener bound to 0.0.0.0:20333\n";
    std::cout << "🔗 Ready for Node B connections!\n";
    std::cout << "Press Ctrl+C to stop\n\n";
    
    // Accept connections
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        
        std::cout << "🔗 Node B connected from " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";
        
        // Simple echo server for testing
        char buffer[1024];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::cout << "📨 Received: " << buffer << "\n";
            
            // Send response
            const char* response = "P2P connection established!\n";
            send(client_socket, response, strlen(response), 0);
        }
        
        close(client_socket);
        std::cout << "🔌 Connection closed\n\n";
    }
    
    return 0;
}
