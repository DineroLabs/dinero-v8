# Top-level build options and mode switches.
#
# Included from the repository root before platform-specific definitions and
# before dependency setup, because these options influence later target wiring.

option(DINERO_USE_VENDORED_DEPS "Use vendored third-party dependencies (ON for CI/releases, OFF for local dev)" ON)
option(ENABLE_SANITIZERS "Enable ASan/UBSan for debug builds" OFF)
option(ENABLE_GPU_MINING "Enable GPU mining support (OpenCL/CUDA)" ON)
option(ENABLE_LIGHTNING "Build Lightning Network support (Layer 3, off-chain)" OFF)
option(ENABLE_TREZOR "Enable Trezor hardware wallet support" OFF)
option(ENABLE_HARDWARE_WALLETS "Enable Ledger/Trezor USB HID hardware wallet support" ON)
option(ENABLE_UTREEXO_INVARIANT_CHECKS "Enable Utreexo Pollard/Stump invariant checks (CI/debug only)" OFF)
option(DIN_ENABLE_LEGACY_RPC "Enable deprecated legacy RPC server code paths" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(DINERO_ENABLE_PORTMAPPING OFF CACHE BOOL "Enable optional UPnP/NAT-PMP P2P port mapping support when libraries are available" FORCE)
else()
  option(DINERO_ENABLE_PORTMAPPING "Enable optional UPnP/NAT-PMP P2P port mapping support when libraries are available" ON)
endif()
option(DINERO_ENABLE_QUIC "Build vendored ngtcp2 QUIC transport dependency (Phase B2; transport activation remains gated)" OFF)
option(DINERO_ENABLE_QUIC_CRYPTO "Build the ngtcp2 OpenSSL QUIC TLS bridge when the active OpenSSL exposes compatible APIs" ON)
option(DINERO_WINDOWS_SERVER_BUILD "Build the Windows headless server artifact without daemon GPU/hardware-wallet load-time dependencies" OFF)

if(DINERO_WINDOWS_SERVER_BUILD)
  if(NOT WIN32)
    message(FATAL_ERROR "DINERO_WINDOWS_SERVER_BUILD=ON is only supported on Windows")
  endif()

  set(ENABLE_GPU_MINING OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)
  set(MINER_ENABLE_CUDA OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)
  set(MINER_ENABLE_OPENCL OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)
  set(ENABLE_HARDWARE_WALLETS OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)
  set(ENABLE_GRPC OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)
  set(DINERO_BUILD_QT OFF CACHE BOOL "Forced OFF by DINERO_WINDOWS_SERVER_BUILD" FORCE)

  message(STATUS "DINERO_WINDOWS_SERVER_BUILD=ON - Windows headless server artifact")
  message(STATUS "   - daemon GPU mining: forced OFF (no CUDA/NVRTC load-time imports)")
  message(STATUS "   - solo-miner CUDA/OpenCL: forced OFF for this headless lane")
  message(STATUS "   - hardware wallets and gRPC: forced OFF")
endif()

# Phase E.3 (Dinero Core 1.0) packaged-service build mode.
# The .deb rules flip this ON to produce the daemon + CLI artifact contract.
option(DINERO_PACKAGED_BUILD "Build the .deb / packaged-service artifact (Linux only)" OFF)
if(DINERO_PACKAGED_BUILD)
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "DINERO_PACKAGED_BUILD=ON only supported on Linux")
  endif()

  set(ENABLE_GPU_MINING OFF CACHE BOOL "Forced OFF by DINERO_PACKAGED_BUILD" FORCE)
  set(ENABLE_HARDWARE_WALLETS OFF CACHE BOOL "Forced OFF by DINERO_PACKAGED_BUILD" FORCE)
  set(DINERO_RELEASE ON CACHE BOOL "Forced ON by DINERO_PACKAGED_BUILD" FORCE)

  message(STATUS "DINERO_PACKAGED_BUILD=ON - packaged-service artifact (Linux .deb)")
  message(STATUS "   - GPU mining: forced OFF (drops libOpenCL from NEEDED)")
  message(STATUS "   - Hardware wallets: forced OFF (drops libudev from NEEDED)")
  message(STATUS "   - DINERO_RELEASE=ON: forced (drops gRPC stack)")
  message(STATUS "   - INSTALL_RPATH: /usr/lib/dinero (DT_RUNPATH via --enable-new-dtags)")
  message(STATUS "   - Installed binaries: dinerod, dinero-cli only")
endif()

# Set after DINERO_PACKAGED_BUILD so forced hardware-wallet state is final.
if(ENABLE_HARDWARE_WALLETS)
  add_compile_definitions(DINERO_HAVE_HARDWARE_WALLETS=1)
endif()

if(ENABLE_UTREEXO_INVARIANT_CHECKS)
  add_compile_definitions(ENABLE_UTREEXO_INVARIANT_CHECKS)
  message(STATUS "Utreexo invariant checks ENABLED (Pollard/Stump cross-validation)")
endif()

option(BUILD_DINEROD "Build L1 blockchain daemon" ON)
option(BUILD_LIGHTNINGD "Build L2 Lightning daemon" ON)

# iOS/mobile FFI-only build mode.
option(BUILD_FFI_ONLY "Build only wallet FFI library (for iOS/mobile)" OFF)
if(BUILD_FFI_ONLY)
  message(STATUS "FFI-ONLY MODE: Building minimal wallet FFI for iOS/mobile")
  message(STATUS "   - Daemon, Lightning, GUI, P2P: DISABLED")
  message(STATUS "   - Only building: dinero_crypto, dinero_wallet, dinero_wallet_ffi")

  set(BUILD_DINEROD OFF CACHE BOOL "" FORCE)
  set(BUILD_LIGHTNINGD OFF CACHE BOOL "" FORCE)
  set(BUILD_GUI OFF CACHE BOOL "" FORCE)
  set(ENABLE_GRPC OFF CACHE BOOL "" FORCE)
  set(ENABLE_GPU_MINING OFF CACHE BOOL "" FORCE)
  set(ENABLE_LIGHTNING OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_FUZZING OFF CACHE BOOL "" FORCE)
  set(ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
  set(ENABLE_ZK OFF CACHE BOOL "" FORCE)

  add_compile_definitions(BUILD_FFI_ONLY)
endif()

# Release-grade build mode (Bitcoin Core style).
option(DINERO_RELEASE "Release-grade build (static deps, no gRPC)" OFF)
if(DINERO_RELEASE)
  message(STATUS "RELEASE MODE: Building exchange-grade binaries")
  message(STATUS "   - Static linking enforced")
  message(STATUS "   - gRPC/protobuf/abseil disabled")
  message(STATUS "   - Lightning IPC via raw sockets")

  add_compile_definitions(DINERO_RELEASE_BUILD)
  add_compile_definitions(DISABLE_GRPC)
  set(ENABLE_GRPC OFF)
  set(USE_SYSTEM_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  message(STATUS "DEV MODE: Development build with dynamic linking")
  message(STATUS "   - gRPC disabled by default; pass -DENABLE_GRPC=ON for local IPC work")
  message(STATUS "   - Homebrew dependencies OK")
endif()

# Sanitizer wiring (single source of truth).
set(DIN_ENABLE_ASAN ${ENABLE_SANITIZERS})
include(Sanitizers)
