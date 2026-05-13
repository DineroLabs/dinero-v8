/**
 * Phase G.8: DNS Resolution & Network Bootstrap Tests
 *
 * Test Coverage:
 * - G.8.1: IP address detection (IPv4 and IPv6)
 * - G.8.2: Direct IP address resolution (bypass DNS)
 * - G.8.3: Localhost resolution
 * - G.8.4: Multiple seed resolution
 * - G.8.5: Result limits and max_results
 * - G.8.6: Invalid hostname handling
 */

#include "p2p/dns_resolver.h"
#include <iostream>
#include <cassert>

using namespace dinero::p2p;

// ============================================================================
// Test G.8.1: IP Address Detection
// ============================================================================

void test_g8_1_ip_detection() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.1: IP Address Detection" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Test IPv4 addresses
    std::cout << "Testing IPv4 addresses..." << std::endl;
    assert(DNSResolver::isIPAddress("192.168.1.1"));
    assert(DNSResolver::isIPAddress("10.0.0.1"));
    assert(DNSResolver::isIPAddress("127.0.0.1"));
    assert(DNSResolver::isIPAddress("8.8.8.8"));
    assert(DNSResolver::isIPAddress("203.0.113.1"));
    std::cout << "✅ IPv4 detection working\n" << std::endl;

    // Test IPv6 addresses
    std::cout << "Testing IPv6 addresses..." << std::endl;
    assert(DNSResolver::isIPAddress("::1"));
    assert(DNSResolver::isIPAddress("2001:db8::1"));
    assert(DNSResolver::isIPAddress("fe80::1"));
    assert(DNSResolver::isIPAddress("2001:4860:4860::8888"));
    std::cout << "✅ IPv6 detection working\n" << std::endl;

    // Test non-IP strings (hostnames)
    std::cout << "Testing non-IP strings..." << std::endl;
    assert(!DNSResolver::isIPAddress("localhost"));
    assert(!DNSResolver::isIPAddress("example.com"));
    assert(!DNSResolver::isIPAddress("seed.dinero-coin.com"));
    assert(!DNSResolver::isIPAddress("not-an-ip"));
    assert(!DNSResolver::isIPAddress(""));
    std::cout << "✅ Hostname detection working\n" << std::endl;

    std::cout << "✅ Test G.8.1 PASSED: IP address detection working\n" << std::endl;
}

// ============================================================================
// Test G.8.2: Direct IP Address Resolution
// ============================================================================

void test_g8_2_direct_ip_resolution() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.2: Direct IP Resolution" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // IPv4 address should be returned directly
    std::cout << "Resolving IPv4 address directly..." << std::endl;
    auto ipv4_results = DNSResolver::resolve("192.168.1.100", 8333);

    assert(ipv4_results.size() == 1);
    assert(ipv4_results[0].ip == "192.168.1.100");
    assert(ipv4_results[0].port == 8333);
    assert(ipv4_results[0].is_ipv6 == false);

    std::cout << "  Resolved: " << ipv4_results[0].to_string() << std::endl;
    std::cout << "✅ IPv4 direct resolution working\n" << std::endl;

    // IPv6 address should be returned directly
    std::cout << "Resolving IPv6 address directly..." << std::endl;
    auto ipv6_results = DNSResolver::resolve("2001:db8::1", 8333);

    assert(ipv6_results.size() == 1);
    assert(ipv6_results[0].ip == "2001:db8::1");
    assert(ipv6_results[0].port == 8333);
    assert(ipv6_results[0].is_ipv6 == true);

    std::cout << "  Resolved: " << ipv6_results[0].to_string() << std::endl;
    std::cout << "✅ IPv6 direct resolution working\n" << std::endl;

    std::cout << "✅ Test G.8.2 PASSED: Direct IP resolution working\n" << std::endl;
}

// ============================================================================
// Test G.8.3: Localhost Resolution
// ============================================================================

