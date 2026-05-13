#include "common/utils.h"
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include "compat/net_compat.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <sys/utsname.h>
#endif

namespace Dinero {
namespace Common {

// Time utilities
uint64_t TimeUtils::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}

std::string TimeUtils::formatTimestamp(uint64_t timestamp) {
    auto time_val = static_cast<time_t>(timestamp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_val), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void TimeUtils::sleep(uint64_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// String utilities
std::vector<std::string> StringUtils::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string StringUtils::join(const std::vector<std::string>& parts, const std::string& delimiter) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += delimiter;
        result += parts[i];
    }
    return result;
}

std::string StringUtils::toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string StringUtils::toUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

std::string StringUtils::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

bool StringUtils::startsWith(const std::string& str, const std::string& prefix) {
    return str.length() >= prefix.length() && str.substr(0, prefix.length()) == prefix;
}

bool StringUtils::endsWith(const std::string& str, const std::string& suffix) {
    return str.length() >= suffix.length() && str.substr(str.length() - suffix.length()) == suffix;
}

std::string StringUtils::replace(const std::string& str, const std::string& from, const std::string& to) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

// File utilities
bool FileUtils::fileExists(const std::string& path) {
    struct stat buffer;
    return stat(path.c_str(), &buffer) == 0;
}

std::string FileUtils::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool FileUtils::writeFile(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    
    file << content;
    return true;
}

bool FileUtils::createDirectory(const std::string& path) {
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), NULL) != 0;
#else
    return mkdir(path.c_str(), 0755) == 0;
#endif
}

std::vector<std::string> FileUtils::listFiles(const std::string& path) {
    std::vector<std::string> files;
    // Implementation would depend on platform-specific directory reading
    return files;
}

uint64_t FileUtils::getFileSize(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) return 0;
    return static_cast<uint64_t>(buffer.st_size);
}

// Network utilities
std::string NetworkUtils::getLocalIP() {
    // Simplified implementation - would need more robust network detection
    return "127.0.0.1";
}

std::string NetworkUtils::getHostname() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }
    return "unknown";
}

bool NetworkUtils::isPortOpen(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(host.c_str());
    
    int result = connect(sock, (struct sockaddr*)&server, sizeof(server));
    COMPAT_CLOSE_SOCKET(sock);
    
    return result == 0;
}

std::string NetworkUtils::urlEncode(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            result += hex;
        }
    }
    return result;
}

std::string NetworkUtils::urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            sscanf(str.substr(i + 1, 2).c_str(), "%x", &value);
            result += static_cast<char>(value);
            i += 2;
        } else {
            result += str[i];
        }
    }
    return result;
}

// System utilities
int SystemUtils::getCPUCount() {
    return static_cast<int>(std::thread::hardware_concurrency());
}

uint64_t SystemUtils::getMemoryUsage() {
    // Simplified implementation - would need platform-specific memory detection
    return 0;
}

std::string SystemUtils::getOSInfo() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string SystemUtils::getArchitecture() {
#ifdef _WIN32
    return "x86_64";
#else
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        return std::string(buffer.machine);
    }
    return "unknown";
#endif
}

bool SystemUtils::isLittleEndian() {
    union {
        uint16_t value;
        uint8_t bytes[2];
    } test = {0x0102};
    return test.bytes[0] == 0x02;
}

// Math utilities
double MathUtils::calculateHashRate(uint64_t hashes, uint64_t seconds) {
    if (seconds == 0) return 0.0;
    return static_cast<double>(hashes) / static_cast<double>(seconds);
}

uint64_t MathUtils::calculateTarget(uint32_t bits) {
    // Bitcoin-style target calculation
    uint8_t exp = (bits >> 24) & 0xFF;
    uint32_t mantissa = bits & 0xFFFFFF;
    
    if (exp <= 3) {
        return mantissa >> (8 * (3 - exp));
    } else {
        return static_cast<uint64_t>(mantissa) << (8 * (exp - 3));
    }
}

bool MathUtils::isHashBelowTarget(const std::string& hash, uint64_t target) {
    // Convert hash to integer and compare with target
    // This is a simplified implementation
    return true; // Placeholder
}

std::string MathUtils::difficultyToTarget(double difficulty) {
    // Convert difficulty to target
    uint64_t target = static_cast<uint64_t>(0xFFFFULL * 256ULL * 256ULL * 256ULL / difficulty);
    return std::to_string(target);
}

double MathUtils::targetToDifficulty(uint64_t target) {
    // Convert target to difficulty
    return static_cast<double>(0xFFFFULL * 256ULL * 256ULL * 256ULL) / static_cast<double>(target);
}

// Validation utilities
bool ValidationUtils::isValidAddress(const std::string& address) {
    // Basic Dinero address validation
    return address.length() >= 26 && address.substr(0, 3) == "hc1";
}

bool ValidationUtils::isValidHash(const std::string& hash) {
    // Basic hash validation (64 hex characters)
    if (hash.length() != 64) return false;
    for (char c : hash) {
        if (!isxdigit(c)) return false;
    }
    return true;
}

bool ValidationUtils::isValidHex(const std::string& hex) {
    for (char c : hex) {
        if (!isxdigit(c)) return false;
    }
    return true;
}

bool ValidationUtils::isValidPort(int port) {
    return port > 0 && port <= 65535;
}

bool ValidationUtils::isValidIP(const std::string& ip) {
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) == 1;
}

} // namespace Common
} // namespace Dinero 