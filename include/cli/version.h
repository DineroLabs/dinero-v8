#pragma once
#include <string>

namespace dinero::cli {

struct VersionInfo {
    std::string version;
    std::string gitSha;
    std::string buildDate;
    std::string schemaTag;
};

// Get version information
VersionInfo getVersionInfo();

// Print version information
void printVersion();

} // namespace dinero::cli
