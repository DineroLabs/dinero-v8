// OpenKYC Provider Integration Test
// wallet-core/ffi/test_openkyc_integration.cpp

#include "wallet_ffi.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

// Note: This test uses FFI functions only, no direct logger access
// Logger symbols are provided by dinero_wallet_ffi library

int main(int argc, char* argv[]) {
    std::cout << "=== OpenKYC Provider Integration Test ===" << std::endl;
    std::cout << std::endl;
    
    // Parse command line arguments
    std::string api_url = "http://localhost:8080";
    std::string api_key = "";
    
    if (argc > 1) {
        api_url = argv[1];
    }
    if (argc > 2) {
        api_key = argv[2];
    }
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  API URL: " << api_url << std::endl;
    std::cout << "  API Key: " << (api_key.empty() ? "(none)" : "***") << std::endl;
    std::cout << std::endl;
    
    // Initialize wallet
    std::cout << "1. Initializing wallet..." << std::endl;
    int result = dinero_wallet_init("/tmp/test_wallet_openkyc");
    if (result != 0) {
        std::cerr << "   ❌ Failed to initialize wallet" << std::endl;
        return 1;
    }
    std::cout << "   ✅ Wallet initialized" << std::endl;
    std::cout << std::endl;
    
    // Initialize OpenKYC provider
    std::cout << "2. Initializing OpenKYC provider..." << std::endl;
    std::string config = "api_url=" + api_url;
    if (!api_key.empty()) {
        config += "&api_key=" + api_key;
    }
    
    result = dinero_wallet_init_kyc_provider("openkyc", config.c_str());
    if (result != 0) {
        std::cerr << "   ❌ Failed to initialize OpenKYC provider" << std::endl;
        std::cerr << "   Make sure OpenKYC backend is running at: " << api_url << std::endl;
        return 1;
    }
    std::cout << "   ✅ OpenKYC provider initialized" << std::endl;
    std::cout << std::endl;
    
    // Check provider name
    std::cout << "3. Verifying provider..." << std::endl;
    char* provider_name = nullptr;
    result = dinero_wallet_get_kyc_provider_name(&provider_name);
    if (result != 0 || !provider_name) {
        std::cerr << "   ❌ Failed to get provider name" << std::endl;
        return 1;
    }
    std::cout << "   ✅ Active provider: " << provider_name << std::endl;
    dinero_wallet_free_string(provider_name);
    std::cout << std::endl;
    
    // Get initial status
    std::cout << "4. Checking initial KYC status..." << std::endl;
    FFI_KYCStatus initial_status;
    result = dinero_wallet_get_kyc_status(&initial_status);
    if (result == 0) {
        std::cout << "   Status: " << (initial_status.is_verified ? "Verified" : "Not Verified") << std::endl;
        std::cout << "   Level: " << initial_status.verification_level << std::endl;
        std::cout << "   Provider: " << initial_status.provider << std::endl;
    } else {
        std::cout << "   ⚠️  Could not get status (expected if no user exists)" << std::endl;
    }
    std::cout << std::endl;
    
    // Start verification
    std::cout << "5. Starting KYC verification..." << std::endl;
    char* verification_url = nullptr;
    result = dinero_wallet_start_kyc_verification("basic", "US", &verification_url);
    
    if (result != 0) {
        std::cerr << "   ❌ Failed to start verification" << std::endl;
        std::cerr << "   Error code: " << dinero_wallet_get_last_error() << std::endl;
        
        // Try with mock provider as fallback
        std::cout << "   Trying mock provider as fallback..." << std::endl;
        dinero_wallet_init_kyc_provider("mock", "");
        result = dinero_wallet_start_kyc_verification("basic", "US", &verification_url);
        if (result == 0) {
            std::cout << "   ✅ Mock provider works (OpenKYC backend may not be running)" << std::endl;
        }
        
        return 1;
    }
    
    std::cout << "   ✅ Verification session created" << std::endl;
    std::cout << "   Verification URL: " << verification_url << std::endl;
    std::cout << std::endl;
    std::cout << "   📋 Next steps:" << std::endl;
    std::cout << "      1. Open the URL above in your browser" << std::endl;
    std::cout << "      2. Complete the verification process" << std::endl;
    std::cout << "      3. Press Enter to check status..." << std::endl;
    std::cout << std::endl;
    
    // Wait for user input
    std::cout << "Press Enter to check verification status..." << std::endl;
    std::cin.get();
    
    // Check status multiple times
    std::cout << "6. Checking verification status..." << std::endl;
    for (int i = 0; i < 5; i++) {
        FFI_KYCStatus status;
        result = dinero_wallet_get_kyc_status(&status);
        
        if (result == 0) {
            std::cout << "   Attempt " << (i + 1) << ":" << std::endl;
            std::cout << "     Verified: " << (status.is_verified ? "✅ Yes" : "❌ No") << std::endl;
            std::cout << "     Level: " << status.verification_level << std::endl;
            std::cout << "     Provider: " << status.provider << std::endl;
            std::cout << "     Country: " << status.country << std::endl;
            
            if (status.is_verified) {
                std::cout << std::endl;
                std::cout << "   🎉 Verification successful!" << std::endl;
                dinero_wallet_free_string(verification_url);
                return 0;
            }
        } else {
            std::cout << "   Attempt " << (i + 1) << ": Status check failed" << std::endl;
        }
        
        if (i < 4) {
            std::cout << "   Waiting 3 seconds before next check..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    
    std::cout << std::endl;
    std::cout << "   ⏳ Verification still pending or failed" << std::endl;
    std::cout << "   Check OpenKYC backend logs for details" << std::endl;
    
    dinero_wallet_free_string(verification_url);
    return 0;
}

