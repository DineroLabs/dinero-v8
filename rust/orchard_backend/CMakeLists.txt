cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dinero_orchard_backend LANGUAGES C CXX)
  include(CTest)
endif()
if(NOT TARGET secp256k1)
  # Use the repository-pinned library, never a system secp256k1 substitute.
  set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
  set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE BOOL "" FORCE)
  set(SECP256K1_ENABLE_MODULE_SCHNORRSIG ON CACHE BOOL "" FORCE)
  foreach(feature ECDH RECOVERY ELLSWIFT GENERATOR RANGEPROOF SURJECTIONPROOF WHITELIST MUSIG ECDSA_ADAPTOR ECDSA_S2C BPPP)
    set(SECP256K1_ENABLE_MODULE_${feature} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(kind BENCHMARK TESTS EXHAUSTIVE_TESTS CTIME_TESTS EXAMPLES)
    set(SECP256K1_BUILD_${kind} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/secp256k1-zkp" "secp256k1")
endif()
# This first build integration supports native Linux/macOS. Refuse accidental
# cross compilation until the Rust target and native libraries are qualified.
if(CMAKE_CROSSCOMPILING OR WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "Orchard backend cross/platform toolchain is not qualified yet")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    message(FATAL_ERROR "Orchard backend currently requires the native macOS architecture")
  endif()
endif()
# file(STRINGS) inputs must also invalidate an existing configured build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include/consensus/tx_validation.h")
find_program(ORCHARD_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml" ORCHARD_TOOLCHAIN_LINE REGEX "^channel =")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" ORCHARD_RUST_TOOLCHAIN "${ORCHARD_TOOLCHAIN_LINE}")
if(NOT ORCHARD_RUST_TOOLCHAIN)
  message(FATAL_ERROR "Missing pinned Orchard Rust toolchain")
endif()
set(ORCHARD_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(ORCHARD_ARCHIVE "${ORCHARD_TARGET_DIR}/release/libdinero_orchard_backend.a")
add_custom_command(OUTPUT "${ORCHARD_ARCHIVE}"
  COMMAND ${CMAKE_COMMAND} -E env RAYON_NUM_THREADS=2
    ${ORCHARD_CARGO} +${ORCHARD_RUST_TOOLCHAIN} build --locked --release --jobs 2
    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    --target-dir "${ORCHARD_TARGET_DIR}"
  DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml src/lib.rs src/frontier.rs src/wallet.rs src/wallet_prover.rs src/wallet_note.rs src/wallet_witness.rs
    include/orchard_backend_ffi.h tests/fixtures/candidate-spend.bundle
    tests/fixtures/candidate-spend.digest tests/fixtures/candidate-spend.effect
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
add_custom_target(dinero_orchard_rust_build DEPENDS "${ORCHARD_ARCHIVE}")
add_library(dinero_orchard_rust STATIC IMPORTED)
set_target_properties(dinero_orchard_rust PROPERTIES IMPORTED_LOCATION "${ORCHARD_ARCHIVE}")
add_dependencies(dinero_orchard_rust dinero_orchard_rust_build)
add_library(dinero_orchard_backend STATIC orchard_backend.cpp orchard_signing.cpp orchard_transaction.cpp orchard_wallet.cpp)
target_compile_features(dinero_orchard_backend PUBLIC cxx_std_20)
target_include_directories(dinero_orchard_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
find_package(Threads REQUIRED)
if(NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()
target_link_libraries(dinero_orchard_backend PUBLIC dinero_orchard_rust OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(dinero_orchard_backend PUBLIC "-framework Security" "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(dinero_orchard_backend PUBLIC m)
endif()
find_package(SQLite3 REQUIRED)
add_library(dinero_orchard_wallet_storage STATIC orchard_wallet_storage.cpp)
target_link_libraries(dinero_orchard_wallet_storage PUBLIC dinero_orchard_backend SQLite::SQLite3)
option(DINERO_ORCHARD_BUILD_TESTS "Build Orchard component qualification tests" ON)

# Typed host reader: Orchard never becomes an ordinary Transaction. The root
# build uses the real consensus-core primitives. Standalone qualification builds
# those exact source files, not a second parser or mock implementation.
set(ORCHARD_HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_library(dinero_transaction_reader STATIC
  "${ORCHARD_HOST_ROOT}/src/primitives/transaction_reader.cpp"
  "${ORCHARD_HOST_ROOT}/src/consensus/orchard_coin_snapshot.cpp")
target_include_directories(dinero_transaction_reader PUBLIC "${ORCHARD_HOST_ROOT}/include")
target_link_libraries(dinero_transaction_reader PUBLIC dinero_orchard_backend)
if(TARGET dinero_consensus_core)
  target_link_libraries(dinero_transaction_reader PUBLIC dinero_consensus_core)
else()
  add_library(orchard_historical_codec STATIC
    "${ORCHARD_HOST_ROOT}/src/primitives/transaction.cpp"
    "${ORCHARD_HOST_ROOT}/src/primitives/transaction_serializer.cpp"
    "${ORCHARD_HOST_ROOT}/src/primitives/uint256.cpp"
    "${ORCHARD_HOST_ROOT}/src/crypto/sha256.cpp")
  target_compile_features(orchard_historical_codec PUBLIC cxx_std_20)
  target_include_directories(orchard_historical_codec PUBLIC "${ORCHARD_HOST_ROOT}/include")
  target_link_libraries(dinero_transaction_reader PUBLIC orchard_historical_codec)
endif()
add_library(dinero_orchard_transparent STATIC
  "${ORCHARD_HOST_ROOT}/src/consensus/orchard_transparent.cpp"
  "${ORCHARD_HOST_ROOT}/src/consensus/orchard_authorization.cpp"
  "${ORCHARD_HOST_ROOT}/src/consensus/orchard_state_transition.cpp")
target_link_libraries(dinero_orchard_transparent PUBLIC dinero_transaction_reader secp256k1)
# Storage integration links the real ChainDB only in the repository build.
# Keeping this separate avoids making the historical database depend on Rust.
if(TARGET dinero_chainstate)
  add_library(dinero_orchard_block_reader STATIC
    "${ORCHARD_HOST_ROOT}/src/primitives/orchard_block_reader.cpp")
  target_link_libraries(dinero_orchard_block_reader PUBLIC dinero_transaction_reader dinero_consensus)
  add_library(dinero_orchard_wallet_scan STATIC "${ORCHARD_HOST_ROOT}/src/wallet/orchard_scan_state.cpp")
  target_link_libraries(dinero_orchard_wallet_scan PUBLIC dinero_orchard_block_reader dinero_orchard_transparent dinero_orchard_wallet_storage)
  add_library(dinero_orchard_block_coins STATIC
    "${ORCHARD_HOST_ROOT}/src/consensus/orchard_block_coins.cpp"
    "${ORCHARD_HOST_ROOT}/src/consensus/orchard_forest_transition.cpp")
  target_link_libraries(dinero_orchard_block_coins PUBLIC dinero_orchard_block_reader dinero_orchard_transparent)
  add_library(dinero_orchard_block_staging STATIC
    "${ORCHARD_HOST_ROOT}/src/consensus/orchard_block_staging.cpp")
  target_link_libraries(dinero_orchard_block_staging PUBLIC dinero_orchard_transparent dinero_chainstate dinero_orchard_block_reader dinero_orchard_block_coins)
  target_include_directories(dinero_orchard_block_staging PRIVATE "${ORCHARD_HOST_ROOT}/src")
endif()
if(DINERO_ORCHARD_BUILD_TESTS)
  # Fail closed if the host monetary constant changes or cannot be read.
  # The C++ test compares this host value to both its own value and Rust's ABI.
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../include/consensus/tx_validation.h"
    ORCHARD_HOST_MONEY_LINE REGEX "^constexpr uint64_t MAX_MONEY = [0-9]+ULL;")
  list(LENGTH ORCHARD_HOST_MONEY_LINE ORCHARD_HOST_MONEY_COUNT)
  if(NOT ORCHARD_HOST_MONEY_COUNT EQUAL 1)
    message(FATAL_ERROR "Cannot determine unique host MAX_MONEY for Orchard gate")
  endif()
  string(REGEX MATCH "[0-9]+ULL" ORCHARD_HOST_MAX_MONEY "${ORCHARD_HOST_MONEY_LINE}")
  add_executable(test_orchard_backend tests/backend.cpp)
  target_include_directories(test_orchard_backend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
  target_compile_definitions(test_orchard_backend PRIVATE DINERO_HOST_MAX_MONEY=${ORCHARD_HOST_MAX_MONEY})
  target_link_libraries(test_orchard_backend PRIVATE dinero_orchard_backend)
  add_test(NAME OrchardBackendCpp COMMAND test_orchard_backend "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(test_orchard_wallet tests/wallet.cpp)
  target_link_libraries(test_orchard_wallet PRIVATE dinero_orchard_backend)
  add_test(NAME OrchardWalletKeys COMMAND test_orchard_wallet)
  add_executable(test_orchard_wallet_shield tests/wallet_shield.cpp)
  target_link_libraries(test_orchard_wallet_shield PRIVATE dinero_orchard_backend)
  add_test(NAME OrchardWalletShield COMMAND test_orchard_wallet_shield)
  add_executable(test_orchard_wallet_spend tests/wallet_spend.cpp)
  target_link_libraries(test_orchard_wallet_spend PRIVATE dinero_orchard_backend)
  add_test(NAME OrchardWalletSpend COMMAND test_orchard_wallet_spend)
  add_executable(test_orchard_wallet_storage tests/wallet_storage.cpp)
  target_link_libraries(test_orchard_wallet_storage PRIVATE dinero_orchard_wallet_storage)
  add_test(NAME OrchardWalletStorage COMMAND test_orchard_wallet_storage)
  add_executable(test_orchard_transaction_reader "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_transaction_reader.cpp")
  target_link_libraries(test_orchard_transaction_reader PRIVATE dinero_transaction_reader)
  if(TARGET dinero_consensus_core)
    target_compile_definitions(test_orchard_transaction_reader PRIVATE DINERO_READER_ROOT_BLOCK_TESTS=1)
    # The pure core archive intentionally omits runtime chain selection; its
    # real block/Utreexo codec needs the canonical implementation in this test.
    target_sources(test_orchard_transaction_reader PRIVATE "${ORCHARD_HOST_ROOT}/src/consensus/chainparams_impl.cpp")
  endif()
  add_test(NAME OrchardTransactionReader COMMAND test_orchard_transaction_reader "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(test_orchard_coin_snapshot "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_coin_snapshot.cpp")
  target_link_libraries(test_orchard_coin_snapshot PRIVATE dinero_transaction_reader)
  add_test(NAME OrchardCoinSnapshot COMMAND test_orchard_coin_snapshot "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(test_orchard_transparent "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_transparent.cpp")
  target_link_libraries(test_orchard_transparent PRIVATE dinero_orchard_transparent)
  add_test(NAME OrchardTransparent COMMAND test_orchard_transparent "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(test_orchard_authorization "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_authorization.cpp")
  target_link_libraries(test_orchard_authorization PRIVATE dinero_orchard_transparent)
  add_test(NAME OrchardAuthorization COMMAND test_orchard_authorization "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(test_orchard_state_transition "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_state_transition.cpp")
  target_link_libraries(test_orchard_state_transition PRIVATE dinero_orchard_transparent)
  add_test(NAME OrchardStateTransition COMMAND test_orchard_state_transition "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  if(TARGET dinero_orchard_block_staging)
    add_executable(test_orchard_wallet_scan "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_wallet_scan.cpp")
    target_link_libraries(test_orchard_wallet_scan PRIVATE dinero_orchard_wallet_scan)
    add_test(NAME OrchardWalletScan COMMAND test_orchard_wallet_scan "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(OrchardWalletScan PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;wallet;integration-staging")
    add_executable(test_orchard_forest_transition
      "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_forest_transition.cpp")
    target_link_libraries(test_orchard_forest_transition PRIVATE dinero_orchard_block_coins)
    add_test(NAME OrchardForestTransition COMMAND test_orchard_forest_transition "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(OrchardForestTransition PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
      ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;integration-staging")
    add_executable(test_orchard_block_coins
      "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_block_coins.cpp")
    target_link_libraries(test_orchard_block_coins PRIVATE dinero_orchard_block_coins)
    add_test(NAME OrchardBlockCoins COMMAND test_orchard_block_coins "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(OrchardBlockCoins PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
      ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;integration-staging")
    add_executable(test_orchard_block_reader
      "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_block_reader.cpp"
      "${ORCHARD_HOST_ROOT}/src/consensus/chainparams_impl.cpp")
    target_link_libraries(test_orchard_block_reader PRIVATE dinero_orchard_block_reader)
    add_test(NAME OrchardBlockReader COMMAND test_orchard_block_reader "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(OrchardBlockReader PROPERTIES TIMEOUT 120 LABELS "orchard;serialization;integration-staging")
    add_executable(test_orchard_block_staging
      "${ORCHARD_HOST_ROOT}/tests/consensus/test_orchard_block_staging.cpp"
      "${ORCHARD_HOST_ROOT}/src/common/logger.cpp")
    target_link_libraries(test_orchard_block_staging PRIVATE dinero_orchard_block_staging dinero_crypto RocksDB::rocksdb)
    target_include_directories(test_orchard_block_staging PRIVATE "${ORCHARD_HOST_ROOT}/src")
    add_test(NAME OrchardBlockStaging COMMAND test_orchard_block_staging "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME OrchardAtomicCrash COMMAND test_orchard_block_staging --crash-lifecycle "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(OrchardBlockStaging OrchardAtomicCrash PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
      ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;storage;integration-staging")
  endif()
  add_test(NAME OrchardBackendRust COMMAND ${ORCHARD_CARGO} +${ORCHARD_RUST_TOOLCHAIN} test --locked --release --jobs 2
    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" --target-dir "${ORCHARD_TARGET_DIR}"
    -- --test-threads=1)
  set_tests_properties(OrchardBackendCpp OrchardBackendRust OrchardWalletKeys OrchardWalletShield OrchardWalletSpend OrchardWalletStorage OrchardTransactionReader OrchardCoinSnapshot OrchardTransparent OrchardAuthorization OrchardStateTransition PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;integration-staging")
endif()
