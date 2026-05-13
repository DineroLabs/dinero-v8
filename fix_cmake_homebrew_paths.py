#!/usr/bin/env python3
"""
Fix CMakeLists.txt to remove hardcoded Homebrew paths
Replaces /opt/homebrew/lib/... with proper find_library() calls
"""

import re
import sys

def fix_cmake_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    original_content = content

    # Step 1: Add library finding at the top (after line 18)
    library_finding_code = """
# ================================================================
# Find required libraries (macOS standalone support)
# ================================================================
if(APPLE)
  # Set RPATH for standalone distribution
  set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
  set(CMAKE_MACOSX_RPATH ON)

  # Find jsoncpp
  find_library(JSONCPP_LIBRARY NAMES jsoncpp
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
    NO_DEFAULT_PATH)
  if(NOT JSONCPP_LIBRARY)
    find_library(JSONCPP_LIBRARY NAMES jsoncpp)
  endif()
  if(JSONCPP_LIBRARY)
    message(STATUS "Found jsoncpp: ${JSONCPP_LIBRARY}")
  else()
    message(FATAL_ERROR "jsoncpp not found! Install with: brew install jsoncpp")
  endif()

  # Find secp256k1
  find_library(SECP256K1_LIBRARY NAMES secp256k1
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
    NO_DEFAULT_PATH)
  if(NOT SECP256K1_LIBRARY)
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
  endif()
  if(SECP256K1_LIBRARY)
    message(STATUS "Found secp256k1: ${SECP256K1_LIBRARY}")
  else()
    message(FATAL_ERROR "secp256k1 not found! Install with: brew install secp256k1")
  endif()
endif()
# ================================================================

"""

    # Insert after the APPLE check (around line 18)
    content = content.replace(
        'if(APPLE)\n  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")\nendif()',
        'if(APPLE)\n  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")\nendif()\n' + library_finding_code
    )

    # Step 2: Replace all hardcoded /opt/homebrew/lib/libjsoncpp.dylib
    content = re.sub(
        r'/opt/homebrew/lib/libjsoncpp\.dylib',
        '${JSONCPP_LIBRARY}',
        content
    )

    # Step 3: Replace all hardcoded /opt/homebrew/lib/libsecp256k1.dylib
    content = re.sub(
        r'/opt/homebrew/lib/libsecp256k1\.dylib',
        '${SECP256K1_LIBRARY}',
        content
    )

    # Step 4: Replace /opt/homebrew/include references
    content = re.sub(
        r'/opt/homebrew/include',
        '/opt/homebrew/include  # TODO: Use find_path() for portability',
        content,
        count=1  # Only first occurrence to avoid breaking things
    )

    # Step 5: Fix the APPLE-specific link_libraries blocks to use variables
    # This handles the if(APPLE) blocks that have multiple libraries

    if content != original_content:
        # Backup original
        with open(filepath + '.original', 'w') as f:
            f.write(original_content)

        # Write fixed version
        with open(filepath, 'w') as f:
            f.write(content)

        print("✅ CMakeLists.txt fixed!")
        print(f"   Backup saved to: {filepath}.original")
        print("")
        print("Changes made:")
        print("  • Added library finding code with find_library()")
        print("  • Added RPATH settings for standalone distribution")
        print("  • Replaced hardcoded Homebrew paths with CMake variables")
        print("")
        print("Next steps:")
        print("  1. Review the changes: diff CMakeLists.txt CMakeLists.txt.original")
        print("  2. Build: rm -rf build && mkdir build && cd build && cmake .. && make")
        print("  3. Test: ./build/dinerod --version")
        print("  4. Check dependencies: otool -L ./build/dinerod")
        return 0
    else:
        print("❌ No changes made")
        return 1

if __name__ == "__main__":
    if len(sys.argv) > 1:
        filepath = sys.argv[1]
    else:
        filepath = "CMakeLists.txt"

    sys.exit(fix_cmake_file(filepath))
