cmake_minimum_required(VERSION 3.20)
# Project name is lowercase `dinero` to match the target monorepo
# repository name (DineroLabs/dinero). VERSION 8.0.0 is the first
# unified release line after the monorepo consolidation; daemon,
# CLI, Qt GUI, and solo miner all derive their version from this
# declaration (sub-project project() calls in qt/ and miner/ also
# pin to 8.0.0, see the comments there).
project(dinero VERSION 8.0.0 LANGUAGES C CXX)

# Enable Objective-C++ at the root scope on Apple platforms so that
# qt/ (which has .mm sources via Qt6 + macOS native frameworks like
# CoreImage / AppKit) can rely on CMAKE_OBJCXX_COMPILE_OBJECT being
# set during the generate phase. Pre-consolidation qt/ enabled this
# language itself in standalone mode; in add_subdirectory mode CMake
# needs OBJCXX known at the top-level project scope to wire
# generate-time compile rules.
if(APPLE)
  enable_language(OBJCXX)
endif()

# Export compile_commands.json for clangd and related tooling.
# Harmless for generators that do not use it, and helpful for editor parity.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ═══════════════════════════════════════════════════════════════════════════
# Prevent In-Source Builds
# ═══════════════════════════════════════════════════════════════════════════
# In-source builds pollute the source tree and can cause issues with git.
# Always use out-of-source builds: cmake -S . -B build && cd build && make
# ═══════════════════════════════════════════════════════════════════════════
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "\n"
    "❌ FATAL: In-source builds are not allowed!\n"
    "\n"
    "You are attempting to build in the source directory:\n"
    "  ${CMAKE_SOURCE_DIR}\n"
    "\n"
    "This would pollute the source tree with build artifacts.\n"
    "\n"
    "Please use an out-of-source build:\n"
    "  rm -f CMakeCache.txt\n"
    "  rm -rf CMakeFiles/\n"
    "  cmake -S . -B build\n"
    "  cd build\n"
    "  make\n"
    "\n"
    "Or use the hermetic build script:\n"
    "  ./scripts/hermetic-build.sh <target>\n"
    "\n"
  )
endif()

# Guardrail: production code must not depend on legacy mempool::Mempool path.
add_custom_target(check_no_legacy_mempool_prod
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check_no_legacy_mempool_prod.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Checking production tree for legacy mempool references"
)

# ═══════════════════════════════════════════════════════════════════════════
# Hermetic Build Detection
# ═══════════════════════════════════════════════════════════════════════════
# DEV MODE (default): Fast iteration, normal cmake/make
# HERMETIC MODE: Deterministic validation, use ./scripts/hermetic-build.sh
#
# Hermetic builds are RECOMMENDED for releases but not required for development.
# ═══════════════════════════════════════════════════════════════════════════

if(NOT DEFINED ENV{SOURCE_DATE_EPOCH})
  message(WARNING
    "\n"
    "⚠️  DEV MODE: Not a hermetic build\n"
    "   For release validation, use: ./scripts/hermetic-build.sh <target>\n"
  )
  set(HERMETIC_BUILD OFF)
else()
  message(STATUS "🔒 Hermetic build: SOURCE_DATE_EPOCH = $ENV{SOURCE_DATE_EPOCH}")
  set(HERMETIC_BUILD ON)
endif()

# Add cmake modules directory to module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Modern CMake policies. CMP0144 (upper-case <PACKAGENAME>_ROOT variables)
# was added in CMake 3.27; Ubuntu 22.04 ships 3.22, so guard the SET behind
# a POLICY-exists check or older systems hard-fail at configure time.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)  # Use upper-case <PACKAGENAME>_ROOT variables
endif()

# C++ standard (C++17 baseline for std::optional, std::string_view, std::variant)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows compatibility — must be before any target definitions
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  # secp256k1.h declares functions with __declspec(dllimport) on Windows
  # unless SECP256K1_STATIC is defined. We link against the static
  # libsecp256k1.a built in third_party/secp256k1-zkp; without this define,
  # consumers (dinero_crypto, etc.) emit `__imp_secp256k1_*` references
  # that the static archive can't resolve. See third_party/secp256k1-zkp/
  # include/secp256k1.h around the SECP256K1_API macro definition.
  add_compile_definitions(SECP256K1_STATIC)
  # Windows CRT includes threading — no separate pthread library needed.
  # Create an empty interface target so all target_link_libraries(... pthread) calls resolve.
  add_library(pthread INTERFACE)

  # Enable the .rc compiler at root scope so any target (daemon binaries
  # here, dinero-qt under qt/) can attach a VS_VERSION_INFO resource via
  # dinero_attach_win32_versioninfo() below.
  enable_language(RC)
endif()

# Release tag for Windows file-properties / About dialog metadata.
# PROJECT_VERSION carries semver only ("8.0.0"); rc/preview suffixes live
# in this variable. Override per cut at configure time:
#   cmake -S . -B build -DDINERO_RELEASE_TAG=8.0.0-rc2 ...
# Defaults to PROJECT_VERSION so unflagged dev builds never embed an
# empty string. Promoted to CACHE here so qt/ and daemon targets see the
# same value.
if(NOT DEFINED DINERO_RELEASE_TAG OR "${DINERO_RELEASE_TAG}" STREQUAL "")
  set(DINERO_RELEASE_TAG "${PROJECT_VERSION}"
      CACHE STRING "User-visible release tag (e.g. 8.0.0-rc2)")
endif()

