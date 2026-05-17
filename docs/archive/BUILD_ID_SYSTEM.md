# Build Identification & Consistency System

## The Problem

**Current state**: Building binaries is like roulette - no consistency, no traceability:
- ❌ Same source code produces different binaries on different days
- ❌ No way to know which source/commit was used for a binary
- ❌ No way to verify a binary matches its claimed version
- ❌ Build flags and configuration not tracked
- ❌ Dependencies versions not recorded

**Result**: We can't reproduce builds or verify what we're distributing.

---

## The Solution: Build ID System

Every binary should display at startup:
1. **Git commit hash** - Exact source code version
2. **Build timestamp** - When it was built
3. **Build host** - Where it was built (Mac/Linux/Docker)
4. **Compiler version** - What compiled it
5. **Build flags** - How it was compiled
6. **Dependencies** - Library versions used

---

## Implementation

### Step 1: Add Build ID to Version Header

**File**: `include/version.h`
```cpp
#pragma once
#include <string>

namespace dinero {
namespace version {

// Version information
constexpr const char* VERSION = "0.1.0";
constexpr const char* PROTOCOL_VERSION = "70001";

// Build identification (set at compile time)
extern const char* GIT_COMMIT;
extern const char* BUILD_TIMESTAMP;
extern const char* BUILD_HOST;
extern const char* BUILD_TYPE;
extern const char* COMPILER_VERSION;
extern const char* BUILD_FLAGS;

// Library versions
extern const char* ROCKSDB_VERSION;
extern const char* OPENSSL_VERSION;
extern const char* SQLITE_VERSION;
extern const char* BOOST_VERSION;

// Get full version string with build ID
std::string GetFullVersion();
std::string GetBuildInfo();

} // namespace version
} // namespace dinero
```

### Step 2: Generate Build ID at Compile Time

**File**: `cmake/BuildInfo.cmake`
```cmake
# Generate build identification information
function(generate_build_info TARGET)
    # Get Git commit hash
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Get short commit hash
    execute_process(
        COMMAND git rev-parse --short=8 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Get build timestamp
    string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

    # Get build host
    cmake_host_system_information(RESULT BUILD_HOST QUERY HOSTNAME)
    cmake_host_system_information(RESULT BUILD_OS QUERY OS_NAME)
    cmake_host_system_information(RESULT BUILD_ARCH QUERY OS_PLATFORM)
    set(BUILD_HOST "${BUILD_HOST} (${BUILD_OS} ${BUILD_ARCH})")

    # Get compiler version
    set(COMPILER_VERSION "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    # Get build type
    set(BUILD_TYPE_INFO "${CMAKE_BUILD_TYPE}")

    # Collect build flags
    set(BUILD_FLAGS_INFO "")
    if(CMAKE_CXX_FLAGS)
        set(BUILD_FLAGS_INFO "${CMAKE_CXX_FLAGS}")
    endif()
    if(CMAKE_CXX_FLAGS_RELEASE)
        set(BUILD_FLAGS_INFO "${BUILD_FLAGS_INFO} ${CMAKE_CXX_FLAGS_RELEASE}")
    endif()

    # Generate version.cpp with build info
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/version.cpp.in
        ${CMAKE_BINARY_DIR}/generated/version.cpp
        @ONLY
    )

    # Add generated file to target
    target_sources(${TARGET} PRIVATE ${CMAKE_BINARY_DIR}/generated/version.cpp)
    target_include_directories(${TARGET} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
```

### Step 3: Version Source Template

**File**: `cmake/version.cpp.in`
```cpp
#include "version.h"
#include <sstream>
#include <rocksdb/version.h>
#include <openssl/opensslv.h>
#include <sqlite3.h>
#include <boost/version.hpp>

namespace dinero {
namespace version {

// Build information (generated at compile time)
const char* GIT_COMMIT = "@GIT_COMMIT@";
const char* BUILD_TIMESTAMP = "@BUILD_TIMESTAMP@";
const char* BUILD_HOST = "@BUILD_HOST@";
const char* BUILD_TYPE = "@BUILD_TYPE_INFO@";
const char* COMPILER_VERSION = "@COMPILER_VERSION@";
const char* BUILD_FLAGS = "@BUILD_FLAGS_INFO@";

// Library versions (detected at compile time)
const char* ROCKSDB_VERSION = ROCKSDB_VERSION_STRING;
const char* OPENSSL_VERSION = OPENSSL_VERSION_TEXT;
const char* SQLITE_VERSION = SQLITE_VERSION;
const char* BOOST_VERSION = BOOST_LIB_VERSION;

std::string GetFullVersion() {
    std::ostringstream oss;
    oss << "Dinero v" << VERSION
        << " (" << GIT_COMMIT << ")\n"
        << "Built: " << BUILD_TIMESTAMP << "\n"
        << "Host: " << BUILD_HOST;
    return oss.str();
}

std::string GetBuildInfo() {
    std::ostringstream oss;
    oss << "Build Information:\n"
        << "  Version:    " << VERSION << "\n"
        << "  Git:        " << GIT_COMMIT << "\n"
        << "  Timestamp:  " << BUILD_TIMESTAMP << "\n"
        << "  Host:       " << BUILD_HOST << "\n"
        << "  Type:       " << BUILD_TYPE << "\n"
        << "  Compiler:   " << COMPILER_VERSION << "\n"
        << "  Flags:      " << BUILD_FLAGS << "\n"
        << "\n"
        << "Dependencies:\n"
        << "  RocksDB:    " << ROCKSDB_VERSION << "\n"
        << "  OpenSSL:    " << OPENSSL_VERSION << "\n"
        << "  SQLite:     " << SQLITE_VERSION << "\n"
        << "  Boost:      " << BOOST_VERSION << "\n";
    return oss.str();
}

} // namespace version
} // namespace dinero
```

