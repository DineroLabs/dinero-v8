// Standalone RPC daemon harness for validation
// Proves HTTP/1.1 + JSON-RPC + cookie auth stack
#include "rpc_http.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
#include <csignal>
#include "compat/net_compat.h"

using json = Json::Value;

static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down rpcd..." << std::endl;
    g_shutdown_requested.store(true);
}

// Mock backend - returns deterministic values for CLI/dev testing
static json mock_dispatch(const std::string& path, const std::string& method,
                          const json& params, const json& id) {
    std::cout << "RPC call: " << method;
    if (!path.empty() && path != "/") {
        std::cout << " (path: " << path << ")";
    }
    if (!params.empty()) {
        std::cout << " params=" << paramsJson::FastWriter().write();
    }
    std::cout << std::endl;

    // Core blockchain methods
    if (method == "getblockchaininfo") {
        return json{
            {"chain", "main"},
            {"blocks", 123456},
            {"headers", 123456},
            {"bestblockhash", "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f"},
            {"difficulty", 1048576.0},
            {"mediantime", 1640995200},
            {"verificationprogress", 1.0},
            {"initialblockdownload", false},
            {"chainwork", "0000000000000000000000000000000000000000000000000000000100010001"},
            {"size_on_disk", 50000000},
            {"pruned", false},
            {"warnings", ""}
        };
    }
    else if (method == "getblockcount") {
        return json(123456);
    }
    else if (method == "getblockhash") {
        if (params.isArray() && params.size() > 0) {
            int height = params[0].asInt();
            // Return deterministic hash based on height
            char hash[65];
            snprintf(hash, sizeof(hash), "%064x", height);
            return json(std::string(hash));
        }
        throw std::runtime_error("Invalid params for getblockhash");
    }
    else if (method == "getblock") {
        if (params.isArray() && params.size() > 0) {
            std::string hash = params[0].asString();
            return json{
                {"hash", hash},
                {"confirmations", 100},
                {"height", 123456},
                {"version", 1},
                {"merkleroot", "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"},
                {"time", 1640995200},
                {"nonce", 2083236893},
                {"bits", "1d00ffff"},
                {"difficulty", 1.0},
                {"tx", json::array({"coinbase_tx_id"})}
            };
        }
        throw std::runtime_error("Invalid params for getblock");
    }
    
    // Mining methods
    else if (method == "getmininginfo") {
        return json{
            {"blocks", 123456},
            {"currentblockweight", 4000000},
            {"currentblocktx", 2500},
            {"difficulty", 1048576.0},
            {"networkhashps", 1000000000000.0},
            {"pooledtx", 50},
            {"chain", "main"},
            {"warnings", ""},
            {"generate", false},
            {"genproclimit", 1}
        };
    }
    else if (method == "setgenerate") {
        if (params.isArray() && params.size() >= 1) {
            bool generate = params[0].asBool();
            int threads = params.size() > 1 ? params[1].asInt() : 1;
            std::cout << "Mock: setgenerate(" << generate << ", " << threads << ")" << std::endl;
            return json(true);
        }
        throw std::runtime_error("Invalid params for setgenerate");
    }
    
    // Network methods
    else if (method == "getnetworkinfo") {
        return json{
            {"version", 100000},
            {"subversion", "/Dinero:1.0.0-rpcd/"},
            {"protocolversion", 70015},
            {"localservices", "0000000000000409"},
            {"localrelay", true},
            {"timeoffset", 0},
            {"connections", 8},
            {"networkactive", true},
            {"networks", json::array()},
            {"relayfee", 0.00001000},
            {"incrementalfee", 0.00001000},
            {"localaddresses", json::array()},
            {"warnings", ""}
        };
    }
    else if (method == "getpeerinfo") {
        return json::array({
            json{
                {"id", 1},
                {"addr", "192.168.1.100:20999"},
                {"services", "0000000000000409"},
                {"lastsend", 1640995200},
                {"lastrecv", 1640995200},
                {"bytessent", 1000000},
                {"bytesrecv", 2000000},
                {"conntime", 1640990000},
                {"version", 70015},
                {"subver", "/Dinero:1.0.0/"},
                {"inbound", false}
            }
        });
    }
    
    // Wallet methods (basic mocks)
    else if (method == "getwalletinfo") {
        return json{
            {"walletname", "default"},
            {"walletversion", 169900},
            {"balance", 50.0},
            {"unconfirmed_balance", 0.0},
            {"immature_balance", 0.0},
            {"txcount", 10},
            {"keypoololdest", 1640990000},
            {"keypoolsize", 1000},
            {"unlocked_until", 0},
            {"paytxfee", 0.0},
            {"hdseedid", "mock_seed_id"}
        };
    }
    else if (method == "getnewaddress") {
        return json("din1qmock_address_for_testing_purposes_only");
    }
    else if (method == "getbalance") {
        return json(50.0);
    }
    
    // Utility methods
    else if (method == "uptime") {
        return json(86400); // 1 day uptime
    }
    else if (method == "help") {
        if (params.isArray() && params.size() > 0) {
            std::string command = params[0].asString();
            return json("Help for " + command + " (mock)");
        }
        return json("Available commands: getblockchaininfo, getblockcount, getmininginfo, setgenerate, getnetworkinfo, getwalletinfo, getnewaddress, getbalance, uptime, help");
    }
    
    // Unknown method
    else {
        throw std::runtime_error("Method not implemented in rpcd mock: " + method);
    }
}

