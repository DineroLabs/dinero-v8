# Standalone Lightning Network Daemon (Phase 8.4: Hermetic Build)
#
# Phase 8.4: Hermetic L2 daemon build
# - NO L1 dependencies (daemon, chainstate, wallet, mempool)
# - NO proto generation required
# - NO gRPC linkage required
# - ONLY: IPC server + event sink + Lightning core
#
# Build independence:
#   cmake -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON .. && make lightningd

if(BUILD_LIGHTNINGD)
  if(DINERO_RELEASE)
    message(FATAL_ERROR
      "lightningd is not release-ready: mock chain/wallet/funding oracles, a placeholder node key, and incomplete reorg/justice handling remain. Build it only in non-release development configurations.")
  endif()
  message(STATUS "Building lightningd (standalone Lightning daemon)")

  # Phase 3 COMPLETE: Minimal lightningd - just link dinero_lightning!
  # NO complex source list needed - dinero_lightning library contains everything

  # Source files list (varies by build mode)
  set(LIGHTNINGD_SOURCES
    src/lightningd/main.cpp                # Main entry point (Phase 8.4: IPC Transport)
    src/lightningd/ipc_server.cpp          # Phase 8.4: Unix socket server for event injection
    src/common/logger.cpp                  # Logger (minimal dependency)

    # Phase 8.6: L2-pure Lightning core files (standalone Lightning in lightningd/)
    src/lightningd/lightning/lightning_app.cpp        # LightningApp (owns ChannelManagerCore)
    src/lightningd/lightning/channel_manager_core.cpp # L2 pure state machine
    src/lightningd/lightning/sqlite_lightning_db.cpp  # Persistence layer

    # Phase 9.3: Bidirectional oracle communication
    src/ipc/watch_registration_client.cpp # Watch registration client for Lightning→dinerod commands
  )

  # In release mode, add socket transport (no gRPC dependency)
  if(DINERO_RELEASE)
    list(APPEND LIGHTNINGD_SOURCES
      src/lightningd/lightning/socket_lightning_transport.cpp  # Socket transport (Bitcoin-style IPC)
    )
  endif()

  add_executable(lightningd ${LIGHTNINGD_SOURCES})

  target_include_directories(lightningd PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    # Phase 8.4: NO proto headers needed - lightningd is L2-only
  )

  target_link_libraries(lightningd PRIVATE
    # Phase 8.4: Hermetic L2 build - NO L1 dependencies
    # - NO dinero_lightning (has compilation errors, fix in Phase 8.5+)
    # - NO dinerod_proto (L1-only types)
    # - NO gRPC (L1 transport layer)
    # Only: IPC server + event sink (header-only) + system libs

    # Phase 8.6: L2-pure dependencies
    sqlite3          # SQLiteLightningDB persistence

    # System libraries
    pthread
    ${CMAKE_DL_LIBS}
  )

  # Phase 8.4: NO dinerod_proto linkage - lightningd is independent of L1 types

  # macOS-specific: Add Security framework
  if(APPLE)
    target_link_libraries(lightningd PRIVATE "-framework Security")
  endif()

  # Compiler definitions
  target_compile_definitions(lightningd PRIVATE
    DINERO_CLI_VERSION=${PROJECT_VERSION}
    DINERO_CLI_GIT_SHA=${GIT_HASH}
  )

  message(STATUS "✅ lightningd binary enabled (Phase 8.4: Hermetic Build)")
  message(STATUS "    Architecture: lightningd (IPC server + event sink)")
  message(STATUS "    ✅ NO L1 dependencies")
  message(STATUS "    ✅ NO proto generation")
  message(STATUS "    ✅ NO gRPC linkage")
  message(STATUS "    ✅ NO wallet/daemon code")
  message(STATUS "    ✅ Hermetic L2 build achieved")
else()
  message(STATUS "lightningd disabled (BUILD_LIGHTNINGD=OFF)")
endif()  # BUILD_LIGHTNINGD
