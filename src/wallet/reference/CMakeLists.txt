# Reference wallet CLI and offline test wiring.
#
# This file is included from the repository root so paths remain root-relative
# and root-scoped helper functions/variables stay available.

set(DINERO_REFERENCE_WALLET_SOURCES
  src/wallet/reference/wallet.cpp
  src/wallet/reference/database.cpp
  src/wallet/reference/crypto.cpp
  src/wallet/reference/utxo_manager.cpp
  src/wallet/reference/transaction_builder.cpp
  src/wallet/reference/blockchain_sync.cpp
  src/wallet/bip39.cpp
  src/crypto/pbkdf2.cpp
  src/common/rpc_client.cpp
  external/bech32/bech32.cpp
)

set(DINERO_REFERENCE_WALLET_LIBS
  dinero_crypto
  sqlite3
  lightning_core_static
  ${DINERO_CURL_TARGET}
  jsoncpp_static
  OpenSSL::Crypto
)

add_executable(dinero-wallet-cli
  src/wallet/reference/main.cpp
  ${DINERO_REFERENCE_WALLET_SOURCES}
)

target_link_libraries(dinero-wallet-cli PRIVATE
  ${DINERO_REFERENCE_WALLET_LIBS}
)
link_secp256k1_zkp(dinero-wallet-cli)
if(APPLE)
  target_link_libraries(dinero-wallet-cli PRIVATE "-framework Security")
endif()
target_include_directories(dinero-wallet-cli PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)

dinero_attach_win32_versioninfo(dinero-wallet-cli "Dinero reference wallet CLI")

# Reference Wallet Test Suite
# PURE OFFLINE TESTS: No RPC calls in tests, but wallet.cpp includes optional sync method.
add_executable(test_reference_wallet
  tests/wallet/test_reference_wallet.cpp
  ${DINERO_REFERENCE_WALLET_SOURCES}
)

add_dependencies(test_reference_wallet gtest)

target_link_libraries(test_reference_wallet PRIVATE
  ${DINERO_REFERENCE_WALLET_LIBS}
  gtest
)
link_secp256k1_zkp(test_reference_wallet)

if(APPLE)
  target_link_libraries(test_reference_wallet PRIVATE "-framework Security")
endif()

target_include_directories(test_reference_wallet PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
)

add_test(NAME ReferenceWallet COMMAND test_reference_wallet)
set_tests_properties(ReferenceWallet PROPERTIES
  LABELS "wallet;reference;critical;determinism"
  TIMEOUT 60
)

message(STATUS "✅ Reference wallet test suite enabled")