# Helper: attach a VS_VERSION_INFO resource to a Windows executable so
# Explorer Properties -> Details shows version/company/component fields.
# No-op on non-Windows. Pass a human description for the FileDescription
# field (shown in Task Manager and right-click Properties).
function(dinero_attach_win32_versioninfo target description)
  if(NOT WIN32)
    return()
  endif()
  # Respect set_target_properties(... OUTPUT_NAME ...) so InternalName and
  # OriginalFilename match the actual .exe on disk (e.g. dinero-solo-miner-cli
  # ships as dinero-solo-miner.exe).
  get_target_property(_outname ${target} OUTPUT_NAME)
  if(NOT _outname OR "${_outname}" STREQUAL "_outname-NOTFOUND")
    set(_outname "${target}")
  endif()
  set(DINERO_VI_FILEVERSION
      "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
  set(DINERO_VI_PRODUCTVERSION "${DINERO_VI_FILEVERSION}")
  set(DINERO_VI_VERSION_STRING "${DINERO_RELEASE_TAG}")
  set(DINERO_VI_DESCRIPTION    "${description}")
  set(DINERO_VI_INTERNAL_NAME  "${_outname}")
  set(DINERO_VI_ORIGINAL_FILENAME "${_outname}.exe")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${target}_version_info.rc")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/dinero_version_info.rc.in"
    "${_gen}"
    @ONLY
  )
  target_sources(${target} PRIVATE "${_gen}")
endfunction()
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options and mode switches live in cmake/.
include(cmake/BuildOptions.cmake)

# Platform defaults, global include policy, and build timestamp setup.
include(cmake/PlatformConfig.cmake)

find_package(Threads REQUIRED)

# Optional UPnP/NAT-PMP P2P port mapping dependency target.
include(cmake/PortMapping.cmake)

# Git/build provenance and generated version_config.h.
include(cmake/VersionMetadata.cmake)

# Fixed seed header regeneration target.
include(cmake/SeedGeneration.cmake)

# Third-party dependency setup lives in cmake/ThirdParty.cmake.
# Keep this as include() so helper functions and imported targets remain root-scoped.
include(cmake/ThirdParty.cmake)

# V7 PQ façade + bench + round-trip tests. Requires OpenSSL::Crypto
# (pq_derivation.cpp uses HMAC-SHA256). Works with BOTH system and
# vendored OpenSSL — moved here from the vendored-only branch so Linux
# servers using USE_SYSTEM_OPENSSL=ON can build dinero_pq.
add_subdirectory(src/consensus/pq EXCLUDE_FROM_ALL)   # dinero_pq lib
add_subdirectory(tools/pq_bench   EXCLUDE_FROM_ALL)   # bench + tests
add_subdirectory(src/consensus/shielded EXCLUDE_FROM_ALL)  # dinero_shielded: commitment tree, nullifiers, validation

# Hardware wallet mock signing guard.
include(cmake/HardwareWalletMock.cmake)

# gRPC/protobuf generation and optional daemon transport support.
include(cmake/GrpcProtobuf.cmake)

# Lightning Network Core (unified dependency module)
# Provides: libwally-core, secp256k1-zkp, blake3
# See: LIGHTNING_VENDOR_QUICKSTART.md for installation
add_subdirectory(third_party/lightning_core)

# NOTE: secp256k1 is now built directly from third_party/secp256k1
# No alias needed - the real library target is available as 'secp256k1'

# ═══════════════════════════════════════════════════════════════════════════
# BULLETPROOFS FFI (Dalek Bulletproofs via Rust FFI)
# ═══════════════════════════════════════════════════════════════════════════

# Bulletproofs FFI Library - Rust implementation of Dalek Bulletproofs
# This provides production-grade range proofs for shielded proof support
set(BP_FFI_DIR "${CMAKE_SOURCE_DIR}/third_party/bulletproofs_ffi")

# Determine library extension based on platform
if(APPLE)
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/libbulletproofs_ffi.a")
elseif(WIN32)
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/bulletproofs_ffi.lib")
else()
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/libbulletproofs_ffi.a")
endif()

# Check if cargo is available (Rust toolchain)
# Search in standard cargo install locations (portable across all users)
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")

if(CARGO_EXECUTABLE)
    set(BP_FFI_BUILD_COMMAND "${CARGO_EXECUTABLE}" build --release)

    if(APPLE)
        set(BP_RUSTFLAGS "$ENV{RUSTFLAGS}")
        if(BP_RUSTFLAGS)
            set(BP_RUSTFLAGS "${BP_RUSTFLAGS} -C link-arg=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_RUSTFLAGS "-C link-arg=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_CFLAGS "$ENV{CFLAGS}")
        if(BP_CFLAGS)
            set(BP_CFLAGS "${BP_CFLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_CFLAGS "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_LDFLAGS "$ENV{LDFLAGS}")
        if(BP_LDFLAGS)
            set(BP_LDFLAGS "${BP_LDFLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_LDFLAGS "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_FFI_BUILD_COMMAND
            ${CMAKE_COMMAND} -E env
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "RUSTFLAGS=${BP_RUSTFLAGS}"
            "CFLAGS=${BP_CFLAGS}"
            "LDFLAGS=${BP_LDFLAGS}"
            "${CARGO_EXECUTABLE}" build --release
        )

        message(STATUS "   Bulletproofs FFI macOS target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()

    # Add build target for Rust library
    add_custom_target(build_bulletproofs_ffi
        COMMAND ${BP_FFI_BUILD_COMMAND}
        WORKING_DIRECTORY "${BP_FFI_DIR}"
        COMMENT "Building Bulletproofs FFI library (Rust)..."
        BYPRODUCTS "${BP_FFI_LIB}"
    )

    # Import as static library
    add_library(bulletproofs_ffi STATIC IMPORTED)
    set_target_properties(bulletproofs_ffi PROPERTIES
        IMPORTED_LOCATION "${BP_FFI_LIB}"
    )

    # Ensure the Rust library is built before dinerod
    add_dependencies(bulletproofs_ffi build_bulletproofs_ffi)

    message(STATUS "✅ Bulletproofs FFI: ENABLED (Dalek via Rust)")
    message(STATUS "   Library: ${BP_FFI_LIB}")
else()
    message(WARNING "⚠️  Bulletproofs FFI: DISABLED (cargo not found)")
    message(WARNING "   To enable: Install Rust from https://rustup.rs/")
    message(WARNING "   Then run: cd third_party/bulletproofs_ffi && cargo build --release")

    # Create a dummy target so build doesn't fail
    add_library(bulletproofs_ffi INTERFACE)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# ZERO-KNOWLEDGE PRIVACY (Confidential Transactions)
# ═══════════════════════════════════════════════════════════════════════════

option(ENABLE_ZK "Enable zero-knowledge proof features" ON)

if(ENABLE_ZK)
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "Zero-Knowledge Privacy: ENABLED")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

    # ZK privacy library (Pedersen commitments + Bulletproofs + Ring Signatures)
    add_library(dinero_zk STATIC
        src/zk/zkvm/scalar.cpp
        src/zk/zkvm/secp256k1_msm_cpu.cpp
        src/zk/zkvm/r1cs.cpp
        src/zk/zkvm/ipa.cpp
        src/zk/zkvm/hyrax.cpp
        src/zk/zkvm/nova.cpp
        src/zk/zkvm/gadgets.cpp
        src/zk/zkvm/tapscript_step.cpp
        src/zk/zkvm/r1cs_ipa.cpp
        src/zk/zkvm/r1cs_spartan.cpp
        src/zk/zkvm/poseidon_gadget.cpp
        src/zk/zkvm/sha256_gadget.cpp
        src/zk/zkvm/secp256k1_fe_gadget.cpp
        src/zk/zkvm/ec_gadget.cpp
        src/zk/zkvm/schnorr_gadget.cpp
        src/zk/zkvm/taproot_path_circuit.cpp
    )

    target_include_directories(dinero_zk PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
    )

    target_link_libraries(dinero_zk PUBLIC
        secp256k1_zkp          # ElementsProject/secp256k1-zkp (Pedersen + Bulletproofs)
        lightning_core_static  # Uses secp256k1-zkp for Pedersen + Bulletproofs!
        OpenSSL::Crypto        # For secure random (RAND_bytes)
    )

    target_compile_definitions(dinero_zk PUBLIC
        HAVE_ZK_PRIVACY
        HAVE_CONFIDENTIAL_TX
    )

    message(STATUS "  ✅ secp256k1-zkp      (Pedersen commitments + Bulletproofs)")
    message(STATUS "  ✅ libwally-core      (PSBT wrapper for proofs)")
    message(STATUS "  ✅ OpenSSL            (Secure random generation)")
    message(STATUS "")
    message(STATUS "  🔒 Features:")
    message(STATUS "     - Confidential amounts (Pedersen commitments)")
    message(STATUS "     - Range proofs (Bulletproofs)")
    message(STATUS "     - Hidden balances (~5KB overhead per output)")
    message(STATUS "")
