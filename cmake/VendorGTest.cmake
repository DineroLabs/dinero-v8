# ===== GoogleTest Dependency Management =====
# Switches between vendored (submodule) and system GoogleTest
# based on DINERO_USE_VENDORED_DEPS option

if(DINERO_USE_VENDORED_DEPS)
  message(STATUS "Using vendored GoogleTest from third_party/googletest")

  # ═══════════════════════════════════════════════════════════════════════════
  # Layer 2: Hard Quarantine Homebrew (macOS ARM64)
  # ═══════════════════════════════════════════════════════════════════════════
  # Remove /opt/homebrew from CMake's search space entirely
  # This is not "prefer vendored" - this is "Homebrew does not exist"
  # Without this, Homebrew headers leak via transitive dependencies (Boost, etc)
  # ═══════════════════════════════════════════════════════════════════════════
  list(FILTER CMAKE_SYSTEM_INCLUDE_PATH EXCLUDE REGEX "/opt/homebrew")
  list(FILTER CMAKE_SYSTEM_LIBRARY_PATH EXCLUDE REGEX "/opt/homebrew")
  list(FILTER CMAKE_PREFIX_PATH        EXCLUDE REGEX "/opt/homebrew")
  list(FILTER CMAKE_FRAMEWORK_PATH     EXCLUDE REGEX "/opt/homebrew")

  message(STATUS "🔒 Homebrew Quarantine: /opt/homebrew removed from CMake search paths")

  # Explicitly prevent find_package(GTest) from finding system GoogleTest
  # This blocks /opt/homebrew/include/gtest from entering the dependency graph
  set(GTEST_DISABLE_FIND_PACKAGE ON CACHE BOOL "Disable find_package(GTest)" FORCE)

  # Check if submodule is initialized
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    message(FATAL_ERROR "GoogleTest submodule not initialized. Run: git submodule update --init --recursive")
  endif()

  # Configure GoogleTest build options
  set(BUILD_GMOCK ON CACHE BOOL "Build GoogleMock" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "Use shared CRT" FORCE)

  # Add GoogleTest as subdirectory (exclude from ALL)
  add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/googletest EXCLUDE_FROM_ALL)

  # ═══════════════════════════════════════════════════════════════════════════
  # Layer 1: Upgrade GoogleTest includes from SYSTEM to NORMAL (Critical Fix)
  # ═══════════════════════════════════════════════════════════════════════════
  # Problem: GoogleTest declares SYSTEM INTERFACE includes (googletest/CMakeLists.txt:144-147)
  #          SYSTEM → -isystem (low priority) loses to Homebrew's -I (high priority)
  #          Result: System headers + vendored library = ABI mismatch
  #
  # Solution: Preserve GoogleTest's paths, upgrade priority level
  #          1. Extract existing include paths
  #          2. Clear SYSTEM designation
  #          3. Re-add as NORMAL includes (-I instead of -isystem)
  #
  # This is the canonical hermetic build pattern - no source patching required
  # ═══════════════════════════════════════════════════════════════════════════

  # Extract GoogleTest's SYSTEM include directories (that's where they are)
  get_target_property(_gtest_sys_inc gtest INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
  get_target_property(_gtest_main_sys_inc gtest_main INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
  get_target_property(_gmock_sys_inc gmock INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

  # Clear SYSTEM designation (this removes -isystem behavior)
  set_target_properties(gtest PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
  set_target_properties(gtest_main PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
  set_target_properties(gmock PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

  # Re-add as NORMAL INTERFACE includes (creates -I instead of -isystem)
  if(_gtest_sys_inc)
    target_include_directories(gtest INTERFACE ${_gtest_sys_inc})
  endif()
  if(_gtest_main_sys_inc)
    target_include_directories(gtest_main INTERFACE ${_gtest_main_sys_inc})
  endif()
  if(_gmock_sys_inc)
    target_include_directories(gmock INTERFACE ${_gmock_sys_inc})
  endif()

  # ═══════════════════════════════════════════════════════════════════════════
  # Layer 3: Global Include Precedence (Defense Against Legacy Patterns)
  # ═══════════════════════════════════════════════════════════════════════════
  # Problem: Boost uses include_directories(${Boost_INCLUDE_DIRS}) globally
  #          Global includes apply to ALL targets and precede target-level includes
  #          Result: /opt/homebrew/include (from Boost) comes before GoogleTest
  #
  # Solution: Also add GoogleTest globally with BEFORE keyword
  #          This fights fire with fire - global BEFORE beats regular global
  # ═══════════════════════════════════════════════════════════════════════════
  include_directories(BEFORE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googlemock/include
  )

  # Create alias targets for consistency (if not already created)
  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest ALIAS gtest)
  endif()
  if(NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
  endif()
  if(NOT TARGET GTest::gmock)
    add_library(GTest::gmock ALIAS gmock)
  endif()

  message(STATUS "GoogleTest: Using vendored build (v1.14.0)")

else()
  message(STATUS "Using system GoogleTest via find_package")

  # Use standard FindGTest module
  find_package(GTest REQUIRED)

  message(STATUS "GoogleTest: Using system installation")
endif()
