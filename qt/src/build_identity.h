#pragma once

#include <sstream>
#include <string>

#ifndef DINERO_QT_REPO
#define DINERO_QT_REPO "dinero-qt"
#endif

#ifndef DINERO_QT_COMPONENT
#define DINERO_QT_COMPONENT "dinero-qt"
#endif

#ifndef DINERO_QT_VERSION
#define DINERO_QT_VERSION "unknown"
#endif

#ifndef DINERO_QT_GIT_SHA
#define DINERO_QT_GIT_SHA "unknown"
#endif

#ifndef DINERO_QT_GIT_FULL
#define DINERO_QT_GIT_FULL DINERO_QT_GIT_SHA
#endif

#ifndef DINERO_QT_BUILD_TIME
#define DINERO_QT_BUILD_TIME __DATE__ " " __TIME__
#endif

#ifndef DINERO_RELEASE_TAG
#define DINERO_RELEASE_TAG DINERO_QT_VERSION
#endif

namespace dinero::qt::build {

inline std::string FormatIdentity() {
    std::ostringstream out;
    out << DINERO_QT_COMPONENT << " " << DINERO_QT_VERSION << "\n";
    out << "release_tag: " << DINERO_RELEASE_TAG << "\n";
    out << "repo: " << DINERO_QT_REPO << "\n";
    out << "component: " << DINERO_QT_COMPONENT << "\n";
    out << "commit: " << DINERO_QT_GIT_FULL << "\n";
    out << "build_time: " << DINERO_QT_BUILD_TIME << "\n";
    out << "schema: din.build.v1\n";
    return out.str();
}

}  // namespace dinero::qt::build
