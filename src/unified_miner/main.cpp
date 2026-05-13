#include <CLI/CLI.hpp>
#include "common/rpc_client.h"
#include "common/rocksdb_manager.h"
#include "common/utils.h"
#include "unified_miner/miner_manager.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace Dinero::Common;

// Global miner manager
Dinero::UnifiedMiner::MinerManager g_miner_manager;

// Subcommand handlers
void handle_gui_miner(CLI::App* app) {
    std::cout << "🎨 Starting GUI Miner..." << std::endl;
    
    int port = 8080;
    std::string config_path = "./config";
    
    app->add_option("--port", port, "GUI port (default: 8080)");
    app->add_option("--config", config_path, "Config directory (default: ./config)");
    
    app->callback([port, config_path]() {
        try {
            g_miner_manager.startGuiMiner(port, config_path);
        } catch (const std::exception& e) {
            std::cerr << "❌ GUI Miner failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

void handle_embedded_miner(CLI::App* app) {
    std::cout << "⚡ Starting Embedded Miner..." << std::endl;
    
    int threads = 4;
    std::string address;
    std::string rpc_url = "http://127.0.0.1:8332";
    std::string rpc_user = "dinero_Dinero_USB_1754372740";
    std::string rpc_pass = "5431cfe6e2b435637ec89dc2a85324c3";
    std::string db_path = "./mining_stats";
    bool benchmark_mode = false;
    int benchmark_duration = 60;
    std::string miner_id = "";
    
    app->add_option("threads", threads, "Number of mining threads (default: 4)")
        ->check(CLI::Range(1, 64));
    app->add_option("address", address, "Mining address")->required();
    app->add_option("--rpc-url", rpc_url, "RPC URL (default: http://127.0.0.1:8332)");
    app->add_option("--rpc-user", rpc_user, "RPC username");
    app->add_option("--rpc-pass", rpc_pass, "RPC password");
    app->add_option("--db-path", db_path, "Database path (default: ./mining_stats)");
    app->add_flag("--benchmark", benchmark_mode, "Run in benchmark mode");
    app->add_option("--benchmark-duration", benchmark_duration, "Benchmark duration in seconds (default: 60)");
    app->add_option("--miner-id", miner_id, "Miner ID (auto-generated if not specified)");
    
    app->callback([=]() {
        try {
            g_miner_manager.startEmbeddedMiner(threads, address, rpc_url, rpc_user, rpc_pass, 
                                              db_path, benchmark_mode, benchmark_duration, miner_id);
        } catch (const std::exception& e) {
            std::cerr << "❌ Embedded Miner failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

void handle_lightweight_miner(CLI::App* app) {
    std::cout << "🚀 Starting Lightweight Miner..." << std::endl;
    
    int threads = 4;
    std::string address;
    std::string rpc_url = "http://127.0.0.1:8332";
    std::string rpc_user = "dinero_Dinero_USB_1754372740";
    std::string rpc_pass = "5431cfe6e2b435637ec89dc2a85324c3";
    
    app->add_option("threads", threads, "Number of mining threads (default: 4)")
        ->check(CLI::Range(1, 64));
    app->add_option("address", address, "Mining address")->required();
    app->add_option("--rpc-url", rpc_url, "RPC URL (default: http://127.0.0.1:8332)");
    app->add_option("--rpc-user", rpc_user, "RPC username");
    app->add_option("--rpc-pass", rpc_pass, "RPC password");
    
    app->callback([=]() {
        try {
            g_miner_manager.startLightweightMiner(threads, address, rpc_url, rpc_user, rpc_pass);
        } catch (const std::exception& e) {
            std::cerr << "❌ Lightweight Miner failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

void handle_status(CLI::App* app) {
    std::cout << "📊 Checking Miner Status..." << std::endl;
    
    app->callback([]() {
        try {
            g_miner_manager.showStatus();
        } catch (const std::exception& e) {
            std::cerr << "❌ Status check failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

void handle_stop(CLI::App* app) {
    std::cout << "🛑 Stopping All Miners..." << std::endl;
    
    std::string miner_type = "all";
    
    app->add_option("miner", miner_type, "Miner type to stop (gui|embedded|light|all) (default: all)");
    
    app->callback([miner_type]() {
        try {
            g_miner_manager.stopMiner(miner_type);
        } catch (const std::exception& e) {
            std::cerr << "❌ Stop failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

void handle_dashboard(CLI::App* app) {
    std::cout << "🎛️ Starting Dashboard..." << std::endl;
    
    int port = 21001;
    bool auto_refresh = true;
    int refresh_interval = 5;
    
    app->add_option("--port", port, "Dashboard port (default: 21001)");
    app->add_flag("--no-auto-refresh", auto_refresh, "Disable auto-refresh");
    app->add_option("--refresh-interval", refresh_interval, "Refresh interval in seconds (default: 5)");
    
    app->callback([=]() {
        try {
            g_miner_manager.startDashboard(port, auto_refresh, refresh_interval);
        } catch (const std::exception& e) {
            std::cerr << "❌ Dashboard failed: " << e.what() << std::endl;
            exit(1);
        }
    });
}

int main(int argc, char** argv) {
    CLI::App app{"Dinero Unified Miner - Single binary for all mining clients"};
    
    app.set_version_flag("--version", "1.0.0");
    app.require_subcommand(1);
    
    // Global options
    bool verbose = false;
    app.add_flag("--verbose,-v", verbose, "Enable verbose output");
    
    // Subcommands
    CLI::App* gui_cmd = app.add_subcommand("gui", "Run GUI miner");
    handle_gui_miner(gui_cmd);
    
    CLI::App* embedded_cmd = app.add_subcommand("embedded", "Run native C++ miner");
    handle_embedded_miner(embedded_cmd);
    
    CLI::App* light_cmd = app.add_subcommand("light", "Run lightweight RPC miner");
    handle_lightweight_miner(light_cmd);
    
    CLI::App* status_cmd = app.add_subcommand("status", "Show miner status");
    handle_status(status_cmd);
    
    CLI::App* stop_cmd = app.add_subcommand("stop", "Stop miners");
    handle_stop(stop_cmd);
    
    CLI::App* dashboard_cmd = app.add_subcommand("dashboard", "Start monitoring dashboard");
    handle_dashboard(dashboard_cmd);
    
    // Parse command line
    CLI11_PARSE(app, argc, argv);
    
    // Set verbose mode
    if (verbose) {
        g_miner_manager.setVerbose(true);
    }
    
    return 0;
} 
