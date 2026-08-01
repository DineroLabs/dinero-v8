# Dinero daemon executable.
#
# Keep dinerod target wiring close to the daemon sources. Shared libraries and
# cross-cutting tools still live in the top-level CMakeLists until later splits.

# ─────────────────────────────────────────────────────────────────────────
# Conditional gRPC Server Source Files (Dev Mode Only)
# ─────────────────────────────────────────────────────────────────────────
if(ENABLE_GRPC)
  set(DINEROD_GRPC_SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/grpc/grpc_server.cpp
    ${CMAKE_SOURCE_DIR}/src/grpc/blockchain_service.cpp
    ${CMAKE_SOURCE_DIR}/src/grpc/mempool_service.cpp
    ${CMAKE_SOURCE_DIR}/src/grpc/wallet_service.cpp
    ${CMAKE_SOURCE_DIR}/src/grpc/blockchain_client.cpp
    ${CMAKE_SOURCE_DIR}/src/grpc/mempool_client.cpp
  )
  message(STATUS "🔌 gRPC Server Sources: INCLUDED (dev mode)")
else()
  set(DINEROD_GRPC_SERVER_SOURCES "")
  message(STATUS "🔌 gRPC Server Sources: EXCLUDED (release mode)")
endif()

# Socket wallet server is included on POSIX (both dev and release modes).
# On Windows it's excluded — the implementation calls poll() directly with
# struct pollfd, which is POSIX-only (Winsock has WSAPoll with a slightly
# different surface). Lightning support isn't shipped on the Windows preview
# track, so the missing wallet socket server has no user-visible impact on
# the rc7+ Windows binaries. Re-enable this file by porting poll() to
# WSAPoll() if/when Lightning is ported to Windows.
if(WIN32)
  # Windows uses no-op stub implementations of the SocketWalletServer
  # methods so daemon_app.cpp's references resolve at link time. Lightning
  # support isn't shipped on the Windows preview track, so the stubs are
  # never actually exercised at runtime — Start() returns false and the
  # daemon proceeds without the socket wallet server.
  set(DINEROD_SOCKET_SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/grpc/socket_wallet_server_windows_stub.cpp
  )
else()
  set(DINEROD_SOCKET_SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/grpc/socket_wallet_server.cpp
  )
endif()

# ─────────────────────────────────────────────────────────────────────────
# Layer 4: Daemon Executable (Thin shell around libdinero_core)
# ─────────────────────────────────────────────────────────────────────────
if(APPLE)
  add_executable(dinerod
    # Daemon lifecycle and entry point
    ${CMAKE_SOURCE_DIR}/src/daemon/main.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/legacy_globals_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/daemon_app.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/datadir_guard.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp  # Phase 8: Node configuration
    ${CMAKE_SOURCE_DIR}/src/daemon/rpc_context_wiring.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp  # Phase G.3: Structured tx ingress
    ${CMAKE_SOURCE_DIR}/src/daemon/db_repair.cpp  # Database repair utility (--repair-db)
    ${CMAKE_SOURCE_DIR}/src/daemon/genesis_guard.cpp  # Stale chain data detection

    # Doctor diagnostic command (dinerod doctor)
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_command.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_context.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_json_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_checks_v1.cpp

    # gRPC and Socket services (conditional compilation)
    ${DINEROD_GRPC_SERVER_SOURCES}    # Only in dev mode when ENABLE_GRPC=ON
    ${DINEROD_SOCKET_SERVER_SOURCES}  # Always enabled (both dev and release)

    # RPC server infrastructure
    ${CMAKE_SOURCE_DIR}/src/daemon/http_rpc_server.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/rpc_auth.cpp

    # Stratum V1 mining server - REMOVED (separate binary: dinero-stratum)

    # Pool accounting system

    # Contract/Escrow support
    ${CMAKE_SOURCE_DIR}/src/contracts/escrow_contract.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/contract_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/daemon_mediator.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/contract_state_db.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/commitment_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/state_verifier.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/escrow_contract_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/contracts/lending_contract_manager.cpp
    # Phase 9.2: IPC oracle clients (Lightning event forwarding)
    ${CMAKE_SOURCE_DIR}/src/ipc/oracles/chain_oracle_client.cpp        # Chain event oracle for lightningd
    ${CMAKE_SOURCE_DIR}/src/ipc/oracles/time_oracle_client.cpp         # Time oracle for Lightning block height tracking
    ${CMAKE_SOURCE_DIR}/src/ipc/oracles/transaction_oracle_client.cpp  # Transaction oracle for Lightning TX tracking
    # Phase 9.3: Bidirectional oracle communication
    ${CMAKE_SOURCE_DIR}/src/ipc/watch_registration_server.cpp          # Watch registration server for Lightning→dinerod commands
    ${CMAKE_SOURCE_DIR}/src/contracts/dao_governance_manager.cpp
    # Phase P.2: Utreexo proof generation for stateless/mobile clients
  )
