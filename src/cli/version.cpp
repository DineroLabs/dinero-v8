#include "cli/version.h"
#include <iostream>
#include <iomanip>

namespace dinero::cli {

// Version constants - these would be set by CMake/build system
#ifndef DINERO_CLI_VERSION
#define DINERO_CLI_VERSION "1.0.0"
#endif

#ifndef DINERO_CLI_GIT_SHA
#define DINERO_CLI_GIT_SHA "unknown"
#endif

#ifndef DINERO_GIT_COMMIT_FULL
#define DINERO_GIT_COMMIT_FULL DINERO_CLI_GIT_SHA
#endif

#ifndef DINERO_CLI_BUILD_DATE
#define DINERO_CLI_BUILD_DATE __DATE__ " " __TIME__
#endif

#define DINERO_CLI_SCHEMA_TAG "din.cli.v1"

VersionInfo getVersionInfo() {
    return {
        .version = DINERO_CLI_VERSION,
        .gitSha = DINERO_GIT_COMMIT_FULL,  // Use full commit hash
        .buildDate = DINERO_CLI_BUILD_DATE,
        .schemaTag = DINERO_CLI_SCHEMA_TAG
    };
}

void printVersion() {
    auto info = getVersionInfo();
    
    std::cout << "dinero-cli " << info.version << std::endl;
    std::cout << "Git SHA: " << info.gitSha << std::endl;
    std::cout << "Build Date: " << info.buildDate << std::endl;
    std::cout << "Schema: " << info.schemaTag << std::endl;
    std::cout << std::endl;
    std::cout << "Copyright (c) 2024 Dinero Developers" << std::endl;
    std::cout << "Licensed under MIT License" << std::endl;
}

} // namespace dinero::cli