endif()

# GPU mining target wiring.
include(src/mining/gpu/CMakeLists.txt)

# Create version macros for main.cpp
set(PROJECT_VERSION "\"${PROJECT_VERSION}\"")
set(GIT_HASH "\"${GIT_HASH}\"")
set(BUILD_TIME "\"${BUILD_TIME}\"")

# Core crypto library (minimal, vendored crypto)
add_library(dinero_crypto STATIC
  src/crypto/sha256.cpp
  src/crypto/ripemd160_standalone.cpp
  # NOTE: src/crypto/params_stub.cpp intentionally NOT compiled here.
  # The stub provides a fallback dinero::detail::ParamsImpl() for
  # hypothetical lightweight builds that link dinero_crypto without
  # dinero_consensus. Compiling it into dinero_crypto unconditionally
  # caused LNK2005 multiply-defined ParamsImpl on MSVC whenever
  # dinero_consensus was also in the link line and CMake's transitive
  # expansion ordered dinero_crypto.lib before dinero_consensus.lib --
  # because the WEAK_SYMBOL macro expands to nothing on MSVC and there
  # is no portable C++ weak-function mechanism. No current target needs
  # the stub: every Params() caller (hd_wallet.cpp) lives in
  # dinero_wallet, which PUBLIC-deps dinero_consensus and therefore
  # gets the real chainparams_impl.cpp definition. If a future
  # lightweight HDWallet target ever wants Params() without
  # dinero_consensus, it should add src/crypto/params_stub.cpp to
  # its OWN sources -- not re-introduce it here.
  src/crypto/bip39.cpp
  src/crypto/bip32_slip132.cpp
  src/crypto/hd_keychain.cpp
  src/crypto/extended_pubkey.cpp
  src/crypto/base58.cpp
  src/crypto/hash.cpp
  src/common/sha256d.cpp              # Dinero::Common::sha256 (used by hash.cpp)
  src/core/crypto/ripemd160.cpp
  src/core/crypto/dinero_crypto_minimal.cpp
  external/bech32/bech32.cpp
  src/crypto/sha256_simd.cpp
  src/crypto/sha256_neon.cpp
  src/crypto/sha256_arm_shani.cpp
  src/crypto/sha256_arm_shani_wrapper.cpp
  src/crypto/encrypted_key.cpp  # Phase W.1.1: Encrypted key utilities (ParseBase64, SecureZero)
  src/crypto/decrypt_encrypted_key.cpp  # Phase W.1.1: Encrypted key decryption
)

# Enable ARM Crypto Extensions on ARM64 platforms
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    if(APPLE)
        target_compile_options(dinero_crypto PRIVATE -march=armv8-a+crypto)
        target_compile_definitions(dinero_crypto PRIVATE ENABLE_ARM_SHANI)
        message(STATUS "  ⚡ ARM SHA Extensions enabled (hardware acceleration)")
    endif()
endif()

target_include_directories(dinero_crypto PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/external
)

# ═══════════════════════════════════════════════════════════════════════════
# Transaction Primitives Library (Phase 2: Lightning Wallet Detachment)
# ═══════════════════════════════════════════════════════════════════════════
# Pure stateless transaction and cryptographic primitives.
# Shared by both dinerod (wallet) and lightningd (off-chain).
# NO wallet state, NO database access, NO coin selection.
add_library(dinero_tx_primitives STATIC
  src/primitives/transaction.cpp       # Transaction serialization, txid/wtxid
  src/primitives/schnorr_signer.cpp    # BIP-340 Schnorr signatures
  src/primitives/taproot_tx_signer.cpp # BIP-341 Taproot signing
  src/primitives/hash_domains.cpp      # Phase M.3: Semantic hash domains
)

target_include_directories(dinero_tx_primitives PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include/wallet  # For transaction.h
)