### Step 4: Display on Startup

**Update daemon startup** (`src/daemon/main.cpp`):
```cpp
#include "version.h"

int main(int argc, char* argv[]) {
    // Display version banner
    std::cout << "========================================\n";
    std::cout << dinero::version::GetFullVersion() << "\n";
    std::cout << "========================================\n";
    std::cout << "\n";

    // If --version or --buildinfo requested
    if (args.count("version")) {
        std::cout << dinero::version::GetFullVersion() << "\n";
        return 0;
    }

    if (args.count("buildinfo")) {
        std::cout << dinero::version::GetBuildInfo() << "\n";
        return 0;
    }

    // ... rest of main
}
```

---

## Expected Output

### Normal Startup
```bash
$ ./dinerod
========================================
Dinero v0.1.0 (7c898171a4b2c3d4)
Built: 2025-10-30 21:45:32 UTC
Host: MacBook-Pro.local (Darwin arm64)
========================================

Starting Dinero daemon...
RPC server listening on 127.0.0.1:20998
P2P listening on 0.0.0.0:19003
```

### Version Flag
```bash
$ ./dinerod --version
Dinero v0.1.0 (7c898171a4b2c3d4)
Built: 2025-10-30 21:45:32 UTC
Host: MacBook-Pro.local (Darwin arm64)
```

### Build Info Flag
```bash
$ ./dinerod --buildinfo
Build Information:
  Version:    0.1.0
  Git:        7c898171a4b2c3d4f5e6a7b8c9d0e1f2
  Timestamp:  2025-10-30 21:45:32 UTC
  Host:       MacBook-Pro.local (Darwin arm64)
  Type:       Release
  Compiler:   AppleClang 15.0.0.15000100
  Flags:      -O3 -DNDEBUG -march=native

Dependencies:
  RocksDB:    9.1.1
  OpenSSL:    3.3.2
  SQLite:     3.48.0
  Boost:      1_85_0
```

---

## Build Consistency Rules

### Rule 1: Build from Clean Repo
```bash
# BEFORE building, verify clean state
git status
# Should show: "nothing to commit, working tree clean"

# If dirty, commit or stash
git add .
git commit -m "Build v0.1.0"
```

### Rule 2: Tag Builds
```bash
# After successful build and test
git tag -a v0.1.0-build-$(date +%Y%m%d) -m "macOS build $(date)"
git push --tags
```

### Rule 3: Document Build Environment
Create a file `BUILD_RECORD.md` for each release:
```markdown
# Build Record: v0.1.0 (2025-10-30)

## Source
- Git commit: 7c898171a4b2c3d4f5e6a7b8c9d0e1f2
- Branch: main
- Tag: v0.1.0-build-20251030

## macOS Build
- Host: MacBook-Pro.local
- OS: macOS 14.6.1 (23G93)
- Arch: arm64 (Apple Silicon)
- Xcode: 15.0.0
- CMake: 3.27.7
- Build type: Release
- Build flags: -O3 -DNDEBUG

## Dependencies
- RocksDB: 9.1.1 (vendored)
- OpenSSL: 3.3.2 (vendored)
- SQLite: 3.48.0 (vendored)
- Boost: 1.85.0 (vendored)
- Qt6: 6.9.1 (Homebrew)

## Binary Checksums (SHA256)
dinerod:      e7fd274813a936132fdc510e41cf7e35a4ead5cd1545df4d3ed6c877f67f0087
dinero-cli:   8097db8037b7f28340fc2193c72033e9888a7e0d5baa9a6e7631c8a4a6d0c57e
dinero-miner: b18c6b24aa9b56943c728dcd2e88da955bef3956b4e72c11a31565467568f489
dinero-qt:    e293662eb6a3bdea6acdbe41f2d03c8fa67f146761e1a26516e41487410b90a7

## Testing
- ✅ Daemon starts successfully
- ✅ GUI opens with all features
- ✅ RPC commands work
- ✅ P2P connections established
- ✅ Mining functional
```

