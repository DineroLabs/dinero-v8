#pragma once

#include <string>

// Version is the git commit hash — the only non-confusing identifier.
// Set at compile time via -DDINERO_CLI_GIT_SHA from CMakeLists.txt.

#ifndef DINERO_CLI_GIT_SHA
#define DINERO_CLI_GIT_SHA "unknown"
#endif

#define DINERO_VERSION_FULL DINERO_CLI_GIT_SHA
#define DINERO_VERSION_STRING "dinerod " DINERO_CLI_GIT_SHA

inline std::string DineroUserAgent(const std::string& network_suffix = "") {
    return std::string("/dinerod:") + DINERO_CLI_GIT_SHA + network_suffix + "/";
}