target_link_libraries(dinero_tx_primitives PUBLIC
  dinero_crypto          # For SHA256 hashing
  secp256k1              # For Schnorr/Taproot crypto
  OpenSSL::Crypto        # For randomness
)

add_subdirectory(src/wallet)

# Lightning Key Derivation (always built - used by wallet to provide keys to Lightning)
# This is separate from the Lightning daemon and from wallet UTXOs.
# Keys derived here are PROTOCOL KEYS, not on-chain wallet keys.
add_library(dinero_lightning_keys STATIC
  src/lightning/keys/lightning_key_deriver.cpp
)
target_include_directories(dinero_lightning_keys PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/vendor/include
)
target_link_libraries(dinero_lightning_keys PUBLIC
  dinero_crypto      # BIP32Deriver
  secp256k1          # Public key derivation
  OpenSSL::Crypto    # HMAC, OPENSSL_cleanse
)

# Lightning Network library (Layer 3, off-chain - NOT required for consensus)
# Disabled during genesis to ensure clean, deterministic chain birth
if(ENABLE_LIGHTNING)
  add_library(dinero_lightning STATIC
    src/lightning/lightning_crypto.cpp
    src/lightning/crypto_utils.cpp
    src/lightning/secp256k1_utils.cpp
    # src/lightning/lightning_wallet.cpp  # Phase 3: Disabled - dead code, predates gRPC architecture
    src/lightning/channel_manager.cpp
    src/lightning/channel_manager_core.cpp        # Phase 7: L2 pure state machine
    src/lightning/channel_utxo_lock_manager.cpp   # Phase 35: UTXO locking (Bitcoin+Lightning safety)
    src/lightning/production_chain_oracle.cpp     # L1→L2 chain queries
    src/lightning/production_wallet_oracle.cpp    # L1→L2 wallet queries
    src/lightning/production_funding_service.cpp  # L1→L2 funding TX
    src/lightning/production_htlc_sweep_oracle.cpp # Phase 7B: HTLC sweep TX
    src/lightning/production_justice_oracle.cpp    # Phase 7C: Justice TX
    src/lightning/htlc_manager.cpp
    # src/lightning/lightning_db.cpp  # Removed: legacy implementation deleted
    src/lightning/sqlite_lightning_db.cpp
    src/lightning/invoice.cpp
    src/lightning/onion.cpp
    src/lightning/payment_utils.cpp
    # Phase 31: Multi-Asset Lightning Settlement Layer
    src/lightning/asset_channel.cpp
    src/lightning/asset_htlc.cpp
    src/lightning/swap_htlc.cpp
    src/lightning/routing_multiasset.cpp
    # Release Build: Socket-based Lightning IPC (zero dependencies)
    src/lightning/socket_lightning_transport.cpp
  )
  target_include_directories(dinero_lightning PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/build/generated/proto  # Phase 3 Commit 1: Proto headers for dinerod::UTXO
  )
  target_link_libraries(dinero_lightning PUBLIC
    # Phase 3 Commit 7: dinero_wallet removed - Lightning uses IWalletAPI interface only ✅
    # Lightning accesses wallet via gRPC (WalletClient → WalletServiceImpl → WalletManager)
    dinero_crypto
    secp256k1
    sqlite3  # Required by sqlite_lightning_db.cpp (sqlite3_open, sqlite3_close, sqlite3_exec)
    lightning_core_static  # Lightning Network dependencies (libwally, secp256k1-zkp, blake3)
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
  )

  # Link proto types only when gRPC is enabled (dev mode)
  if(TARGET dinerod_proto)
    target_link_libraries(dinero_lightning PUBLIC dinerod_proto)
  endif()
endif() # ENABLE_LIGHTNING

