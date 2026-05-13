#include "solo_miner/build_identity.h"

#include <sstream>

#ifndef DINERO_SOLO_MINER_REPO
#define DINERO_SOLO_MINER_REPO "dinero-solo-miner"
#endif

#ifndef DINERO_SOLO_MINER_COMPONENT
#define DINERO_SOLO_MINER_COMPONENT "dinero-solo-miner"
#endif

#ifndef DINERO_SOLO_MINER_VERSION
#define DINERO_SOLO_MINER_VERSION "unknown"
#endif

#ifndef DINERO_SOLO_MINER_GIT_SHA
#define DINERO_SOLO_MINER_GIT_SHA "unknown"
#endif

#ifndef DINERO_SOLO_MINER_GIT_FULL
#define DINERO_SOLO_MINER_GIT_FULL DINERO_SOLO_MINER_GIT_SHA
#endif

#ifndef DINERO_SOLO_MINER_BUILD_TIME
#define DINERO_SOLO_MINER_BUILD_TIME __DATE__ " " __TIME__
#endif

namespace dinero::solo {

BuildIdentity GetBuildIdentity() {
    return {
        .repo = DINERO_SOLO_MINER_REPO,
        .component = DINERO_SOLO_MINER_COMPONENT,
        .version = DINERO_SOLO_MINER_VERSION,
        .short_sha = DINERO_SOLO_MINER_GIT_SHA,
        .full_sha = DINERO_SOLO_MINER_GIT_FULL,
        .build_time = DINERO_SOLO_MINER_BUILD_TIME,
        .schema = "din.build.v1",
    };
}

std::string FormatBuildIdentity() {
    auto identity = GetBuildIdentity();
    std::ostringstream out;
    out << identity.component << " " << identity.version << "\n";
    out << "repo: " << identity.repo << "\n";
    out << "component: " << identity.component << "\n";
    out << "commit: " << identity.full_sha << "\n";
    out << "build_time: " << identity.build_time << "\n";
    out << "schema: " << identity.schema << "\n";
    return out.str();
}

}  // namespace dinero::solo
