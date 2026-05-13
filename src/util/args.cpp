#include "util/args.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <climits>
#include <cstdio>

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif // !_WIN32

// Global variables for daemonization (defined in main.cpp)
extern int g_argc;
extern char** g_argv;

namespace dinero {
namespace util {

bool parse_port(const std::string& s, int& out) {
    // Check for empty string or too long
    if (s.empty() || s.size() > 5) return false;
    
    // Check all characters are digits
    if (!std::all_of(s.begin(), s.end(), ::isdigit)) return false;
    
    // Parse as long to avoid overflow
    char* endptr;
    long v = std::strtol(s.c_str(), &endptr, 10);
    
    // Check for conversion errors
    if (*endptr != '\0') return false;
    
    // Check valid port range (0 for auto-selection, or 1024-65535, avoiding privileged ports)
    if ((v != 0 && v < 1024) || v > 65535) return false;
    
    out = static_cast<int>(v);
    return true;
}

// daemonize_process implementation moved to end of file

int ArgParser::ParseWSPort(const std::vector<std::string>& args, int default_port) {
    for (const auto& arg : args) {
        // Test the exact same logic as the daemon
        if (arg.substr(0, 8) == "-wsport=") {
            return std::stoi(arg.substr(8));
        } else if (arg.substr(0, 9) == "--wsport=") {
            return std::stoi(arg.substr(9));
        } else if (arg.substr(0, 10) == "--ws-port=") {
            return std::stoi(arg.substr(10));
        }
    }
    return default_port;
}

int ArgParser::ParseRPCPort(const std::vector<std::string>& args, int default_port) {
    for (const auto& arg : args) {
        if (arg.substr(0, 9) == "-rpcport=") {
            return std::stoi(arg.substr(9));
        } else if (arg.substr(0, 10) == "--rpcport=") {
            return std::stoi(arg.substr(10));
        }
    }
    return default_port;
}

int ArgParser::ParseP2PPort(const std::vector<std::string>& args, int default_port) {
    for (const auto& arg : args) {
        if (arg.substr(0, 6) == "-port=") {
            return std::stoi(arg.substr(6));
        } else if (arg.substr(0, 7) == "--port=") {
            return std::stoi(arg.substr(7));
        }
    }
    return default_port;
}

bool ArgParser::ParseDaemonMode(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "-daemon" || arg == "--daemon" || arg == "-daemon=1" || arg == "--daemon=1") {
            return true;
        }
    }
    return false;
}

std::string ArgParser::ParseDataDir(const std::vector<std::string>& args, const std::string& default_dir) {
    for (const auto& arg : args) {
        if (arg.substr(0, 9) == "-datadir=") {
            return arg.substr(9);
        } else if (arg.substr(0, 10) == "--datadir=") {
            return arg.substr(10);
        }
    }
    return default_dir;
}

#ifndef _WIN32
bool daemonize_process() {
    // Use robust daemonization by re-exec to avoid thread::detach issues
    return daemonize_by_reexec();
}

bool daemonize_by_reexec() {
    // Check if we're already daemonized (avoid infinite recursion)
    const char* already_daemon = getenv("DINERO_DAEMON_REEXEC");
    if (already_daemon && strcmp(already_daemon, "1") == 0) {
        // We're already the daemon child process, continue normally
        return true;
    }
    
    // Get the current executable path
    char exe_path[PATH_MAX];
    uint32_t size = sizeof(exe_path);
    
#ifdef __APPLE__
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        return false;
    }
#else
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        return false;
    }
    exe_path[len] = '\0';
#endif
    
    // Prepare arguments for re-exec
    std::vector<std::string> args;
    
    // Get original command line arguments
    for (int i = 0; i < g_argc; i++) {
        std::string arg = g_argv[i];
        // Replace -daemon=1 with -daemon=0 to prevent infinite recursion
        if (arg == "-daemon=1" || arg == "-daemon") {
            args.push_back("-daemon=0");
        } else {
            args.push_back(arg);
        }
    }
    
    // Convert to char* array for exec
    std::vector<char*> argv_exec;
    argv_exec.push_back(const_cast<char*>(exe_path));
    for (size_t i = 1; i < args.size(); i++) { // Skip the first arg (executable name)
        argv_exec.push_back(const_cast<char*>(args[i].c_str()));
    }
    argv_exec.push_back(nullptr);
    
    // Set environment variable to mark daemon process
    setenv("DINERO_DAEMON_REEXEC", "1", 1);
    
#ifdef __APPLE__
    // Use posix_spawn on macOS
    pid_t pid;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    
    // Redirect stdin, stdout, stderr to /dev/null
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    
    int result = posix_spawn(&pid, exe_path, &actions, nullptr, argv_exec.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    
    if (result == 0) {
        // Parent process exits, child continues as daemon
        exit(0);
    }
    return false;
#else
    // Use fork + exec on Linux/Unix
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    
    if (pid > 0) {
        // Parent process - exit successfully
        exit(0);
    }
    
    // Child process - create new session and re-exec
    setsid();
    
    // Redirect standard file descriptors to /dev/null
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    
    // Re-exec with modified arguments
    execv(exe_path, argv_exec.data());
    
    // If we get here, exec failed
    return false;
#endif
}
#endif // !_WIN32

} // namespace util
} // namespace dinero