std::string load_cookie() {
    // Try to load cookie from various locations
    std::vector<std::string> cookie_paths = {
        "./rpc.cookie",
        "/data/.cookie",
        "/var/lib/dinero/.cookie",
        std::string(getenv("HOME") ? getenv("HOME") : "") + "/.dinero/.cookie"
    };

    for (const auto& path : cookie_paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string cookie_content;
            std::getline(file, cookie_content);
            if (!cookie_content.empty()) {
                std::cout << "Loaded cookie from: " << path << std::endl;
                return "__cookie__:" + cookie_content;
            }
        }
    }

    // Fallback: use dev cookie
    std::cout << "Using fallback dev cookie" << std::endl;
    return "__cookie__:devcookie";
}

int main(int argc, char* argv[]) {
    std::cout << "Dinero RPC Daemon (rpcd) - Development Harness" << std::endl;
    std::cout << "=================================================" << std::endl;
    
    // Parse command line arguments
    int port = 18332; // Different from daemon (8332) to avoid conflicts
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --port <port>     Set RPC port (default: 18332)" << std::endl;
            std::cout << "  --help, -h        Show this help message" << std::endl;
            return 0;
        }
    }

    // Set up signal handlers
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Load cookie credentials
    std::string cookie = load_cookie();

    // Create socket
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Failed to create server socket" << std::endl;
        return 1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Bind to localhost only (secure by default)
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind server socket to port " << port << std::endl;
        COMPAT_CLOSE_SOCKET(server_socket);
        return 1;
    }

    // Listen for connections
    if (listen(server_socket, 128) < 0) {
        std::cerr << "Failed to listen on server socket" << std::endl;
        COMPAT_CLOSE_SOCKET(server_socket);
        return 1;
    }

    std::cout << "rpcd listening on 127.0.0.1:" << port << std::endl;
    std::cout << "Cookie: " << cookie << std::endl;
    std::cout << "Ready for CLI testing..." << std::endl;

    // Set up RPC context
    rpc::RpcContext ctx{
        cookie,
        [&](const std::string& path, const std::string& method, 
            const json& params, const json& id) -> json {
            return mock_dispatch(path, method, params, id);
        }
    };

    // Main server loop
    while (!g_shutdown_requested.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (!g_shutdown_requested.load()) {
                std::cerr << "Failed to accept client connection" << std::endl;
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Client connected: " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // Handle client in separate thread
        std::thread([client_socket, ctx]() {
            try {
                rpc::handle_rpc_connection(client_socket, ctx);
            } catch (const std::exception& e) {
                std::cerr << "[rpcd] Exception in RPC handler: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[rpcd] Unknown exception in RPC handler" << std::endl;
            }
        }).detach();
    }

    COMPAT_CLOSE_SOCKET(server_socket);
    std::cout << "rpcd shutdown complete" << std::endl;
    return 0;
}
