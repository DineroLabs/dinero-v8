# macOS Standalone Build Fix

## Problem

The current CMakeLists.txt has **hardcoded Homebrew paths** throughout:

```cmake
# Line 160
target_link_libraries(dinero_rpc_client PUBLIC /opt/homebrew/lib/libjsoncpp.dylib)

# Line 207-208
/opt/homebrew/lib/libjsoncpp.dylib
/opt/homebrew/lib/libsecp256k1.dylib

# Line 309-310, 433, 458, etc.
# ... many more hardcoded paths ...
```

This means **binaries are compiled with absolute paths to Homebrew libraries**.

When users try to run these binaries on machines without Homebrew installed at `/opt/homebrew/`, they get:

```
dyld: Library not loaded: /opt/homebrew/opt/jsoncpp/lib/libjsoncpp.26.dylib
  Reason: image not found
```

## Solution

We need to **remove ALL hardcoded Homebrew paths** from CMakeLists.txt and use proper CMake `find_package()` and `find_library()` commands instead.

### Step 1: Find jsoncpp properly

**Replace lines 160-165:**
```cmake
if(APPLE)
  target_link_libraries(dinero_rpc_client PUBLIC /opt/homebrew/lib/libjsoncpp.dylib)
elseif(WIN32)
  target_link_libraries(dinero_rpc_client PUBLIC C:/vcpkg/installed/x64-windows-static-md/lib/jsoncpp.lib)
else()
  target_link_libraries(dinero_rpc_client PUBLIC jsoncpp)
endif()
```

**With:**
```cmake
# Find jsoncpp library
find_library(JSONCPP_LIBRARY NAMES jsoncpp PATHS /opt/homebrew/lib /usr/local/lib /usr/lib NO_DEFAULT_PATH)
if(NOT JSONCPP_LIBRARY)
    find_library(JSONCPP_LIBRARY NAMES jsoncpp)
endif()

if(JSONCPP_LIBRARY)
    message(STATUS "Found jsoncpp: ${JSONCPP_LIBRARY}")
    target_link_libraries(dinero_rpc_client PUBLIC ${JSONCPP_LIBRARY})
else()
    message(FATAL_ERROR "jsoncpp not found!")
endif()
```

### Step 2: Find secp256k1 properly

**Replace lines 207-210:**
```cmake
if(APPLE)
  target_link_libraries(dinero_consensus PUBLIC
    dinero_crypto
    /opt/homebrew/lib/libjsoncpp.dylib
    /opt/homebrew/lib/libsecp256k1.dylib
    rocksdb
    sqlite3
  )
```

**With:**
```cmake
# Find secp256k1 library
find_library(SECP256K1_LIBRARY NAMES secp256k1 PATHS /opt/homebrew/lib /usr/local/lib /usr/lib NO_DEFAULT_PATH)
if(NOT SECP256K1_LIBRARY)
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
endif()

if(SECP256K1_LIBRARY)
    message(STATUS "Found secp256k1: ${SECP256K1_LIBRARY}")
else()
    message(FATAL_ERROR "secp256k1 not found!")
endif()

if(APPLE)
  target_link_libraries(dinero_consensus PUBLIC
    dinero_crypto
    ${JSONCPP_LIBRARY}
    ${SECP256K1_LIBRARY}
    rocksdb
    sqlite3
  )
```

### Step 3: Fix ALL other hardcoded paths

Search for `/opt/homebrew` in CMakeLists.txt and replace with proper find_library() calls.

**Lines to fix:**
- Line 160 (dinero_rpc_client)
- Lines 207-208 (dinero_consensus)
- Lines 309-310 (dinerod)
- Line 433 (dinero-miner)
- Line 458 (dinero-cli)
- Line 498, 510, 522, 533, 561-562, 586-587 (tests)

### Step 4: Set proper RPATH

**Add after line 18 (after APPLE check):**
```cmake
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

  # Set RPATH for standalone distribution
  set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
  set(CMAKE_MACOSX_RPATH ON)
endif()
```

### Step 5: Bundle libraries after build

After building, we need to:
1. Copy all non-system `.dylib` files to `lib/` directory
2. Use `install_name_tool` to change absolute paths to `@loader_path/../lib/...`
3. Verify no external dependencies remain

The `build-standalone-macos.sh` script does this automatically.

## Quick Fix (Temporary)

If you don't want to modify CMakeLists.txt immediately, you can use the workaround:

```bash
# Build normally (will have Homebrew paths)
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Use the standalone build script to fix paths
cd ..
./build-standalone-macos.sh
```

This script will:
1. Copy all Homebrew libraries to a bundle
2. Fix paths with install_name_tool
3. Verify the result

## Proper Fix (Recommended)

1. Fix CMakeLists.txt to use `find_library()` instead of hardcoded paths
2. Set proper RPATH settings
3. Build with the fixed CMakeLists.txt
4. Bundle libraries and create DMG

This way, the binaries are built correctly from the start, not patched afterwards.

## Why This Matters

**User quote:** "how do you expect this too work out of the box for all users? if we do shitty job packaging it?"

The user is absolutely right. A proper macOS distribution should:
- ✅ Work on any Mac without Homebrew
- ✅ Have all dependencies bundled
- ✅ Use @loader_path or @rpath for library loading
- ✅ Only link to system libraries (/usr/lib, /System)
- ❌ NOT have absolute paths to /opt/homebrew

This is how Bitcoin Core, Ethereum, and all professional crypto projects distribute their macOS binaries.
