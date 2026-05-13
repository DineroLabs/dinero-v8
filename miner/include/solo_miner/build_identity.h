#pragma once

#include <string>

namespace dinero::solo {

struct BuildIdentity {
    std::string repo;
    std::string component;
    std::string version;
    std::string short_sha;
    std::string full_sha;
    std::string build_time;
    std::string schema;
};

BuildIdentity GetBuildIdentity();
std::string FormatBuildIdentity();

}  // namespace dinero::solo
