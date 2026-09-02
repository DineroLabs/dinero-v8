# NodeCore FFI target wiring.
#
# This file is included from the repository root CMakeLists.txt rather than
# added as a subdirectory because the legacy source list below intentionally
# uses root-relative paths.

# ─────────────────────────────────────────────────────────────────────────
# NodeCore FFI: Full embedded node for iOS (NodeCore.xcframework)
# ─────────────────────────────────────────────────────────────────────────
# C ABI wrapping DaemonApp lifecycle + status + wallet watch + events.
# This is the iOS equivalent of the dinerod executable, but as a library.
#
# Swift calls:
#   nodecore_start(datadir, config_json)
#   nodecore_set_event_callback(cb, ctx)
#   nodecore_watch_script(script, len, label)
#   nodecore_stop()
#
# Dependencies: same as dinerod minus gRPC/GUI/Stratum
# ─────────────────────────────────────────────────────────────────────────
option(BUILD_NODECORE "Build NodeCore FFI library for mobile embedding" OFF)

if(BUILD_NODECORE OR (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT BUILD_FFI_ONLY))
  message(STATUS "📱 Building NodeCore FFI (embedded full node for mobile)")

  set(NODECORE_LIBRARY_TYPE STATIC)
  if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(NODECORE_LIBRARY_TYPE SHARED)
  endif()

  add_library(nodecore_ffi ${NODECORE_LIBRARY_TYPE}
    src/nodecore/nodecore_ffi.cpp
    src/nodecore/sync_profile_policy.cpp
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>:src/nodecore/nodecore_jni_android.cpp>
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},iOS>:src/nodecore/zk_stubs_ios.cpp>
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>:src/nodecore/socket_wallet_stub_android.cpp>

    # DaemonApp lifecycle (same as dinerod, minus main.cpp)
    src/daemon/daemon_app.cpp
    src/daemon/legacy_globals_stub.cpp
    src/daemon/config.cpp
    src/daemon/rpc_context_wiring.cpp
    src/daemon/mempool.cpp
    src/daemon/db_repair.cpp

    # Read-only contract inspection RPC dependencies. Fund-moving handlers
    # remain unregistered, but contract.list/status must link on embedded nodes.
    src/contracts/contract_registry.cpp
    src/contracts/escrow_contract.cpp
    src/contracts/daemon_mediator.cpp

    # Socket wallet server (always enabled)
    ${SOCKET_SERVER_SOURCES}

    # RPC server infrastructure (internal, not HTTP-exposed on iOS)
    src/daemon/http_rpc_server.cpp
    src/daemon/rpc_auth.cpp

    # IPC oracles (for internal use)
    src/ipc/oracles/chain_oracle_client.cpp
    src/ipc/oracles/time_oracle_client.cpp
    src/ipc/oracles/transaction_oracle_client.cpp
    src/ipc/watch_registration_server.cpp

    # Bridge node (Utreexo proof generation)

    # Pool accounting (symbols referenced by daemon_app.cpp; gated by config at runtime)
  )

  target_include_directories(nodecore_ffi PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(nodecore_ffi PUBLIC
    dinero_core
    dinero_rpc_handlers
    argon2
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(nodecore_ffi)

  # Bulletproofs FFI (range proofs)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(nodecore_ffi PUBLIC bulletproofs_ffi)
  endif()

  target_compile_definitions(nodecore_ffi PRIVATE
    MOBILE_NODECORE_BUILD
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},iOS>:IOS_BUILD>
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>:ANDROID_NODECORE_BUILD>
    DISABLE_GRPC
    CMAKE_SOURCE_DIR="${DINERO_RUNTIME_SOURCE_DIR}"
    DINERO_CLI_VERSION=${PROJECT_VERSION}
    DINERO_CLI_GIT_SHA="nodecore"
    DINERO_GIT_COMMIT_FULL="nodecore-embedded"
    DINERO_CLI_BUILD_DATE="embedded"
  )

  set_target_properties(nodecore_ffi PROPERTIES
    PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/include/nodecore/nodecore_ffi.h"
  )

  # iOS-specific configuration
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(nodecore_ffi PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
      POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(nodecore_ffi PUBLIC
      "-framework Security"
      "-framework Foundation"
    )
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set_target_properties(nodecore_ffi PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
      POSITION_INDEPENDENT_CODE ON
    )
    target_link_libraries(nodecore_ffi PUBLIC log android)
  endif()

  message(STATUS "📱 NodeCore FFI target configured")
endif()
