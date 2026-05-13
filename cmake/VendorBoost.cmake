# ===== Boost Dependency Management =====
# Switches between vendored (submodule) and system Boost
# based on DINERO_USE_VENDORED_DEPS option

if(DINERO_USE_VENDORED_DEPS)
  message(STATUS "Using vendored Boost from third_party/boost_1_85_0")

  # ═══════════════════════════════════════════════════════════════════════════
  # Hermetic Boost Configuration (Header-Only)
  # ═══════════════════════════════════════════════════════════════════════════
  # Boost 1.85.0 is fully vendored in third_party/boost_1_85_0/
  # DineroCoin uses header-only Boost libraries:
  #   - Boost.Asio (P2P networking, async I/O)
  #   - Boost.Beast (HTTP/WebSocket for RPC)
  #   - Boost.Endian (byte order conversions)
  #
  # No compiled Boost libraries needed - all header-only
  # This avoids ABI compatibility issues across platforms
  # ═══════════════════════════════════════════════════════════════════════════

  set(BOOST_ROOT "${CMAKE_SOURCE_DIR}/third_party/boost_1_85_0" CACHE PATH "Vendored Boost root" FORCE)
  set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "Prevent system Boost discovery" FORCE)
  set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "Disable BoostConfig.cmake" FORCE)

  # Verify vendored Boost exists
  if(NOT EXISTS "${BOOST_ROOT}/boost/version.hpp")
    message(FATAL_ERROR
      "Vendored Boost not found at ${BOOST_ROOT}\n"
      "Expected: third_party/boost_1_85_0/boost/version.hpp\n"
      "Run: git submodule update --init --recursive")
  endif()

  # ═══════════════════════════════════════════════════════════════════════════
  # Create Boost::headers INTERFACE Target (Modern CMake Pattern)
  # ═══════════════════════════════════════════════════════════════════════════
  # This replaces legacy find_package(Boost) + include_directories(${Boost_INCLUDE_DIRS})
  #
  # Benefits:
  #   1. Target-scoped includes (no global pollution)
  #   2. Transitive dependencies work correctly
  #   3. Compatible with hermetic toolchain system
  #   4. No SYSTEM includes (high-priority -I flags)
  # ═══════════════════════════════════════════════════════════════════════════

  if(NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED GLOBAL)

    # Set include directory (NOT SYSTEM - we want high-priority -I flags)
    set_target_properties(Boost::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${BOOST_ROOT}"
    )

    # Boost compile definitions (if needed)
    # set_target_properties(Boost::headers PROPERTIES
    #   INTERFACE_COMPILE_DEFINITIONS "BOOST_ALL_NO_LIB"  # Header-only mode
    # )
  endif()

  # Create compatibility aliases for common Boost components
  # These allow target_link_libraries(... Boost::asio) to work
  foreach(_component asio beast system)
    if(NOT TARGET Boost::${_component})
      add_library(Boost::${_component} INTERFACE IMPORTED GLOBAL)
      set_target_properties(Boost::${_component} PROPERTIES
        INTERFACE_LINK_LIBRARIES Boost::headers
      )
    endif()
  endforeach()

  message(STATUS "✅ Boost: Using vendored build (v1.85.0, header-only)")
  message(STATUS "   Components: Asio, Beast, Endian")
  message(STATUS "   Root: ${BOOST_ROOT}")

else()
  # System Boost mode (for local development only)
  message(STATUS "Using system Boost via find_package")

  find_package(Boost 1.70 REQUIRED)

  if(Boost_FOUND)
    message(STATUS "✅ Boost: Using system installation (${Boost_VERSION})")
  else()
    message(FATAL_ERROR "System Boost not found. Set DINERO_USE_VENDORED_DEPS=ON for hermetic build.")
  endif()
endif()