void test_g8_3_localhost_resolution() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.3: Localhost Resolution" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Resolve "localhost" - should return 127.0.0.1 and/or ::1
    std::cout << "Resolving 'localhost'..." << std::endl;
    auto results = DNSResolver::resolve("localhost", 21000);

    std::cout << "  Resolved " << results.size() << " address(es):" << std::endl;
    for (const auto& addr : results) {
        std::cout << "    - " << addr.to_string()
                  << " (" << (addr.is_ipv6 ? "IPv6" : "IPv4") << ")" << std::endl;
    }

    // Should get at least one result (127.0.0.1 or ::1)
    assert(results.size() >= 1);

    // Check that we got valid localhost addresses
    bool found_ipv4 = false;
    bool found_ipv6 = false;

    for (const auto& addr : results) {
        if (addr.ip == "127.0.0.1") {
            found_ipv4 = true;
            assert(addr.is_ipv6 == false);
        }
        if (addr.ip == "::1") {
            found_ipv6 = true;
            assert(addr.is_ipv6 == true);
        }
        assert(addr.port == 21000);
    }

    // Should have at least one of IPv4 or IPv6 localhost
    assert(found_ipv4 || found_ipv6);
    std::cout << "✅ Localhost resolution working\n" << std::endl;

    std::cout << "✅ Test G.8.3 PASSED: Localhost resolution working\n" << std::endl;
}

// ============================================================================
// Test G.8.4: Multiple Seed Resolution
// ============================================================================

void test_g8_4_seed_resolution() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.4: Multiple Seed Resolution" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Test with mix of IP addresses and localhost
    std::vector<std::string> seeds = {
        "127.0.0.1",
        "::1",
        "localhost"
    };

    std::cout << "Resolving " << seeds.size() << " seeds..." << std::endl;
    auto results = DNSResolver::resolveSeeds(seeds, 8333, 4);

    std::cout << "  Total resolved: " << results.size() << " addresses\n" << std::endl;

    // Should get results for all seeds
    assert(results.size() >= 3);  // At least 3 (could be more if localhost resolves to both IPv4 and IPv6)

    // Verify all have correct port
    for (const auto& addr : results) {
        assert(addr.port == 8333);
        std::cout << "    - " << addr.to_string()
                  << " (" << (addr.is_ipv6 ? "IPv6" : "IPv4") << ")" << std::endl;
    }

    std::cout << "✅ Seed resolution working\n" << std::endl;

    std::cout << "✅ Test G.8.4 PASSED: Multiple seed resolution working\n" << std::endl;
}

// ============================================================================
// Test G.8.5: Result Limits
// ============================================================================

void test_g8_5_result_limits() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.5: Result Limits" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Test max_results limit
    std::cout << "Testing max_results limit with localhost..." << std::endl;
    auto results_limited = DNSResolver::resolve("localhost", 8333, 5000, 1);

    std::cout << "  Requested max 1, got: " << results_limited.size() << std::endl;
    assert(results_limited.size() <= 1);
    std::cout << "✅ max_results limit enforced\n" << std::endl;

    // Test with larger limit
    auto results_unlimited = DNSResolver::resolve("localhost", 8333, 5000, 10);
    std::cout << "  Requested max 10, got: " << results_unlimited.size() << std::endl;
    assert(results_unlimited.size() <= 10);
    std::cout << "✅ Larger limits work correctly\n" << std::endl;

    std::cout << "✅ Test G.8.5 PASSED: Result limits working\n" << std::endl;
}

// ============================================================================
// Test G.8.6: Invalid Hostname Handling
// ============================================================================

void test_g8_6_invalid_hostname() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.8.6: Invalid Hostname Handling" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Test with invalid/non-existent hostname
    std::cout << "Testing invalid hostname resolution..." << std::endl;
    auto results = DNSResolver::resolve("this-host-definitely-does-not-exist-12345.invalid", 8333);

    std::cout << "  Results for invalid hostname: " << results.size() << std::endl;

    // Should return empty results for invalid hostname
    assert(results.empty());
    std::cout << "✅ Invalid hostname returns empty results\n" << std::endl;

    // Test with empty string
    std::cout << "Testing empty hostname..." << std::endl;
    auto results_empty = DNSResolver::resolve("", 8333);
    std::cout << "  Results for empty hostname: " << results_empty.size() << std::endl;
    assert(results_empty.empty());
    std::cout << "✅ Empty hostname returns empty results\n" << std::endl;

    std::cout << "✅ Test G.8.6 PASSED: Invalid hostname handling working\n" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase G.8: DNS Resolver Tests       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

    try {
        test_g8_1_ip_detection();
        test_g8_2_direct_ip_resolution();
        test_g8_3_localhost_resolution();
        test_g8_4_seed_resolution();
        test_g8_5_result_limits();
        test_g8_6_invalid_hostname();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
