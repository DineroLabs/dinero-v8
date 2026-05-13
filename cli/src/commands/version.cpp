#include "dinero/cli/commands/version.hpp"
#include <iostream>
#include <string>

namespace dinero {
namespace cli {

int cmd_version() {
    std::cout << "Dinero CLI v0.6.0-dirty" << std::endl;
    return 0;
}

int cmd_buildinfo() {
    std::cout << "Dinero CLI Build Information" << std::endl;
    std::cout << "================================" << std::endl;
    std::cout << "Version: v0.6.0-dirty" << std::endl;
    
    // Git information (would be populated by CMake)
    #ifdef GIT_SHA
        std::cout << "Git SHA: " << GIT_SHA << std::endl;
    #else
        std::cout << "Git SHA: unknown (not built with git info)" << std::endl;
    #endif
    
    #ifdef GIT_BRANCH
        std::cout << "Git Branch: " << GIT_BRANCH << std::endl;
    #else
        std::cout << "Git Branch: unknown" << std::endl;
    #endif
    
    #ifdef BUILD_TIMESTAMP
        std::cout << "Build Time: " << BUILD_TIMESTAMP << std::endl;
    #else
        std::cout << "Build Time: " << __DATE__ << " " << __TIME__ << std::endl;
    #endif
    
    // Compiler information
    #ifdef __clang__
        std::cout << "Compiler: Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__ << std::endl;
    #elif defined(__GNUC__)
        std::cout << "Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__ << std::endl;
    #else
        std::cout << "Compiler: Unknown" << std::endl;
    #endif
    
    // Build type
    #ifdef CMAKE_BUILD_TYPE
        std::cout << "Build Type: " << CMAKE_BUILD_TYPE << std::endl;
    #else
        #ifdef NDEBUG
            std::cout << "Build Type: Release" << std::endl;
        #else
            std::cout << "Build Type: Debug" << std::endl;
        #endif
    #endif
    
    // Platform information
    #ifdef __APPLE__
        std::cout << "Platform: macOS" << std::endl;
    #elif defined(__linux__)
        std::cout << "Platform: Linux" << std::endl;
    #elif defined(_WIN32)
        std::cout << "Platform: Windows" << std::endl;
    #else
        std::cout << "Platform: Unknown" << std::endl;
    #endif
    
    return 0;
}

} // namespace cli
} // namespace dinero
