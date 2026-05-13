// CMakeLists.txt addition for FFI tests
// Add this to wallet-core/ffi/CMakeLists.txt or main CMakeLists.txt

# FFI Test Suite
if(ENABLE_TESTS)
    add_executable(ffi_tests
        wallet-core/ffi/tests/ffi_tests.cpp
    )
    
    target_link_libraries(ffi_tests PRIVATE
        dinero_wallet_ffi
        dinero_wallet
        dinero_crypto
        dinero_consensus
    )
    
    target_include_directories(ffi_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/wallet-core/ffi
        ${CMAKE_SOURCE_DIR}/include
    )
    
    add_test(NAME FFITests COMMAND ffi_tests)
endif()