# RPC handlers library (includes GUI-compatible handlers)
add_library(dinero_rpc_handlers STATIC
  src/daemon/rpc/wallet_gui_handlers.cpp
  src/daemon/rpc/wallet_import_handlers.cpp  # Phase W.1.1: Wallet import/rescan (importmnemonic RPC)
  src/core/rpc/wallet_descriptor_rpc_handlers.cpp  # Phase 1: Descriptor wallet RPCs (listdescriptors, getdescriptorinfo, deriveaddresses)
  src/daemon/rpc/encrypted_key_validator.cpp  # Phase W.1.1: Encrypted key validation (used by wallet_import_handlers)
  src/common/log_redactor.cpp  # Phase W.1.1: Log redaction for sensitive data (used by wallet_import_handlers)
  src/daemon/rpc/MiningExtrasHandlers.cpp
  src/rpc/methods_mining_extras.cpp
  src/rpc/methods_mining_extras_vnext.cpp
  src/rpc/methods_mining_vnext.cpp  # Stratum RPC methods (mining.getstratuminfo)
  src/rpc/methods_mining_gpu.cpp  # GPU Mining RPC methods (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
  src/rpc/methods_pool.cpp  # Pool accounting RPC (pool.stats, pool.workers, pool.blocks, pool.payouts)
  src/rpc/address_validation.cpp  # Address encoding/decoding (GetChainParams, EncodeBech32P2WPKH)
  # Legacy RPC implementations (still used by context-aware wrappers)
  src/rpc/methods_market.cpp
  src/rpc/methods_discovery.cpp
  src/rpc/methods_bridge.cpp
  src/rpc/methods_multiasset.cpp
  src/rpc/methods_payment.cpp
  src/rpc/methods_auth.cpp
  $<$<BOOL:${ENABLE_HARDWARE_WALLETS}>:src/rpc/methods_hardware_wallet.cpp>
  src/rpc/methods_descriptor.cpp  # Phase 3C: Descriptor RPC methods
  src/rpc/methods_telemetry.cpp
  src/rpc/methods_daemon_status.cpp  # Daemon readiness contract (getdaemonstatus)
  src/rpc/cpu_stats_rpc.cpp  # Phase E.3.1: CPU stats & resource monitoring
  src/rpc/rpc_init.cpp        # Phase E.3.1: RPC registration (RegisterAllRPCMethods)
  src/rpc/mining_control_rpc.cpp  # Phase Y: CPU miner control (start/stop/getstatus)
  src/rpc/ct_fee_rpc.cpp  # Phase 3: CT fee configuration RPC (ct.setminfee, ct.getfeeconfig, etc.)
  # src/rpc/methods_consensus.cpp  # Disabled: compilation errors with storage namespace
  # src/rpc/methods_silent_payments.cpp  # Disabled: needs wallet integration
  src/rpc/methods_peer_scoring.cpp  # Phase 5D: Peer scoring & DoS protection RPC
  src/rpc/methods_headers_sync.cpp  # Phase 5A: Headers-first sync RPC
  src/rpc/methods_compact_blocks.cpp  # Phase 5B: Compact block relay RPC
  src/rpc/methods_address_manager.cpp  # Phase 5C: Address manager RPC
  src/rpc/methods_rbf_policy.cpp  # Phase 5E: RBF policy RPC
  src/rpc/methods_utreexo.cpp  # Phase 34.2: Utreexo accumulator RPC (getutxoproof, getutreexoroots, etc.)
  src/rpc/methods_utreexo_batch.cpp  # Phase 11a.1: Utreexo batch RPC (getutxoproofs_batch, verifyutxoproofs_batch)
  src/rpc/methods_utreexo_register.cpp  # Phase 11a.1: Utreexo RPC registration
  src/rpc/methods_utreexo_bridge.cpp  # Phase 34.3: Utreexo Bridge API for light clients (utreexo.getproof, getroots, getstate)
  # Liquidity Vault — daemon-side source of truth (Track C).
  src/vault/ledger.cpp
  src/vault/ledger_account.cpp
  src/vault/deposit_flow.cpp
  src/vault/reorg_watcher.cpp
  src/vault/withdrawal_queue.cpp
  src/vault/signing_backend.cpp
  src/vault/vault_service.cpp
  src/vault/ledger_store.cpp
  src/vault/vault_runtime.cpp
  src/vault/wallet_signing_backend.cpp
  src/rpc/methods_vault.cpp
  src/daemon/rpc/websocket_handlers.cpp
  # Service implementations (bridge, payment, auth, etc.)
  src/bridge/fiat_bridge_manager.cpp
  src/bridge/routing_engine.cpp
  src/daemon/arp_manager.cpp
  src/rpc/payment_monitor.cpp
  src/rpc/event_bus.cpp
  src/rpc/token_manager.cpp
  src/rpc/session_manager.cpp
  src/rpc/auth_manager.cpp
  src/contracts/multiasset_escrow_contract.cpp
  src/contracts/multiasset_persistence.cpp
  # Week 2: Context-aware RPC handlers (migration from legacy globals)
  src/rpc/methods_blockchain_context.cpp
  src/rpc/methods_utxoset.cpp
  src/rpc/methods_address_index.cpp
  src/core/rpc/blockchain_rpc_metadata.cpp  # RPC metadata registration
  src/rpc/methods_wallet_context.cpp
  src/rpc/wallet_sync_rpc_handlers.cpp  # Phase W.2.6: Wallet sync UX RPC methods
  src/rpc/wallet_sync_aggregator.cpp  # Phase W.2.6: Single aggregation point for sync status
  src/rpc/ergonomics_rpc_handlers.cpp  # Phase W.3: High-level status aggregation (getnodehealth, getminingreadiness)
  src/rpc/fee_bump_rpc_handlers.cpp  # Phase W.4.4: Fee bump RPC methods (gettxinclusion, gettxbumprecommendation)
  src/rpc/methods_mining_context.cpp  # Phase F.5: Updated for MiningManager v2 + jsoncpp API
  src/rpc/methods_mining_v14.cpp  # v0.14.0.3: BlockAssembler-based mining RPC (getblocktemplate, submitblock)
  src/rpc/methods_mempool_context.cpp
  src/rpc/methods_network_context.cpp
  src/rpc/methods_contract_context.cpp
  src/rpc/methods_economics_context.cpp
  src/rpc/methods_economics_vnext.cpp  # VNext DSL with full metadata (rpc.getcontext, etc.)
  src/rpc/methods_payment_context.cpp
  src/rpc/methods_sync_context.cpp
  src/rpc/methods_auth_context.cpp
  src/rpc/methods_multiasset_context.cpp
  src/rpc/methods_market_context.cpp
  src/rpc/methods_bridge_context.cpp
  src/rpc/methods_discovery_context.cpp
  src/rpc/methods_openrpc.cpp  # OpenRPC schema/introspection RPC (rpc.openrpc, rpc.schema, ...)
  src/rpc/openrpc_generator.cpp
  $<$<BOOL:${ENABLE_HARDWARE_WALLETS}>:src/rpc/methods_hardware_wallet_context.cpp>
  src/rpc/methods_descriptor_context.cpp  # Phase 3C: Context-aware descriptor RPC wrappers
  src/rpc/methods_remaining_context.cpp
  src/rpc/diagnostics_rpc_handlers_context.cpp  # node.info, rpc.methods (Nov 2025)
  src/rpc/logging_rpc_handlers_context.cpp  # logging.setlevel, logging.getlevel (Step C: Dynamic log control)
  src/rpc/logs_rpc_handlers_context.cpp  # logs.recent, logs.services, logs.tail (Unified log aggregator)
  src/rpc/v7_pq_rpc_json.cpp             # Phase 4c.3.1: V7 post-quantum wallet RPC JSON adapters (getnewp2mraddress, listp2mraddresses, signp2mr)
  src/rpc/shielded_rpc_json.cpp          # V7 shielded pool RPCs: wallet.shield, wallet.unshield, wallet.shieldedbalance
  # NOTE: src/rpc/v7_pq_handlers.cpp is part of dinero_pq (src/consensus/pq/CMakeLists.txt) — linked via target_link_libraries(... dinero_pq)
  src/dpi/dpi_protocol.cpp               # DPI Phase 1: Payment Intent protocol (serialize, verify, tagged hash)
  src/rpc/dpi_rpc_handlers.cpp           # DPI Phase 1: RPC handlers (createinvoice, payinvoice, verifypackage, checkconflicts)
  # DISABLED: Payroll feature (experimental Lightning-based payroll with ZK privacy)
  # Re-enable behind a CMake flag once core functionality is stable
  # src/database/payroll_db.cpp  # PayrollDB: RocksDB-based payroll storage
  # src/rpc/payroll_rpc.cpp  # payroll.createinvoice, payroll.payall, payroll.report (Private Payroll over Lightning)
  # Week 9: GPU Mining RPC handlers (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
  # Common utilities (thread isolation for Lightning)
  src/common/thread_pool.cpp
  # Phase 7: Lightning Network (channels, HTLCs, routing, payments) - DISABLED FOR GENESIS
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/rpc/methods_lightning.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_db.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/sqlite_lightning_db.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_peer.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/channel_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/htlc_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/payment_router.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/commitment_builder.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_service.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_wallet.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_crypto.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/invoice.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/bech32.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/onion.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/onion_error.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/payment_utils.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/gossip_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/watchtower_client.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_sweep_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_events.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_event_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_websocket_broadcaster.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_rpc_handlers.cpp>  # Phase 14: RPC endpoints for Lightning event subscriptions
  # Phase 31: Multi-Asset Lightning Settlement Layer - DISABLED FOR GENESIS
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/asset_channel.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/asset_htlc.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/swap_htlc.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/routing_multiasset.cpp>
  src/daemon/websocket_server.cpp  # Phase 14.4: WebSocket server (needed for broadcast_lightning_event)
  src/daemon/security_hardening.cpp  # Required by websocket_server.cpp
  src/security/auth_scrubber.cpp  # Required by security_hardening.cpp
)
target_include_directories(dinero_rpc_handlers BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_rpc_handlers PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/daemon  # For daemon headers (http_rpc_server.h, p2p_manager.h, etc.)
)
target_link_libraries(dinero_rpc_handlers PRIVATE
  dinero_core      # Needed for address_validator.cpp (IsValidDineroAddress)
  dinero_wallet
  dinero_crypto
  dinero_pq        # Phase 4c.3.1: V7 post-quantum wallet RPC handlers (v7_pq_handlers.cpp, v7_pq_rpc_json.cpp)
  dinero_shielded  # V7 shielded pool: wallet.shield, wallet.unshield RPCs
  OpenSSL::Crypto  # Phase 4c.3.1: v7_pq_rpc_json.cpp uses OPENSSL_cleanse directly
  # RocksDB::rocksdb removed - was only needed for payroll_db.cpp (now disabled)
)
target_compile_definitions(dinero_rpc_handlers PRIVATE
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
)

