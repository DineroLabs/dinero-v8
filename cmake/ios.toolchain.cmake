# ios.toolchain.cmake
# CMake toolchain file for cross-compiling Dinero NodeCore to iOS arm64.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DIOS_PLATFORM=OS ..
#
# Supported iOS platforms:
#   OS          - iPhone/iPad device (arm64)
#   SIMULATOR   - iPhone Simulator (arm64 for Apple Silicon)

# Platform selection
if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS" CACHE STRING "iOS platform: OS or SIMULATOR")
endif()

# System identification
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_OSX_ARCHITECTURES arm64)

# Deployment target
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "iOS deployment target")
endif()

# SDK selection
if(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(CMAKE_OSX_SYSROOT iphonesimulator)
else()
    set(CMAKE_OSX_SYSROOT iphoneos)
endif()

# Force static libraries (no dynamic linking on iOS)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Position-independent code (required for static libs linked into frameworks)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disable features not available on iOS
set(BUILD_DINEROD OFF CACHE BOOL "" FORCE)
set(BUILD_LIGHTNINGD OFF CACHE BOOL "" FORCE)
set(BUILD_GUI OFF CACHE BOOL "" FORCE)
set(ENABLE_GRPC OFF CACHE BOOL "" FORCE)
set(ENABLE_GPU_MINING OFF CACHE BOOL "" FORCE)
set(ENABLE_LIGHTNING OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_FUZZING OFF CACHE BOOL "" FORCE)
set(ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
if(BUILD_NODECORE)
    set(ENABLE_ZK ON CACHE BOOL "Enable zero-knowledge privacy features for NodeCore iOS builds" FORCE)
else()
    set(ENABLE_ZK OFF CACHE BOOL "" FORCE)
endif()

# Use vendored OpenSSL (no Homebrew on iOS)
set(USE_SYSTEM_OPENSSL OFF CACHE BOOL "" FORCE)

# Compile definitions
add_compile_definitions(
    IOS_BUILD
    DISABLE_GRPC
    DINERO_RELEASE_BUILD
    # Note: Do NOT define _LIBCPP_DISABLE_AVAILABILITY here.
    # Xcode 26+ libc++ introduced __hash_memory (LLVM 21) which is declared
    # in the SDK headers but absent from the iOS simulator runtime.  Vendor
    # availability annotations correctly gate it; disabling them causes a
    # dyld symbol-not-found crash on the simulator.
)

# iOS-specific compiler flags
# Explicitly set deployment target so libc++ availability checks pass
# (Unix Makefiles generator doesn't auto-translate CMAKE_OSX_DEPLOYMENT_TARGET)
if(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(IOS_VERSION_FLAG "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
    set(IOS_VERSION_FLAG "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fembed-bitcode-marker ${IOS_VERSION_FLAG}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fembed-bitcode-marker -std=c++17 ${IOS_VERSION_FLAG}")

# Suppress warnings that are noisy for cross-compilation
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-shorten-64-to-32")

# Tell CMake not to try to run test programs during configuration
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