else()
  # Linux/Windows server build
  add_executable(dinerod
    # ─── dinerod-only sources (NOT in dinero_core) ───
    ${CMAKE_SOURCE_DIR}/src/daemon/main.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/legacy_globals_stub.cpp  # Temporary stubs for legacy globals
    # Windows stubs for bulletproofs FFI and hardware wallets (when Rust/cargo unavailable)
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/nodecore/zk_stubs_windows.cpp>
    ${CMAKE_SOURCE_DIR}/src/daemon/daemon_app.cpp           # DaemonApp lifecycle orchestrator
    ${CMAKE_SOURCE_DIR}/src/daemon/datadir_guard.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp               # Phase 8: Node configuration
    ${CMAKE_SOURCE_DIR}/src/daemon/rpc_context_wiring.cpp   # Week 2: Wire DaemonContext to RPC system
    ${CMAKE_SOURCE_DIR}/src/daemon/db_repair.cpp            # Database repair utility (--repair-db)
    ${CMAKE_SOURCE_DIR}/src/daemon/genesis_guard.cpp        # Stale chain data detection
    # Doctor diagnostic command (dinerod doctor)
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_command.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_context.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_json_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/doctor/doctor_checks_v1.cpp
    # Phase 9.3: Bidirectional oracle communication
    ${CMAKE_SOURCE_DIR}/src/ipc/watch_registration_server.cpp  # Watch registration server for Lightning→dinerod commands
    # Phase P.2: Utreexo proof generation for stateless/mobile clients
    # gRPC and Socket services (conditional compilation)
    ${DINEROD_GRPC_SERVER_SOURCES}    # Only in dev mode when ENABLE_GRPC=ON
    ${DINEROD_SOCKET_SERVER_SOURCES}  # Always enabled (both dev and release)
    # Pool accounting system (PPLNS, PPS, PROP payout modes) - not in dinero_core
    # Daemon-specific mempool (different from src/mempool/mempool.cpp in dinero_core)
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp
    # HTTP RPC server & auth (server-only, not in dinero_core)
    ${CMAKE_SOURCE_DIR}/src/daemon/http_rpc_server.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/rpc_auth.cpp
    # ─── All other sources come from dinero_core static library ───
    # hd_wallet_manager.cpp excluded (depends on wallet_crypto)
    # wallet_crypto.cpp excluded (needs OpenSSL 3.2+)
  )
  target_compile_definitions(dinerod PRIVATE NO_WALLET_ENCRYPTION)
endif()

