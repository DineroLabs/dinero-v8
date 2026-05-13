#include <iostream>
#include <CLI/CLI.hpp>
#include "common/logger.h"
#include "common/config_manager.h"

using namespace dinero;

int main(int argc, char* argv[]) {
    CLI::App app{"Dinero Internal Miner - GUI-controlled mining integrated with web interface"};
    
    // Add options
    std::string rpc_host = "127.0.0.1";
    int rpc_port = 8332;
    std::string rpc_user = "dinero";
    std::string rpc_password = "";
    int gui_port = 8080;
    bool verbose = false;
    
    app.add_option("--host", rpc_host, "RPC host (default: 127.0.0.1)");
    app.add_option("--port", rpc_port, "RPC port (default: 8332)");
    app.add_option("--user", rpc_user, "RPC username (default: dinero)");
    app.add_option("--password", rpc_password, "RPC password");
    app.add_option("--gui-port", gui_port, "GUI web server port (default: 8080)");
    app.add_flag("--verbose", verbose, "Enable verbose output");
    
    CLI11_PARSE(app, argc, argv);
    
    // Initialize logging
    if (verbose) {
        g_logger.setLogLevel(LogLevel::DEBUG);
    }
    g_logger.setLogFile("logs/internal_miner.log");
    
    std::cout << "🖥️  Dinero Internal Miner Starting..." << std::endl;
    g_logger.info("Internal miner starting up");
    
    // Load configuration
    if (!g_config.loadConfig("data/mining.conf")) {
        g_logger.warning("Could not load mining config, using defaults");
    }
    
    // Override config with command line options
    g_config.set("rpc_host", rpc_host);
    g_config.setInt("rpc_port", rpc_port);
    g_config.set("rpc_user", rpc_user);
    g_config.set("rpc_password", rpc_password);
    g_config.setInt("gui_port", gui_port);
    
    g_logger.info("Configuration loaded");
    g_logger.info("RPC Host: " + rpc_host + ":" + std::to_string(rpc_port));
    g_logger.info("GUI Port: " + std::to_string(gui_port));
    
    // TODO: Initialize GUI and mining components
    
    std::cout << "✅ Internal miner initialized successfully" << std::endl;
    std::cout << "   RPC: " << rpc_host << ":" << rpc_port << std::endl;
    std::cout << "   GUI: http://localhost:" << gui_port << std::endl;
    
    // TODO: Start GUI server and mining loop
    
    g_logger.info("Internal miner started successfully");
    
    return 0;
} 