#include "daemon_globals.h"
#include "version_config.h"

namespace dinero {
namespace daemon {

// Global daemon state
std::chrono::steady_clock::time_point g_daemon_start_time;
std::string g_external_ip = "";

void InitializeDaemonGlobals() {
    g_daemon_start_time = std::chrono::steady_clock::now();
}

int64_t GetDaemonUptimeSeconds() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - g_daemon_start_time);
    return duration.count();
}

std::string GetDaemonVersion() {
    return DIN_VERSION;
}

std::string GetDaemonGitHash() {
    return DIN_GIT_HASH;
}

std::string GetDaemonBuildTime() {
    return DIN_BUILD_TIME;
}

} // namespace daemon
} // namespace dinero
