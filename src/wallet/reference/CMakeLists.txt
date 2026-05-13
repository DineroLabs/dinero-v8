# DineroCoin Reference Wallet

cmake_minimum_required(VERSION 3.15)

# Reference Wallet Library
add_library(dinero_reference_wallet STATIC
    wallet.cpp
    wallet.h
    database.cpp
    database.h
    crypto.cpp
    crypto.h
    utxo_manager.cpp
    utxo_manager.h
    transaction_builder.cpp
    transaction_builder.h
)

target_include_directories(dinero_reference_wallet PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dinero_reference_wallet
    sqlite3
    secp256k1
    dinero_crypto
)

# Reference Wallet CLI
add_executable(dinero-wallet-cli
    main.cpp
)

target_link_libraries(dinero-wallet-cli
    dinero_reference_wallet
)

# Install targets
install(TARGETS dinero-wallet-cli
    RUNTIME DESTINATION bin
)
