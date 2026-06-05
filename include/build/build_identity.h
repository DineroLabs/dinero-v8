#pragma once

#include <string>
#include <sstream>

#ifndef DINERO_BUILD_REPO
#define DINERO_BUILD_REPO "dinero"
#endif

#ifndef DINERO_BUILD_COMPONENT
#define DINERO_BUILD_COMPONENT "unknown"
#endif

#ifndef DINERO_BUILD_DESCRIBE
#define DINERO_BUILD_DESCRIBE ""
#endif

#ifndef DINERO_CLI_VERSION
#define DINERO_CLI_VERSION DINERO_CLI_GIT_SHA
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

namespace dinero::build {

inline constexpr const char* kSchema = "din.build.v1";

struct Identity {
    std::string repo;
    std::string component;
    std::string version;
    std::string describe;
    std::string short_sha;
    std::string full_sha;
    std::string build_time;
    std::string schema;
};

inline Identity CurrentIdentity() {
    return {
        .repo = DINERO_BUILD_REPO,
        .component = DINERO_BUILD_COMPONENT,
        .version = DINERO_CLI_VERSION,
        .describe = DINERO_BUILD_DESCRIBE,
        .short_sha = DINERO_CLI_GIT_SHA,
        .full_sha = DINERO_GIT_COMMIT_FULL,
        .build_time = DINERO_CLI_BUILD_DATE,
        .schema = kSchema,
    };
}

inline std::string FormatIdentityMultiline(const Identity& identity = CurrentIdentity()) {
    std::ostringstream out;
    out << identity.component << " " << identity.version << "\n";
    out << "repo: " << identity.repo << "\n";
    out << "component: " << identity.component << "\n";
    if (!identity.describe.empty()) {
        out << "describe: " << identity.describe << "\n";
    }
    out << "commit: " << identity.full_sha << "\n";
    out << "build_time: " << identity.build_time << "\n";
    out << "schema: " << identity.schema << "\n";
    return out.str();
}

}  // namespace dinero::build