### Rule 4: Reproducible Build Script
Create `scripts/build-release.sh`:
```bash
#!/bin/bash
set -e

echo "========================================="
echo "  Dinero Release Build Script"
echo "========================================="
echo ""

# Check clean repo
if [ -n "$(git status --porcelain)" ]; then
    echo "❌ Error: Working directory is not clean"
    git status --short
    exit 1
fi

# Get commit info
GIT_COMMIT=$(git rev-parse HEAD)
GIT_SHORT=$(git rev-parse --short=8 HEAD)
BUILD_DATE=$(date +%Y-%m-%d)

echo "Git commit: $GIT_COMMIT"
echo "Build date: $BUILD_DATE"
echo ""

# Clean previous build
rm -rf build
mkdir build
cd build

# Configure
echo "=== Configuring ==="
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_VENDORED_ROCKSDB=ON \
    -DENABLE_SANITIZERS=OFF \
    -DBUILD_GUI=ON

# Build
echo ""
echo "=== Building ==="
cmake --build . --config Release -j$(sysctl -n hw.ncpu)

# Verify binaries
echo ""
echo "=== Binary Sizes ==="
ls -lh dinerod dinero-cli dinero-miner gui/dinero-qt

# Test version
echo ""
echo "=== Version Check ==="
./dinerod --version

# Generate checksums
echo ""
echo "=== SHA256 Checksums ==="
shasum -a 256 dinerod dinero-cli dinero-miner gui/dinero-qt

echo ""
echo "✅ Build complete!"
echo "Binaries in: $(pwd)"
```

---

## Immediate Action Items

### 1. Add Build ID System (2-3 hours)
- [ ] Create `include/version.h`
- [ ] Create `cmake/BuildInfo.cmake`
- [ ] Create `cmake/version.cpp.in`
- [ ] Update `CMakeLists.txt` to use `generate_build_info()`
- [ ] Update daemon/CLI/miner/GUI to display build info

### 2. Test Build ID (30 min)
- [ ] Build on macOS
- [ ] Verify `--version` shows correct info
- [ ] Verify `--buildinfo` shows all details
- [ ] Verify different commits show different hashes

### 3. Document Current Builds (30 min)
- [ ] Create `BUILD_RECORD.md` for current v0.1.0
- [ ] Record MAC_DINERO build environment
- [ ] Save checksums

### 4. Create Build Script (30 min)
- [ ] Create `scripts/build-release.sh`
- [ ] Test on macOS
- [ ] Create Linux version

---

## Benefits

### Before (Current State)
```bash
$ ./dinerod --version
Dinero Daemon v0.1.0 (7c898171)
Built: 2025-10-28T22:35:37+0000

# ❌ No way to know:
#    - Exact source commit
#    - Build host
#    - Compiler version
#    - Dependency versions
#    - Build flags
```

### After (With Build ID)
```bash
$ ./dinerod --buildinfo
Build Information:
  Version:    0.1.0
  Git:        7c898171a4b2c3d4f5e6a7b8c9d0e1f2  ✅ Exact source
  Timestamp:  2025-10-30 21:45:32 UTC           ✅ When built
  Host:       MacBook-Pro.local (Darwin arm64)  ✅ Where built
  Type:       Release                           ✅ Build type
  Compiler:   AppleClang 15.0.0                ✅ Compiler version
  Flags:      -O3 -DNDEBUG                     ✅ Build flags

Dependencies:                                   ✅ Library versions
  RocksDB:    9.1.1
  OpenSSL:    3.3.2
  SQLite:     3.48.0
  Boost:      1_85_0

# ✅ Now we can:
#    - Reproduce exact build
#    - Verify what's running
#    - Debug issues with full context
#    - Trust our binaries
```

---

## Long-term: Reproducible Builds

Eventually, aim for **deterministic builds** where:
- Same source + same flags = **identical binary** (bit-for-bit)
- Anyone can verify they have the correct binary
- No trust required

**Tools**:
- Docker for consistent build environment
- Fixed timestamps (SOURCE_DATE_EPOCH)
- Stripped debug info
- Canonical file ordering

---

## Summary

**Problem**: Building binaries is roulette - no consistency or traceability.

**Solution**:
1. ✅ **Build ID system** - Every binary identifies itself
2. ✅ **Build records** - Document every release
3. ✅ **Build scripts** - Reproducible process
4. ✅ **Verification** - Checksums + build info matching

**Result**:
- Know exactly what source built each binary
- Reproduce builds consistently
- Verify distributed binaries
- Debug issues with full context

**No more roulette!**
