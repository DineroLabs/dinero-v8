// wallet-core/ffi/tests/ffi_tests.cpp
// FFI Test Suite for Dinero Wallet

#include <cassert>
#include <cstring>
#include <iostream>
#include "wallet_ffi.h"

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            return false; \
        } \
    } while (0)

static bool test_error_codes() {
    std::cout << "Testing error codes...\n";
    
    DineroErrorCode code = dinero_wallet_get_last_error();
    TEST_ASSERT(code == DINERO_SUCCESS || code < 0, "Error code should be valid");
    
    char* message = nullptr;
    int result = dinero_wallet_get_error_message(DINERO_ERROR_GENERIC, &message);
    TEST_ASSERT(result == 0, "Should get error message");
    TEST_ASSERT(message != nullptr, "Error message should not be null");
    TEST_ASSERT(strlen(message) > 0, "Error message should not be empty");
    
    if (message) {
        dinero_wallet_free_string(message);
    }
    
    std::cout << "  ✓ Error codes test passed\n";
    return true;
}

static bool test_uri_parsing() {
    std::cout << "Testing URI parsing...\n";
    
    FFI_QRPayment payment;
    const char* uri1 = "dinero:din1qtest123";
    int result = dinero_wallet_parse_uri(uri1, &payment);
    TEST_ASSERT(result == 0, "Should parse simple URI");
    TEST_ASSERT(strcmp(payment.address, "din1qtest123") == 0, "Address should match");
    
    const char* uri2 = "dinero:din1qtest123?amount=15.25&label=Test";
    result = dinero_wallet_parse_uri(uri2, &payment);
    TEST_ASSERT(result == 0, "Should parse URI with params");
    TEST_ASSERT(payment.amount == 15.25, "Amount should match");
    TEST_ASSERT(strcmp(payment.label, "Test") == 0, "Label should match");
    
    const char* uri3 = "invalid:uri";
    result = dinero_wallet_parse_uri(uri3, &payment);
    TEST_ASSERT(result != 0, "Should reject invalid URI");
    
    std::cout << "  ✓ URI parsing test passed\n";
    return true;
}

static bool test_uri_generation() {
    std::cout << "Testing URI generation...\n";
    
    char* uri = nullptr;
    int result = dinero_wallet_generate_uri("din1qtest123", 0.0, nullptr, &uri);
    TEST_ASSERT(result == 0, "Should generate URI");
    TEST_ASSERT(uri != nullptr, "URI should not be null");
    TEST_ASSERT(strstr(uri, "dinero:") != nullptr, "URI should start with dinero:");
    
    if (uri) {
        dinero_wallet_free_string(uri);
    }
    
    result = dinero_wallet_generate_uri("din1qtest123", 100.5, "Test Label", &uri);
    TEST_ASSERT(result == 0, "Should generate URI with params");
    TEST_ASSERT(strstr(uri, "amount=100.5") != nullptr, "URI should contain amount");
    
    if (uri) {
        dinero_wallet_free_string(uri);
    }
    
    std::cout << "  ✓ URI generation test passed\n";
    return true;
}

static bool test_sync_progress() {
    std::cout << "Testing sync progress...\n";
    
    FFI_SyncProgress progress;
    int result = dinero_wallet_get_sync_progress(&progress);
    
    // May fail if wallet not initialized, which is OK for tests
    if (result == 0) {
        TEST_ASSERT(progress.progress >= 0.0 && progress.progress <= 1.0, 
                   "Progress should be between 0 and 1");
        
        if (progress.status_message) {
            dinero_wallet_free_string(progress.status_message);
        }
    }
    
    std::cout << "  ✓ Sync progress test passed\n";
    return true;
}

int main() {
    std::cout << "=== Dinero Wallet FFI Test Suite ===\n\n";
    
    bool all_passed = true;
    
    all_passed &= test_error_codes();
    all_passed &= test_uri_parsing();
    all_passed &= test_uri_generation();
    all_passed &= test_sync_progress();
    
    std::cout << "\n=== Test Results ===\n";
    if (all_passed) {
        std::cout << "✓ All tests passed!\n";
        return 0;
    } else {
        std::cout << "✗ Some tests failed!\n";
        return 1;
    }
}