# Link ZK library if enabled (for zk.* RPC methods)
if(ENABLE_ZK)
  target_link_libraries(dinero_rpc_handlers PRIVATE dinero_zk)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# dinero_rpc_client — FIREWALL SURFACE for downstream consumers
# ═══════════════════════════════════════════════════════════════════════════
#
# This is the only RPC-client library that targets outside the daemon
# (dinero-cli, dinero-qt, dinero-solo-miner, dinero-wallet-cli, dinero-gpu-miner)
# may link against. It is intentionally minimal: src/rpc_client.cpp plus a
# JSON dependency, no consensus headers, no chain state, no validation, no
# database. A target linking dinero_rpc_client MUST NOT transitively pull
# dinero_consensus, dinero_chainstate, dinero_validation, or any other
# chain-side library.
#
# Why this matters
#   The Qt GUI, miners, and CLI talk to dinerod over JSON-RPC. They are
#   *clients* of the node, not co-residents of its consensus logic.
#   Keeping them on this thin surface means a Qt crash cannot corrupt
#   chain state, a miner bug cannot fork the chain, and consensus
#   refactors don't ripple into the wallet GUI.
#
# How the firewall is enforced
#   target_link_libraries() visibility. dinero_rpc_client PUBLIC-links
#   only jsoncpp_static. Anything richer must be a deliberate amendment
#   here, reviewed against this contract.
#
# The compile-fail probe in tests/build/qt_firewall_test.cmake exists to
# catch regressions — a Qt source file that #includes "validation.h"
# must fail to build. If you weaken this contract, that probe will go
# silent. Don't.
# ═══════════════════════════════════════════════════════════════════════════
add_library(dinero_rpc_client STATIC
  src/rpc_client.cpp
)
target_include_directories(dinero_rpc_client PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
# Use bundled jsoncpp_static (cross-platform, no Homebrew dependency).
# Do NOT add consensus/chainstate/validation libraries here — see firewall
# contract above.
target_link_libraries(dinero_rpc_client PUBLIC jsoncpp_static)

# Consensus target wiring lives with src/consensus/.
include(src/consensus/CMakeLists.txt)

add_subdirectory(src/storage)

# Mobile and embedded-node FFI target wiring lives with the source dirs.
include(wallet-core/ffi/CMakeLists.txt)
include(src/nodecore/CMakeLists.txt)

# ─────────────────────────────────────────────────────────────────────────
# Wallet Test Dependencies (Minimal stubs for testing)
# ─────────────────────────────────────────────────────────────────────────
add_library(wallet_test_deps STATIC
  src/wallet/utxo_index.cpp
  src/sqlite_open.cpp
  src/daemon/lightning_stubs.cpp
)

target_link_libraries(wallet_test_deps PUBLIC
  dinero_wallet
  dinero_chainstate  # ChainDB (WalletManager::rescanBlockchain dependency)
  RocksDB::rocksdb
  sqlite3
)

target_include_directories(wallet_test_deps PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Bridge and mining-pool support target wiring lives with the source dirs.
include(src/network/CMakeLists.txt)
include(src/pool/CMakeLists.txt)

# Live P2P/network source list consumed by dinero_core.
include(src/p2p/CMakeLists.txt)

# ─────────────────────────────────────────────────────────────────────────
# Layer 3: Core Logic Library (Reusable, Testable)
# ─────────────────────────────────────────────────────────────────────────
# This library contains all core business logic that can be tested independently.
# Excludes: main(), RPC server, gRPC server, WebSocket server
# Includes: consensus, mining, mempool, services, metrics, daemon context
add_library(dinero_core STATIC
  # Daemon context (dependency injection container access)
  src/daemon/daemon_context.cpp
  src/daemon/gui_websocket_events.cpp
  src/daemon/ws_subscriptions.cpp
  src/daemon/ws_globals.cpp

  # Phase 3a of the shielded reorg invertibility plan
  # (docs/specs/atomic_consensus_persistence_phase3.md). Skeleton
  # only — opt-in via -consensus.atomic_persist=1, default off.
  src/consensus/consensus_write_batch.cpp

  # Service wrappers (dependency injection containers)
  src/daemon/services/config_service.cpp
  src/daemon/services/wallet_service.cpp
  src/daemon/services/chainstate_service.cpp
  src/daemon/services/mempool_service.cpp
  src/daemon/services/metrics_service.cpp
  src/daemon/services/p2p_service.cpp

  # Phase D.4 (Dinero Core 1.0) — pure health-check decision logic
  src/daemon/health.cpp
  src/daemon/services/mining_service.cpp
  src/daemon/services/rpc_service.cpp
  src/daemon/services/peer_scoring_service.cpp
  src/daemon/services/headers_sync_service.cpp
  src/daemon/services/compact_block_service.cpp
  src/daemon/services/address_manager_service.cpp
  src/daemon/services/rbf_policy_service.cpp
  src/daemon/services/block_ingress_service.cpp  # Step 5: IBlockIngress implementation
  src/daemon/services/prune_service.cpp  # Phase 34.8: Block pruning for mobile nodes

  # Step 5: Interface type implementations (canonical ingress types)
  src/daemon/interfaces/ingress_types.cpp

  # Mining subsystem (Phase C - core logic)
  src/mining/block_assembler.cpp
  src/mining/block_assembly_context.cpp  # Phase W.1: Network-aware assembly context
  src/mining/transaction_scorer.cpp  # Phase W.1: Intelligent transaction scoring
  src/mining/ct_selection_policy.cpp  # CT-specific transaction selection policies
  src/mining/template_delta_tracker.cpp  # Phase W.1: Incremental template refresh
  src/mining/tx_inclusion_analyzer.cpp  # Phase W.4.1: Transaction inclusion analysis (read-only)
  src/mining/mining_manager_v2.cpp
  # src/mining/mining_manager.cpp  # Phase C: Replaced by mining_manager_v2.cpp (ODR violation fix)
  src/mining/miner.cpp
  src/mining/mining_script_override.cpp
  src/mining/midstate_cache.cpp
  src/mining/address_validator.cpp  # Phase F.5: Address validation for mining RPC
  src/mining/payout_spec.cpp  # Phase C1: Daemon-defined pool payouts
  src/daemon/mining_safety_gates.cpp
  src/daemon/mining_events.cpp
  src/daemon/mining_metrics.cpp
  src/daemon/header_metadata_recovery.cpp
  src/daemon/undo_rebuild_orchestrator.cpp  # Offline --rebuild-undo-range orchestrator (commit #4 of post-Apr 30 series)

  # Consensus layer
  src/consensus/reindexer.cpp
  src/consensus/undo.cpp
  src/consensus/header_sync_manager.cpp
  src/consensus/header_chain.cpp
  src/consensus/header_store.cpp
  src/consensus/header_sync.cpp
  src/consensus/header_sync_p2p.cpp
  src/consensus/proof_gossip.cpp
  src/consensus/block_download_scheduler.cpp
  src/consensus/assume_utxo.cpp
  src/consensus/parallel_block_validator.cpp
  src/consensus/validation_worker_pool.cpp
  src/consensus/validation_queue.cpp
  src/consensus/block_validation.cpp
  src/consensus/block_index.cpp
  src/consensus/block_lifecycle.cpp
  src/consensus/orphan_manager.cpp
  src/consensus/block_index_persistence.cpp
  src/consensus/genesis_canonical.cpp  # Phase 3: Genesis hash self-test
  src/consensus/adapters/wallet_utxo_adapter.cpp  # UTXO adapter for wallet integration

  # Mempool
  src/mempool/mempool.cpp  # Full mempool implementation (needed by compact_block.cpp)
  src/mempool/coins_view_mempool.cpp
  src/mempool/mempool_persistence.cpp
  src/mempool/fee_estimator.cpp
  src/mempool/invalid_tx_cache.cpp
  src/mempool/tx_orphan_pool.cpp  # Transaction orphan pool (holds TXs with missing parents)

  # Network relay

  # Common utilities
  src/common/logger.cpp
  src/common/json_logger.cpp
  src/common/logger_router.cpp
  src/common/address_script_builder.cpp
  src/common/config_manager.cpp
  src/util/ser.cpp  # Compact size varint serialization

  # Metrics
  src/metrics/metrics_registry.cpp

  ${DINERO_CORE_P2P_SOURCES}

  # Policy layer
  src/policy/mempool_policy.cpp
  src/policy/rbf_policy.cpp
  src/policy/fee_estimator.cpp

  # Address handling
  src/address/addr_codec.cpp
  src/address/addr_types.cpp

  # Wallet core logic
  src/wallet/wallet_worker.cpp
  src/daemon/hd_wallet_manager.cpp
  src/daemon/wallet_crypto.cpp
  src/wallet/psbt.cpp  # PSBT helpers (Phase 2 Step 4: export for tests)

  # Crypto utilities
  src/crypto/sp_context.cpp
  src/daemon/crypto_utils.cpp
  src/daemon/secure_random.cpp

  # RPC registry (logic only, not server)
  src/rpc/rpc_registry.cpp

  # Database utilities
  src/database/sqlite_manager.cpp
  src/daemon/db_meta_utils.cpp

  # Validation
  src/daemon/block_acceptor.cpp
  src/daemon/block_relay_manager.cpp  # Phase G.2: Block propagation
  src/daemon/tx_relay_manager.cpp  # Phase G.3: Mempool relay

  # Privacy
  src/core/privacy/silent_scanner_manager.cpp
  src/core/privacy/silent_scanner.cpp

  # Encoding/decoding
  src/daemon/bech32_decode.cpp
  src/daemon/bech32_encoder.cpp
  src/wallet/p2mr_address.cpp            # Phase 4a: V7 P2MR bech32m address codec (din1r1...)

  # Genesis initialization (subsidy now in dinero_consensus library)
  src/daemon/genesis_init.cpp

  # Transaction pool
  src/daemon/transaction_pool.cpp

  # CLI version info
  src/cli/version.cpp

  # IPC oracle clients (Phase 9.2: Lightning event forwarding)
  src/ipc/oracles/chain_oracle_client.cpp
  src/ipc/oracles/time_oracle_client.cpp
  src/ipc/oracles/transaction_oracle_client.cpp

  # Chainstate restart metadata persistence
  src/storage/chainstate_metadata.cpp
)

target_include_directories(dinero_core BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Quality hardening: enable compiler warnings for core library
if(NOT MSVC)
  target_compile_options(dinero_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-mismatched-tags)
else()
  target_compile_options(dinero_core PRIVATE /W3)
endif()

target_link_libraries(dinero_core PUBLIC
  dinero_crypto
  dinero_chainstate
  dinero_consensus
  dinero_bridge           # BridgeNode::* (Utreexo proof gen) — used by chainstate_service.cpp
  dinero_pool             # PoolDB / PoolManager / PayoutCalculator — used by chainstate_service.cpp
  dinero_wallet
  dinero_lightning_keys  # Phase 3: Lightning key derivation for gRPC wallet service
  dinero_gpu_mining      # GPU mining backends (Metal/CUDA/OpenCL)
  dinero_portmap_deps    # Optional UPnP/NAT-PMP P2P port mapping
  jsoncpp_static
  sqlite3
  secp256k1
  RocksDB::rocksdb
)

# ═════════════════════════════════════════════════════════════════════
# Phase 3: Lightning Build-Time Decoupling COMPLETE
# ═════════════════════════════════════════════════════════════════════
# Lightning is now EXTERNAL - runs as standalone lightningd process
# - dinerod = pure L1 node (NO Lightning library linked)
# - lightningd = optional L2 daemon (links dinero_lightning)
# - Communication via gRPC (WalletClient → dinerod gRPC server)
#
# dinerod always uses stubs (Lightning service not embedded)
# ═════════════════════════════════════════════════════════════════════
target_sources(dinero_core PRIVATE src/daemon/lightning_stubs.cpp)
message(STATUS "⚡ Lightning: EXTERNAL (use standalone 'lightningd' binary)")
message(STATUS "   dinerod = pure L1 node (blockchain, mempool, wallet, gRPC)")
message(STATUS "   lightningd = optional L2 daemon (channels, HTLCs, routing)")

# Add compile definitions needed by core logic
target_compile_definitions(dinero_core PRIVATE
  CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"  # SQL-first schema path
)

# ─────────────────────────────────────────────────────────────────────────
# Layer 4: Daemon Executable (Thin shell around libdinero_core)
# ─────────────────────────────────────────────────────────────────────────
add_subdirectory(src/daemon)

# Preserve the previous root-scope compile definition for subsequent
# non-Apple targets. The daemon target also gets it directly in src/daemon.
if(NOT APPLE)
  add_compile_definitions(NO_WALLET_ENCRYPTION)
endif()

# Platform-specific linking with proper crypto libraries
# NOTE: OpenSSL is already configured earlier (lines 110-152)
# Don't call find_package again as it overrides vendored imported targets
if(NOT TARGET OpenSSL::Crypto)
  # This should never happen - OpenSSL targets are created above
  message(FATAL_ERROR "OpenSSL::Crypto target not found - check OpenSSL configuration above")
endif()

# Wallet encryption uses PBKDF2-HMAC-SHA512 (built into OpenSSL, no external dependencies)

# ═══════════════════════════════════════════════════════════════════════════
# jsoncpp is now BUNDLED in third_party/jsoncpp (no external dependency needed)
# ═══════════════════════════════════════════════════════════════════════════
# All targets use jsoncpp_static (Mac + Linux, no Homebrew or pkg-config needed)
# Legacy find_package code commented out:
#
# find_package(PkgConfig QUIET)
# if(PKG_CONFIG_FOUND)
#     pkg_check_modules(JSONCPP jsoncpp)
# endif()
# if(NOT JSONCPP_FOUND)
#     find_path(JSONCPP_INCLUDE_DIR json/json.h)
#     find_library(JSONCPP_LIBRARY jsoncpp)
# endif()
# Standalone Lightning daemon target wiring lives with src/lightningd/.
include(src/lightningd/CMakeLists.txt)

# Native MSVC needs CMake/vcpkg's imported libcurl target. Keep the existing
# raw `curl` link name for Unix-style builds that already resolve -lcurl.
if(WIN32)
  find_package(CURL REQUIRED)
  set(DINERO_CURL_TARGET CURL::libcurl)
else()
  set(DINERO_CURL_TARGET curl)
endif()

# Standalone developer/miner tool target wiring lives with tools/.
include(tools/CMakeLists.txt)

# Product CLI target wiring lives with src/cli/.
include(src/cli/CMakeLists.txt)

# Embed VS_VERSION_INFO into each daemon binary so Explorer Properties
# -> Details shows the right version/company/component rather than an
# empty dialog. No-op outside Windows. See dinero_attach_win32_versioninfo()
# definition near the top of this file.
dinero_attach_win32_versioninfo(dinerod               "Dinero full node daemon")

# Reference wallet CLI and offline test wiring lives with src/wallet/reference/.
include(src/wallet/reference/CMakeLists.txt)

# External sibling project discovery stays in cmake/.
include(cmake/ExternalProjects.cmake)

# Install and package-gate policy lives in cmake/.
include(cmake/InstallRules.cmake)

# Test target registration lives in tests/CMakeLists.txt.
# Keep this as include() rather than add_subdirectory(): the legacy test
# definitions use root-relative source paths and root-scope CTest properties.
include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)

# Optional in-tree component switches live in cmake/.
include(cmake/OptionalComponents.cmake)
