# Wallet FFI target wiring.
#
# This file is included from the repository root CMakeLists.txt rather than
# added as a subdirectory because the legacy source list below intentionally
# uses root-relative paths.

# ─────────────────────────────────────────────────────────────────────────
# Wallet FFI Library (C API for iOS/Mobile/Rust)
# ─────────────────────────────────────────────────────────────────────────
# This library provides a C-compatible API for use via FFI from:
# - iOS Swift apps (via bridging header)
# - Android JNI
# - Rust (via cbindgen)
# - Any language with C FFI support
#
# Built as STATIC library for iOS/mobile embedding.
# ─────────────────────────────────────────────────────────────────────────
add_library(dinero_wallet_ffi STATIC
  wallet-core/ffi/wallet_ffi.cpp
  wallet-core/ffi/kyc_provider_mock.cpp
  wallet-core/ffi/kyc_provider_registry.cpp
)

target_include_directories(dinero_wallet_ffi PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/wallet-core/ffi
)

target_link_libraries(dinero_wallet_ffi PUBLIC
  dinero_wallet       # HD wallet, signing, coin selection
  dinero_crypto       # Cryptographic primitives
  jsoncpp_static      # JSON serialization
  OpenSSL::Crypto     # For secure random, hashing
)

# Export C API header for Swift/ObjC bridging
set_target_properties(dinero_wallet_ffi PROPERTIES
  PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/wallet-core/ffi/wallet_ffi.h"
)

# iOS-specific configuration
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(STATUS "📱 iOS build: Configuring dinero_wallet_ffi for iOS")

  # Ensure static library for iOS embedding
  set_target_properties(dinero_wallet_ffi PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    POSITION_INDEPENDENT_CODE ON
  )

  # iOS frameworks needed for crypto operations
  target_link_libraries(dinero_wallet_ffi PUBLIC
    "-framework Security"
    "-framework Foundation"
  )
endif()
