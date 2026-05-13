#pragma once

#include <atomic>
#include <chrono>
#include <string>

// ============================================================================
// PHASE D: Daemon Global State for Telemetry
// ============================================================================
// Provides runtime state tracking for health monitoring and diagnostics
// - Zero consensus impact (runtime-only data)
// - Thread-safe atomic counters
// - Uptime tracking for operational visibility
// ============================================================================

namespace dinero {
namespace daemon {

// Daemon startup timestamp (set once at initialization)
extern std::chrono::steady_clock::time_point g_daemon_start_time;

// Initialize daemon globals (call once at startup)
void InitializeDaemonGlobals();

// Get daemon uptime in seconds
int64_t GetDaemonUptimeSeconds();

// Get daemon version string
std::string GetDaemonVersion();

// Get daemon git commit hash
std::string GetDaemonGitHash();

// Get daemon build time
std::string GetDaemonBuildTime();

// Get external IP (if configured via --external-ip)
extern std::string g_external_ip;

} // namespace daemon
} // namespace dinero
