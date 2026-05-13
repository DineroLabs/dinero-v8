// SPDX-License-Identifier: MIT
// Phase W.4.4: Fee Bump RPC Handler Tests

#include "rpc/fee_bump_rpc_handlers.h"
#include "daemon/execution_context.h"
#include "din_json.h"
#include <iostream>
#include <cassert>

using namespace dinero;

// ============================================================================
// Test Utilities
// ============================================================================

#define ASSERT_TRUE(expr, msg) \
    if (!(expr)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expression: " << #expr << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_FALSE(expr, msg) \
    if ((expr)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expression: !" << #expr << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_EQ(a, b, msg) \
    if ((a) != (b)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: " << (b) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

// ============================================================================
// Test 1: wallet.gettxinclusion - Missing Parameter
// ============================================================================

bool test_w4_4_gettxinclusion_missing_param() {
    std::cout << "\n[Test 1] W.4.4: wallet.gettxinclusion missing parameter" << std::endl;

    Json::Value params(Json::arrayValue);
    // Empty params - should throw

    try {
        wallet_gettxinclusion(params);
        ASSERT_TRUE(false, "Should have thrown exception for missing parameter");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("Missing required parameter") != std::string::npos,
                    "Error should mention missing parameter");
    }

    std::cout << "✅ Test 1 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 2: wallet.gettxinclusion - Invalid Parameter Type
// ============================================================================

bool test_w4_4_gettxinclusion_invalid_type() {
    std::cout << "\n[Test 2] W.4.4: wallet.gettxinclusion invalid parameter type" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append(123);  // Number instead of string

    try {
        wallet_gettxinclusion(params);
        ASSERT_TRUE(false, "Should have thrown exception for invalid type");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("must be a string") != std::string::npos,
                    "Error should mention type mismatch");
    }

    std::cout << "✅ Test 2 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 3: wallet.gettxinclusion - Valid Call (No Context)
// ============================================================================

bool test_w4_4_gettxinclusion_valid() {
    std::cout << "\n[Test 3] W.4.4: wallet.gettxinclusion valid call" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append("0000000000000000000000000000000000000000000000000000000000000001");

    Json::Value result = wallet_gettxinclusion(params);

    // Verify result structure
    ASSERT_TRUE(result.isMember("txid"), "Result should have txid");
    ASSERT_TRUE(result.isMember("state"), "Result should have state");
    ASSERT_TRUE(result.isMember("primary_reason"), "Result should have primary_reason");
    ASSERT_TRUE(result.isMember("estimated_inclusion_prob"), "Result should have probability");
    ASSERT_TRUE(result.isMember("effective_feerate"), "Result should have effective_feerate");
    ASSERT_TRUE(result.isMember("cutoff_feerate"), "Result should have cutoff_feerate");
    ASSERT_TRUE(result.isMember("rbf_available"), "Result should have rbf_available");
    ASSERT_TRUE(result.isMember("cpfp_available"), "Result should have cpfp_available");
    ASSERT_TRUE(result.isMember("explanation"), "Result should have explanation");
    ASSERT_TRUE(result.isMember("timestamp_ms"), "Result should have timestamp");

    // Verify types
    ASSERT_TRUE(result["txid"].isString(), "txid should be string");
    ASSERT_TRUE(result["state"].isString(), "state should be string");
    ASSERT_TRUE(result["rbf_available"].isBool(), "rbf_available should be bool");
    ASSERT_TRUE(result["cpfp_available"].isBool(), "cpfp_available should be bool");

    std::cout << "✅ Test 3 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 4: wallet.gettxbumprecommendation - Missing Parameter
// ============================================================================

bool test_w4_4_gettxbumprecommendation_missing_param() {
    std::cout << "\n[Test 4] W.4.4: wallet.gettxbumprecommendation missing parameter" << std::endl;

    Json::Value params(Json::arrayValue);
    // Empty params - should throw

    try {
        wallet_gettxbumprecommendation(params);
        ASSERT_TRUE(false, "Should have thrown exception for missing parameter");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("Missing required parameter") != std::string::npos,
                    "Error should mention missing parameter");
    }

    std::cout << "✅ Test 4 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 5: wallet.gettxbumprecommendation - Invalid Txid Type
// ============================================================================

bool test_w4_4_gettxbumprecommendation_invalid_txid() {
    std::cout << "\n[Test 5] W.4.4: wallet.gettxbumprecommendation invalid txid type" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append(123);  // Number instead of string

    try {
        wallet_gettxbumprecommendation(params);
        ASSERT_TRUE(false, "Should have thrown exception for invalid type");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("must be a string") != std::string::npos,
                    "Error should mention type mismatch");
    }

    std::cout << "✅ Test 5 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 6: wallet.gettxbumprecommendation - Invalid Target Blocks
// ============================================================================

bool test_w4_4_gettxbumprecommendation_invalid_target() {
    std::cout << "\n[Test 6] W.4.4: wallet.gettxbumprecommendation invalid target_blocks" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append("0000000000000000000000000000000000000000000000000000000000000001");
    params.append(0);  // Invalid: target_blocks must be > 0

    try {
        wallet_gettxbumprecommendation(params);
        ASSERT_TRUE(false, "Should have thrown exception for invalid target_blocks");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("must be > 0") != std::string::npos,
                    "Error should mention target_blocks constraint");
    }

    std::cout << "✅ Test 6 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 7: wallet.gettxbumprecommendation - Target Blocks Too Large
// ============================================================================

bool test_w4_4_gettxbumprecommendation_target_too_large() {
    std::cout << "\n[Test 7] W.4.4: wallet.gettxbumprecommendation target_blocks too large" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append("0000000000000000000000000000000000000000000000000000000000000001");
    params.append(2000);  // Too large: > 1008

    try {
        wallet_gettxbumprecommendation(params);
        ASSERT_TRUE(false, "Should have thrown exception for target_blocks too large");
    } catch (const std::runtime_error& e) {
        std::string error_msg(e.what());
        ASSERT_TRUE(error_msg.find("must be <= 1008") != std::string::npos,
                    "Error should mention max target_blocks");
    }

    std::cout << "✅ Test 7 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 8: wallet.gettxbumprecommendation - Valid Call (No Context)
// ============================================================================

bool test_w4_4_gettxbumprecommendation_valid() {
    std::cout << "\n[Test 8] W.4.4: wallet.gettxbumprecommendation valid call" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append("0000000000000000000000000000000000000000000000000000000000000001");

    Json::Value result = wallet_gettxbumprecommendation(params);

    // Verify result structure
    ASSERT_TRUE(result.isMember("txid"), "Result should have txid");
    ASSERT_TRUE(result.isMember("strategy"), "Result should have strategy");
    ASSERT_TRUE(result.isMember("rationale"), "Result should have rationale");
    ASSERT_TRUE(result.isMember("current_feerate"), "Result should have current_feerate");
    ASSERT_TRUE(result.isMember("target_feerate"), "Result should have target_feerate");
    ASSERT_TRUE(result.isMember("mempool_min_feerate"), "Result should have mempool_min_feerate");
    ASSERT_TRUE(result.isMember("estimated_blocks_current"), "Result should have estimated_blocks_current");
    ASSERT_TRUE(result.isMember("estimated_blocks_target"), "Result should have estimated_blocks_target");
    ASSERT_TRUE(result.isMember("warnings"), "Result should have warnings");
    ASSERT_TRUE(result.isMember("timestamp_ms"), "Result should have timestamp");

    // Verify types
    ASSERT_TRUE(result["txid"].isString(), "txid should be string");
    ASSERT_TRUE(result["strategy"].isString(), "strategy should be string");
    ASSERT_TRUE(result["warnings"].isArray(), "warnings should be array");

    std::cout << "✅ Test 8 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 9: wallet.gettxbumprecommendation - With Target Blocks
// ============================================================================

bool test_w4_4_gettxbumprecommendation_with_target() {
    std::cout << "\n[Test 9] W.4.4: wallet.gettxbumprecommendation with target_blocks" << std::endl;

    Json::Value params(Json::arrayValue);
    params.append("0000000000000000000000000000000000000000000000000000000000000001");
    params.append(6);  // Target 6 blocks

    Json::Value result = wallet_gettxbumprecommendation(params);

    // Verify basic structure
    ASSERT_TRUE(result.isMember("txid"), "Result should have txid");
    ASSERT_TRUE(result.isMember("strategy"), "Result should have strategy");
    ASSERT_TRUE(result.isMember("estimated_blocks_target"), "Result should have estimated_blocks_target");

    // Verify target blocks is reflected
    ASSERT_EQ(result["estimated_blocks_target"].asUInt(), 6U,
              "estimated_blocks_target should match input");

    std::cout << "✅ Test 9 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 10: RPC Registration
// ============================================================================

bool test_w4_4_rpc_registration() {
    std::cout << "\n[Test 10] W.4.4: RPC handler registration" << std::endl;

    // Test that registration function exists and doesn't crash
    try {
        RegisterFeeBumpRpcHandlers();
        std::cout << "RPC handlers registered successfully" << std::endl;
    } catch (const std::exception& e) {
        ASSERT_TRUE(false, std::string("Registration failed: ") + e.what());
    }

    std::cout << "✅ Test 10 passed" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase W.4.4: Fee Bump RPC Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    // Run all tests
    all_passed &= test_w4_4_gettxinclusion_missing_param();
    all_passed &= test_w4_4_gettxinclusion_invalid_type();
    all_passed &= test_w4_4_gettxinclusion_valid();
    all_passed &= test_w4_4_gettxbumprecommendation_missing_param();
    all_passed &= test_w4_4_gettxbumprecommendation_invalid_txid();
    all_passed &= test_w4_4_gettxbumprecommendation_invalid_target();
    all_passed &= test_w4_4_gettxbumprecommendation_target_too_large();
    all_passed &= test_w4_4_gettxbumprecommendation_valid();
    all_passed &= test_w4_4_gettxbumprecommendation_with_target();
    all_passed &= test_w4_4_rpc_registration();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "❌ SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
