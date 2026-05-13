#pragma once
#include <string>
#include <vector>

namespace dinero {
namespace util {

/**
 * Parse and validate a port number from string
 * @param s Port string to parse
 * @param out Output port number (1024-65535)
 * @return true if valid port, false otherwise
 */
bool parse_port(const std::string& s, int& out);

/**
 * Daemonize the current process (fork and detach from terminal)
 * @return true if successful, false otherwise
 */
bool daemonize_process();

/**
 * Robust daemonization by re-exec to avoid thread::detach issues
 * @return true if successful, false otherwise
 */
bool daemonize_by_reexec();

/**
 * Simple argument parsing utilities for testing
 */
class ArgParser {
public:
    static int ParseWSPort(const std::vector<std::string>& args, int default_port = 18332);
    static int ParseRPCPort(const std::vector<std::string>& args, int default_port = 20998);
    static int ParseP2PPort(const std::vector<std::string>& args, int default_port = 20999);
    static bool ParseDaemonMode(const std::vector<std::string>& args);
    static std::string ParseDataDir(const std::vector<std::string>& args, const std::string& default_dir = "./data");
};

} // namespace util
} // namespace dinero
