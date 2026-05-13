#ifndef DINERO_UTILS_H
#define DINERO_UTILS_H

#include <string>
#include <vector>
#include <chrono>
#include <thread>

namespace Dinero {
namespace Common {

// Time utilities
class TimeUtils {
public:
    static uint64_t getCurrentTimestamp();
    static std::string formatTimestamp(uint64_t timestamp);
    static void sleep(uint64_t milliseconds);
};

// String utilities
class StringUtils {
public:
    static std::vector<std::string> split(const std::string& str, char delimiter);
    static std::string join(const std::vector<std::string>& parts, const std::string& delimiter);
    static std::string toLower(const std::string& str);
    static std::string toUpper(const std::string& str);
    static std::string trim(const std::string& str);
    static bool startsWith(const std::string& str, const std::string& prefix);
    static bool endsWith(const std::string& str, const std::string& suffix);
    static std::string replace(const std::string& str, const std::string& from, const std::string& to);
};

// File utilities
class FileUtils {
public:
    static bool fileExists(const std::string& path);
    static std::string readFile(const std::string& path);
    static bool writeFile(const std::string& path, const std::string& content);
    static bool createDirectory(const std::string& path);
    static std::vector<std::string> listFiles(const std::string& path);
    static uint64_t getFileSize(const std::string& path);
};

// Network utilities
class NetworkUtils {
public:
    static std::string getLocalIP();
    static std::string getHostname();
    static bool isPortOpen(const std::string& host, int port);
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);
};

// System utilities
class SystemUtils {
public:
    static int getCPUCount();
    static uint64_t getMemoryUsage();
    static std::string getOSInfo();
    static std::string getArchitecture();
    static bool isLittleEndian();
};

// Math utilities
class MathUtils {
public:
    static double calculateHashRate(uint64_t hashes, uint64_t seconds);
    static uint64_t calculateTarget(uint32_t bits);
    static bool isHashBelowTarget(const std::string& hash, uint64_t target);
    static std::string difficultyToTarget(double difficulty);
    static double targetToDifficulty(uint64_t target);
};

// Validation utilities
class ValidationUtils {
public:
    static bool isValidAddress(const std::string& address);
    static bool isValidHash(const std::string& hash);
    static bool isValidHex(const std::string& hex);
    static bool isValidPort(int port);
    static bool isValidIP(const std::string& ip);
};

} // namespace Common
} // namespace Dinero

#endif // DINERO_UTILS_H 