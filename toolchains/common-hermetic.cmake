# ═══════════════════════════════════════════════════════════════════════════
# Common Hermetic Build Contract
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Universal build constraints that apply to ALL platforms
# Usage:   Included by platform-specific toolchain files (not invoked directly)
#
# Philosophy: "Define reality, remove ambiguity"
# ═══════════════════════════════════════════════════════════════════════════

# ─────────────────────────────────────────────────────────────────────────────
# Discovery Quarantine: Block Ambient Package Managers
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF CACHE BOOL "Ignore PATH" FORCE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF CACHE BOOL "Ignore system registry" FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "Ignore user registry" FORCE)
set(CMAKE_FIND_USE_INSTALL_PREFIX OFF CACHE BOOL "Ignore install prefix" FORCE)

# ─────────────────────────────────────────────────────────────────────────────
# Build Reproducibility
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Build PIC" FORCE)
set(CMAKE_SKIP_BUILD_RPATH ON CACHE BOOL "Skip build RPATH" FORCE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF CACHE BOOL "Skip install RPATH" FORCE)
set(CMAKE_COLOR_DIAGNOSTICS OFF CACHE BOOL "Disable color" FORCE)

# ─────────────────────────────────────────────────────────────────────────────
# Vendored Dependency Path Precedence
# ─────────────────────────────────────────────────────────────────────────────

if(CMAKE_SOURCE_DIR)
  list(PREPEND CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/third_party"
    "${CMAKE_SOURCE_DIR}/vendor"
  )
endif()

message(STATUS "🔒 Hermetic Contract: Common rules applied")
