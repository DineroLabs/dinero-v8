/**
 * @file test_utreexo_header_size.cpp
 * @brief Consensus-critical test for Utreexo header size enforcement
 *
 * This test ensures 80-byte headers are REJECTED post-Utreexo activation.
 * This is a HARD FORK enforcement test - failure indicates consensus bug.
 */

#include "consensus/header_consensus.h"
#include <cassert>
#include <iostream>

using namespace dinero::consensus;

int main() {
    std::cout << "=================================================\n";
    std::cout << "Utreexo Header Size Consensus Test\n";
    std::cout << "=================================================\n\n";

    // Test 1: Verify 128-byte headers are accepted (v1 blocks)
    {
        std::cout << "Test 1: 128-byte header (valid)..." << std::endl;
        bool valid = IsValidHeaderSize(128, 1, 0);
        assert(valid && "128-byte headers must be valid");
        std::cout << "  ✅ PASS: 128-byte headers accepted\n\n";
    }

    // Test 2: Verify 80-byte headers are REJECTED (consensus-critical)
    {
        std::cout << "Test 2: 80-byte header (MUST REJECT)..." << std::endl;
        bool valid = IsValidHeaderSize(80, 1, 0);
        assert(!valid && "80-byte headers must be REJECTED");
        std::cout << "  ✅ PASS: 80-byte headers rejected\n\n";
    }

    // Test 3: Verify invalid header sizes are rejected
    {
        std::cout << "Test 3: Invalid header size (96 bytes)..." << std::endl;
        bool valid = IsValidHeaderSize(96, 1, 0);
        assert(!valid && "Invalid header sizes must be rejected");
        std::cout << "  ✅ PASS: Invalid sizes rejected\n\n";
    }

    // Test 4: Verify expected header size is 128 bytes
    {
        std::cout << "Test 4: Expected header size..." << std::endl;
        size_t expected = GetExpectedHeaderSize(1, 0);
        assert(expected == 128 && "Expected header size must be 128 bytes");
        std::cout << "  ✅ PASS: Expected size = 128 bytes\n\n";
    }

    // Test 5: Verify PoW coverage enforcement
    {
        std::cout << "Test 5: PoW coverage validation..." << std::endl;

        // PoW must cover full 128 bytes
        bool valid_full = IsValidPoWCoverage(128, 1);
        assert(valid_full && "PoW must cover full 128 bytes");

        // PoW covering only 80 bytes is INVALID
        bool valid_partial = IsValidPoWCoverage(80, 1);
        assert(!valid_partial && "PoW covering only 80 bytes must be invalid");

        std::cout << "  ✅ PASS: PoW coverage enforced\n\n";
    }

    std::cout << "=================================================\n";
    std::cout << "ALL TESTS PASSED ✅\n";
    std::cout << "=================================================\n";
    std::cout << "\nConsensus enforcement verified:\n";
    std::cout << "  - 128-byte headers: ACCEPTED\n";
    std::cout << "  - 80-byte headers:  REJECTED\n";
    std::cout << "  - PoW coverage:     ENFORCED (full 128 bytes)\n\n";
    std::cout << "⚠️  Hard fork active: All blocks MUST use 128-byte format\n\n";

    return 0;
}
