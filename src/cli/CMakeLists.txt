# dinero-cli product CLI target.
#
# This file is included from the repository root so paths remain root-relative
# and root-scoped version metadata stays available.

add_executable(dinero-cli src/dinero_cli.cpp)
target_link_libraries(dinero-cli PRIVATE
  dinero_rpc_client
  jsoncpp_static
)

# dinero_rpc_client (used by dinero-cli) calls Winsock2 directly:
# WSAStartup, socket(), connect(), send(), recv(), gethostbyname(),
# closesocket(), inet_pton(), htons(). MinGW does NOT auto-link ws2_32,
# so we must add it explicitly. Without this dinero-cli fails to link
# with "undefined reference to `__imp_WSAStartup'" + friends. dinerod's
# link line gets ws2_32 transitively through other targets, but
# dinero-cli only pulls in dinero_rpc_client + jsoncpp_static so we
# add it here.
if(WIN32)
  target_link_libraries(dinero-cli PRIVATE ws2_32)
endif()

target_include_directories(dinero-cli PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Phase D.4 / E.3 — version + bundled-libs metadata.
#
# `dinero-cli version --json` emits a contract schema that
# release-time gates (PackageGate) and external monitoring tools
# can pin against. The data is purely compile-time — build
# version, git sha, build date, plus the vendored-static
# dependency versions. No runtime daemon required (so the gate
# can run in --static mode against staging too).
#
# Versions are sourced from the vendored submodule paths /
# CMakeLists.txt comments. Update here when a vendored dep
# bumps. Static-linked deps report no `sha256` per spec §1.4
# nuance (no separate .so artifact to hash).
# DINERO_PACKAGE_VERSION used to maintain its own version string,
# stale at 2.2.5 against the v2.2.6 release line — a real instance
# of the version drift that motivated the monorepo consolidation.
# After consolidation, this tracks CMAKE_PROJECT_VERSION (which is
# the top-level project's VERSION, unquoted — set by `project()`
# at line 2). Overridable via -DDINERO_PACKAGE_VERSION=... for
# packagers that need to pin a different string.
if(NOT DEFINED DINERO_PACKAGE_VERSION)
  set(DINERO_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
endif()

set(DINERO_CLI_OPENSSL_VERSION "${DINERO_VENDORED_OPENSSL_VERSION}")
if(USE_SYSTEM_OPENSSL)
  set(DINERO_CLI_OPENSSL_VERSION "${OPENSSL_VERSION}")
endif()
if(DINERO_CLI_OPENSSL_VERSION STREQUAL "")
  set(DINERO_CLI_OPENSSL_VERSION "unknown")
endif()

target_compile_definitions(dinero-cli PRIVATE
  DINERO_PACKAGE_VERSION=\"${DINERO_PACKAGE_VERSION}\"
  # GIT_HASH and BUILD_TIME are already pre-quoted in CMake (see
  # the `set(GIT_HASH "\"${GIT_HASH}\"")` block earlier in this
  # file) so they expand to a complete C string literal without
  # additional escaping here.
  DINERO_BUILD_GIT_SHA=${GIT_HASH}
  DINERO_BUILD_DATE_ISO=${BUILD_TIME}
  DINERO_BUNDLED_OPENSSL_VERSION=\"${DINERO_CLI_OPENSSL_VERSION}\"
  DINERO_BUNDLED_ROCKSDB_VERSION=\"8.11.3\"
  DINERO_BUNDLED_SECP256K1_ZKP_VERSION=\"vendored\"
  DINERO_BUNDLED_JSONCPP_VERSION=\"vendored\"
  DINERO_BUNDLED_ZSTD_VERSION=\"1.4.7+vendored\"
)

dinero_attach_win32_versioninfo(dinero-cli "Dinero RPC CLI client")
