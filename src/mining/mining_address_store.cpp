#include "mining/mining_address_store.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <iomanip>

// Daemon-only version: use std::filesystem + fstream instead of Qt

namespace dinero::miningaddr {

static std::string filePath(const std::string& datadir, const std::string& netName) {
    namespace fs = std::filesystem;
    fs::path p(datadir);
    if (!netName.empty()) {
        p /= netName;
        fs::create_directories(p);
    }
    return (p / "mining.json").string();
}

std::optional<std::string> load(const std::string& datadir, const std::string& netName, std::string* why) {
    namespace fs = std::filesystem;
    const auto path = filePath(datadir, netName);
    if (!fs::exists(path)) return std::nullopt;
    
    std::ifstream f(path);
    if (!f) {
        if (why) *why = "open failed: " + path;
        return std::nullopt;
    }
    
    std::string line;
    std::getline(f, line);
    // Simple JSON parse: {"address":"..."}
    auto pos = line.find("\"address\"");
    if (pos == std::string::npos) return std::nullopt;
    
    auto start = line.find("\"", pos + 9);
    if (start == std::string::npos) return std::nullopt;
    start++;
    
    auto end = line.find("\"", start);
    if (end == std::string::npos) return std::nullopt;
    
    return line.substr(start, end - start);
}

bool save(const std::string& datadir, const std::string& netName, const std::string& address, std::string* why) {
    const auto path = filePath(datadir, netName);
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        if (why) *why = "open failed: " + path;
        return false;
    }
    
    // Get current UTC time
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    
    // Write simple JSON
    f << "{\"address\":\"" << address << "\",\"updated_at\":\"" << oss.str() << "\"}\n";
    f.flush();
    f.close();
    
    return f.good();
}

} // namespace dinero::miningaddr
