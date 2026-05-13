#include "daemon/mining_payout_resolver.h"
#include "daemon/address_helpers.h"
#include "common/logger.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <mutex>

namespace dinero {

// Global mining address and script with mutex protection
static std::string g_mining_address;
static std::vector<uint8_t> g_mining_script;
static std::mutex g_mining_mutex;
static std::string g_datadir;

void InitMiningPayoutResolver() {
    g_mining_address = "";
    g_mining_script.clear();
}

void LoadMiningAddress(const std::string& datadir) {
    g_datadir = datadir;
    std::filesystem::path addr_file = std::filesystem::path(datadir) / "mining_address.txt";
    
    if (!std::filesystem::exists(addr_file)) {
        dinero::g_logger.info("No saved mining address found");
        return; // No saved address
    }
    
    std::ifstream file(addr_file);
    if (!file.is_open()) {
        dinero::g_logger.error("Failed to open mining address file: " + addr_file.string());
        return; // Can't read file
    }
    
    std::string addr;
    if (std::getline(file, addr)) {
        // Remove whitespace
        addr.erase(addr.find_last_not_of(" \n\r\t") + 1);
        
        if (!addr.empty()) {
            try {
                SetMiningAddress(addr, GetChainParams());
                dinero::g_logger.info("Loaded mining address from file: " + addr);
            } catch (const std::exception& e) {
                dinero::g_logger.error("Invalid saved mining address '" + addr + "': " + e.what());
                // Remove invalid address file
                std::filesystem::remove(addr_file);
            }
        }
    }
}

bool SetMiningAddress(const std::string& addr, const ChainParamsImpl& params) {
    std::vector<uint8_t> script;
    std::string why;
    
    // Validate address using ToWitnessScript
    if (!ToWitnessScript(addr, script, params, why)) {
        throw std::runtime_error("Invalid mining address: " + why);
    }
    
    // Thread-safe update of both address and script
    {
        std::lock_guard<std::mutex> lock(g_mining_mutex);
        g_mining_address = addr;
        g_mining_script = script;
    }
    
    // Persist to file
    if (!g_datadir.empty()) {
        std::filesystem::path addr_file = std::filesystem::path(g_datadir) / "mining_address.txt";
        std::ofstream file(addr_file);
        if (file.is_open()) {
            file << addr << std::endl;
            file.close();
            dinero::g_logger.info("Saved mining address to file: " + addr);
        } else {
            dinero::g_logger.error("Failed to save mining address to file: " + addr_file.string());
        }
    }
    
    return true;
}

std::string GetMiningAddress() {
    std::lock_guard<std::mutex> lock(g_mining_mutex);
    return g_mining_address;
}

std::vector<uint8_t> GetMiningScript() {
    std::lock_guard<std::mutex> lock(g_mining_mutex);
    return g_mining_script;
}

} // namespace dinero
