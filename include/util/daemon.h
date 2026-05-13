#pragma once

#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>

namespace dinero {
namespace util {

/**
 * Portable daemonization function that works across platforms
 * without deprecated warnings. Uses standard double-fork technique.
 * 
 * @return true on success, false on failure
 */
inline bool daemonize_process() {
    // First fork
    pid_t pid = fork();
    if (pid < 0) return false;  // Fork failed
    if (pid > 0) _exit(0);      // Parent exits
    
    // Child becomes session leader
    if (setsid() < 0) return false;
    
    // Second fork to prevent acquiring controlling terminal
    pid = fork();
    if (pid < 0) return false;  // Fork failed
    if (pid > 0) _exit(0);      // First child exits
    
    // Set file creation mask
    umask(027);
    
    // Redirect standard file descriptors to /dev/null
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    
    // Change to root directory to avoid keeping any directory in use
    chdir("/");
    
    return true;
}

} // namespace util
} // namespace dinero

