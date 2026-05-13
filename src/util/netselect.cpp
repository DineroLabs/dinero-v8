#include "util/netselect.h"
#include "common/logger.h"
#include <filesystem>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

namespace dinero {

// Helper function to find argument in argv
std::string FindArg(int argc, char* argv[], const std::string& arg_name) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == arg_name) {
            // Boolean flag
            return "true";
        } else if (arg.find(arg_name + "=") == 0) {
            // Value argument
            return arg.substr(arg_name.length() + 1);
        }
    }
    return "";
}

bool HasArg(int argc, char* argv[], const std::string& arg_name) {
    return !FindArg(argc, argv, arg_name).empty();
}

Chain DetermineChain(int argc, char* argv[], const fs::path& datadir) {
    // First, validate for conflicts
    ValidateNetworkArgs(argc, argv);
    
    // Helper to parse chain string
    auto parseChain = [](const std::string& v) -> Chain {
        if (v == "mainnet" || v == "" || v == "main") return Chain::MAINNET;
        if (v == "testnet" || v == "test") return Chain::TESTNET;
        if (v == "regtest") return Chain::REGTEST;
        if (v == "auto") throw std::logic_error("AUTO sentinel handled elsewhere");
        throw std::runtime_error("Unknown chain: " + v);
    };
    
    // 1. CLI: -chain=<network> has highest priority
    std::string chainArg = FindArg(argc, argv, "-chain");
    if (!chainArg.empty() && chainArg != "auto" && chainArg != "true") {
        LogNetworkSelection(parseChain(chainArg), "CLI argument -chain=" + chainArg);
        return parseChain(chainArg);
    }
    
    // 2. CLI aliases: -regtest, -testnet
    bool isRegtest = HasArg(argc, argv, "-regtest");
    bool isTestnet = HasArg(argc, argv, "-testnet");
    
    if (isRegtest) {
        LogNetworkSelection(Chain::REGTEST, "CLI flag -regtest");
        return Chain::REGTEST;
    }
    if (isTestnet) {
        LogNetworkSelection(Chain::TESTNET, "CLI flag -testnet");
        return Chain::TESTNET;
    }
    
    // 3. Config file: chain=<network> (lower precedence than CLI)
    // For now, skip config file parsing - can be added later
    
    // 4. AUTO mode: infer from datadir markers
    if (chainArg == "auto") {
        auto detected = AutoDetectNetwork(datadir);
        if (detected.has_value()) {
            LogNetworkSelection(*detected, "auto-detected from datadir markers");
            return *detected;
        }
        // No markers found: default to mainnet
        LogNetworkSelection(Chain::MAINNET, "auto mode with no markers (default to mainnet)");
        return Chain::MAINNET;
    }
    
    // 5. Default: mainnet
    LogNetworkSelection(Chain::MAINNET, "default (no network specified)");
    return Chain::MAINNET;
}

void ValidateNetworkArgs(int argc, char* argv[]) {
    std::string chainArg = FindArg(argc, argv, "-chain");
    bool isRegtest = HasArg(argc, argv, "-regtest");
    bool isTestnet = HasArg(argc, argv, "-testnet");
    
    // Check for conflicting flags
    if (isRegtest && isTestnet) {
        throw std::runtime_error("Conflicting network flags: -regtest and -testnet cannot both be specified");
    }
    
    // Check for conflicts between -chain and aliases
    if (!chainArg.empty() && chainArg != "true" && (isRegtest || isTestnet)) {
        throw std::runtime_error("Conflicting network flags: -chain cannot be used with -regtest or -testnet");
    }
}

std::optional<Chain> AutoDetectNetwork(const fs::path& datadir) {
    struct NetworkProbe {
        const char* name;
        Chain chain;
    };
    
    NetworkProbe probes[] = {
        {"regtest", Chain::REGTEST},
        {"testnet", Chain::TESTNET},
        {"mainnet", Chain::MAINNET},
    };
    
    std::optional<Chain> found;
    std::string foundNetworks;
    
    for (const auto& probe : probes) {
        fs::path netdir = datadir / probe.name;
        
        if (HasNetworkMarkers(netdir)) {
            if (found.has_value() && found.value() != probe.chain) {
                throw std::runtime_error(
                    "Multiple networks detected in datadir (" + foundNetworks + ", " + probe.name + 
                    "). Specify -chain explicitly to resolve ambiguity."
                );
            }
            
            if (!found.has_value()) {
                found = probe.chain;
                foundNetworks = probe.name;
            }
        }
    }
    
    return found;
}

bool HasNetworkMarkers(const fs::path& netdir) {
    if (!fs::exists(netdir)) {
        return false;
    }
    
    // Check for common network-specific files that indicate active use
    std::vector<std::string> markers = {
        ".cookie",           // RPC authentication cookie
        "nodeinfo.json",     // Node information file
        "wallets.db",        // Wallet database
        "chainstate",        // Chainstate directory
        "blocks",            // Blocks directory
        "mempool.dat",       // Mempool persistence
        "peers.dat",         // Peer database
        "banlist.dat"        // Ban list
    };
    
    for (const auto& marker : markers) {
        if (fs::exists(netdir / marker)) {
            return true;
        }
    }
    
    return false;
}

void SetupNetworkDataDir(const fs::path& base_datadir, Chain chain) {
    const auto& params = Params();
    fs::path netdir = base_datadir / params.name;
    
    // Create network-specific directory
    std::error_code ec;
    fs::create_directories(netdir, ec);
    if (ec) {
        throw std::runtime_error("Failed to create network directory " + netdir.string() + ": " + ec.message());
    }
    
    // Change to network directory
    fs::current_path(netdir, ec);
    if (ec) {
        throw std::runtime_error("Failed to change to network directory " + netdir.string() + ": " + ec.message());
    }
    
    dinero::g_logger.info("Network data directory: " + netdir.string());
}

void ValidateNetworkConsistency(Chain selected_chain, const std::string& db_network, bool allow_mismatch) {
    std::string selected_name = ChainToString(selected_chain);
    
    // Normalize database network name
    std::string normalized_db = db_network;
    if (db_network == "main") normalized_db = "mainnet";
    if (db_network == "test") normalized_db = "testnet";
    
    if (selected_name != normalized_db) {
        std::string error_msg = "Network mismatch: selected '" + selected_name + 
                               "' but database contains '" + db_network + "' data";
        
        if (allow_mismatch) {
            dinero::g_logger.warning(error_msg + " (continuing due to -allow-genesis-mismatch)");
        } else {
            throw std::runtime_error(error_msg + ". Use -allow-genesis-mismatch to override (development only).");
        }
    }
}

void LogNetworkSelection(Chain chain, const std::string& selection_reason) {
    const auto& params = Params();
    
    dinero::g_logger.info("Network: " + params.name + " (" + selection_reason + ")");
    dinero::g_logger.info("Address HRP: " + params.hrp);
    dinero::g_logger.info("Default ports - RPC: " + std::to_string(params.rpc_port) + 
                         ", HTTP: " + std::to_string(params.http_port) + 
                         ", WS: " + std::to_string(params.ws_port) + 
                         ", P2P: " + std::to_string(params.p2p_port));
}

} // namespace dinero