set_target_properties(dinerod PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ═══════════════════════════════════════════════════════════════════════════
# Link All Layers: Proper dependency order
# ═══════════════════════════════════════════════════════════════════════════
# Order matters: More specific libraries first, then dependencies
#   1. dinero_chainstate  (RocksDB - depends on consensus)
#   2. dinero_wallet      (SQLite - user data)
#   3. dinero_consensus   (Pure rules - no DB)
#   4. dinero_rpc_handlers (RPC logic)
#   5. dinero_crypto      (Crypto primitives)
# ═══════════════════════════════════════════════════════════════════════════

if(APPLE)
  target_link_libraries(dinerod PRIVATE
    dinero_rpc_handlers    # RPC handlers
    dinero_core            # Core services/logic; must follow rpc_handlers for static resolution
    bulletproofs_ffi       # Dalek Bulletproofs via Rust FFI (range proofs)
    "-framework IOKit"      # Week 3: For mining_safety_gates battery/thermal checks
  )
  if(TARGET dinerod_proto)
    target_link_libraries(dinerod PRIVATE dinerod_proto) # gRPC API (dev mode only)
  endif()
else()
  target_link_libraries(dinerod PRIVATE
    dinero_rpc_handlers    # RPC handlers
    -Wl,--whole-archive dinero_core -Wl,--no-whole-archive  # whole-archive core after rpc_handlers resolves static refs in one pass
    bulletproofs_ffi       # Dalek Bulletproofs via Rust FFI (range proofs)
  )
  if(TARGET dinerod_proto)
    target_link_libraries(dinerod PRIVATE dinerod_proto) # gRPC API (dev mode only)
  endif()
endif()

# Windows-specific link libraries and linker flags
if(WIN32)
  # ntdll: Rust std in bulletproofs_ffi
  # ws2_32: Winsock2
  target_link_libraries(dinerod PRIVATE ntdll ws2_32)
  # MinGW's ld scans archives left-to-right and only pulls in unresolved
  # symbols from libraries it has already seen. RocksDB's env_win.cc /
  # port_win.cc reference PathIsDirectoryA / UuidCreateSequential / etc.,
  # which live in shlwapi.dll and rpcrt4.dll. The clean way to make those
  # libs land AFTER librocksdb.a in the link line is to add them as part
  # of RocksDB::rocksdb's INTERFACE_LINK_LIBRARIES — CMake then propagates
  # them to every consumer in the right order.
  if(TARGET RocksDB::rocksdb)
    set_property(TARGET RocksDB::rocksdb APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES shlwapi rpcrt4)
  endif()
  # Force linker to include ALL objects from dinero_core (resolves symbol
  # ordering issues with archive-static-init globals).
  #
  # MSVC's link.exe uses /WHOLEARCHIVE:<name>; MinGW-w64's ld.exe uses
  # -Wl,--whole-archive <archive> -Wl,--no-whole-archive. The CMake
  # target_link_libraries(... PRIVATE -Wl,--whole-archive libdinero_core.a
  # -Wl,--no-whole-archive ...) call elsewhere already handles MinGW; the
  # MSVC flag below is only correct under the MSVC toolchain. Without this
  # guard, MinGW's ld.exe interpreted "/WHOLEARCHIVE:dinero_core" as a file
  # path and bailed with "cannot find /WHOLEARCHIVE:dinero_core".
  if(MSVC)
    target_link_options(dinerod PRIVATE "/WHOLEARCHIVE:dinero_core")
  endif()
endif()

# Add generated headers include directory
target_include_directories(dinerod PRIVATE ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(dinerod PRIVATE
  pthread
  ${CMAKE_DL_LIBS}  # Required for Rust FFI (dlopen/dlsym)
)

# Ensure dinerod depends on Bulletproofs FFI build (if cargo is available)
if(CARGO_EXECUTABLE)
  add_dependencies(dinerod build_bulletproofs_ffi)
endif()
# Ensure production code path remains on canonical daemon mempool.
add_dependencies(dinerod
  check_no_legacy_mempool_prod
  check_no_deprecated_openssl_sha256
)

# Windows: sockets + CSPRNG
if(WIN32)
  # NOMINMAX and WIN32_LEAN_AND_MEAN are set globally (line 65)
  target_link_libraries(dinerod PRIVATE ws2_32 bcrypt)
endif()

# Add version compile definitions (PROJECT_VERSION already has quotes from line 131)
target_compile_definitions(dinerod PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="${GIT_REMOTE_SLUG}"
  DINERO_BUILD_DESCRIBE="${GIT_DESCRIBE}"
  DINERO_BUILD_COMPONENT="dinerod"
  CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"  # SQL-first schema path (Phase 2 - Nov 2025)
)

# macOS: Security and CoreFoundation frameworks
if(APPLE)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(SECURITY_FRAMEWORK)
    target_link_libraries(dinerod PRIVATE ${SECURITY_FRAMEWORK})
  endif()
  if(COREFOUNDATION_FRAMEWORK)
    target_link_libraries(dinerod PRIVATE ${COREFOUNDATION_FRAMEWORK})
  endif()
endif()

# Sanitizers for debug builds
if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  dinero_enable_sanitizers(dinerod)
endif()
