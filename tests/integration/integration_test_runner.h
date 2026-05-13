/**
 * IntegrationTestRunner - Orchestrate Layer 1-2 Integration Tests
 *
 * Purpose:
 * - Run test scenarios with real ChainDB, real blocks, real UTXOSet
 * - Track results (pass/fail, duration, error messages)
 * - Provide clean lifecycle (setup, execute, teardown)
 * - Report summary statistics
 *
 * Capabilities:
 * - Individual test registration
 * - Exception handling (test failures don't crash runner)
 * - Detailed error reporting
 * - Timing measurements
 * - Summary dashboard
 *
 * Usage Pattern:
 *   IntegrationTestRunner runner;
 *
 *   runner.RunTest("DisconnectBlock simple", []() {
 *       // Test implementation
 *       return true;  // or throw on failure
 *   });
 *
 *   runner.PrintSummary();
 *   return runner.AllTestsPassed() ? 0 : 1;
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <exception>

namespace dinero {
namespace test {

/**
 * Result of a single integration test
 */
struct IntegrationTestResult {
    std::string test_name;
    bool passed;
    std::string error_message;
    std::chrono::milliseconds duration;

    IntegrationTestResult(const std::string& name)
        : test_name(name)
        , passed(false)
        , duration(0)
    {}

    /**
     * Print individual test result
     */
    void Print() const {
        std::string status = passed ? "✅ PASS" : "❌ FAIL";
        std::cout << status << " | "
                  << std::setw(50) << std::left << test_name
                  << " | " << std::setw(8) << std::right << duration.count() << " ms";

        if (!passed && !error_message.empty()) {
            std::cout << "\n       Error: " << error_message;
        }

        std::cout << "\n";
    }
};

/**
 * Test function signature
 * - Returns true on success
 * - Throws std::runtime_error on failure (or other exceptions)
 */
using TestFunction = std::function<bool()>;

/**
 * Test runner for Layer 1-2 integration tests
 */
class IntegrationTestRunner {
public:
    IntegrationTestRunner()
        : total_duration_(0)
    {}

    /**
     * Run a single test and record result
     *
     * @param test_name Descriptive test name
     * @param test_func Test function (returns true on success, throws on failure)
     *
     * Exception handling:
     * - Catches all exceptions
     * - Records as test failure with error message
     * - Does not propagate (allows other tests to run)
     */
    void RunTest(const std::string& test_name, TestFunction test_func) {
        IntegrationTestResult result(test_name);

        std::cout << "Running: " << test_name << " ... " << std::flush;

        auto start = std::chrono::high_resolution_clock::now();

        try {
            bool success = test_func();
            result.passed = success;

            if (!success) {
                result.error_message = "Test returned false (no exception)";
            }
        }
        catch (const std::exception& e) {
            result.passed = false;
            result.error_message = e.what();
        }
        catch (...) {
            result.passed = false;
            result.error_message = "Unknown exception";
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        total_duration_ += result.duration;

        std::cout << (result.passed ? "✅ PASS" : "❌ FAIL")
                  << " (" << result.duration.count() << " ms)\n";

        if (!result.passed && !result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << "\n";
        }

        results_.push_back(result);
    }

    /**
     * Print detailed summary of all test results
     */
    void PrintSummary() const {
        std::cout << "\n";
        std::cout << "========================================\n";
        std::cout << "  INTEGRATION TEST SUMMARY\n";
        std::cout << "========================================\n";

        // Statistics
        size_t total = results_.size();
        size_t passed = 0;
        size_t failed = 0;

        for (const auto& result : results_) {
            if (result.passed) {
                passed++;
            } else {
                failed++;
            }
        }

        std::cout << "Tests Run:     " << total << "\n";
        std::cout << "Tests Passed:  " << passed << " ✅\n";
        std::cout << "Tests Failed:  " << failed;
        if (failed > 0) {
            std::cout << " ❌";
        }
        std::cout << "\n";
        std::cout << "Total Time:    " << total_duration_.count() << " ms\n";
        std::cout << "========================================\n\n";

        // Individual results
        if (!results_.empty()) {
            std::cout << "Individual Results:\n";
            std::cout << "----------------------------------------\n";
            for (const auto& result : results_) {
                result.Print();
            }
            std::cout << "========================================\n";
        }

        // Final verdict
        if (failed == 0 && total > 0) {
            std::cout << "\n🎉 ALL TESTS PASSED\n\n";
        } else if (failed > 0) {
            std::cout << "\n❌ SOME TESTS FAILED\n\n";
        } else {
            std::cout << "\n⚠️  NO TESTS RUN\n\n";
        }
    }

    /**
     * Print compact summary (single line)
     */
    void PrintCompactSummary() const {
        size_t total = results_.size();
        size_t passed = 0;
        for (const auto& result : results_) {
            if (result.passed) passed++;
        }

        std::cout << "Tests: " << passed << "/" << total << " passed";
        if (passed == total && total > 0) {
            std::cout << " ✅";
        } else if (passed < total) {
            std::cout << " ❌";
        }
        std::cout << " (" << total_duration_.count() << " ms)\n";
    }

    /**
     * Check if all tests passed
     *
     * @return true if all tests passed (and at least one test ran)
     */
    bool AllTestsPassed() const {
        if (results_.empty()) {
            return false;  // No tests run = failure
        }

        for (const auto& result : results_) {
            if (!result.passed) {
                return false;
            }
        }

        return true;
    }

    /**
     * Get number of tests run
     *
     * @return Total number of tests executed
     */
    size_t GetTestCount() const {
        return results_.size();
    }

    /**
     * Get number of tests passed
     *
     * @return Number of successful tests
     */
    size_t GetPassedCount() const {
        size_t count = 0;
        for (const auto& result : results_) {
            if (result.passed) count++;
        }
        return count;
    }

    /**
     * Get number of tests failed
     *
     * @return Number of failed tests
     */
    size_t GetFailedCount() const {
        return results_.size() - GetPassedCount();
    }

    /**
     * Get total duration of all tests
     *
     * @return Total execution time
     */
    std::chrono::milliseconds GetTotalDuration() const {
        return total_duration_;
    }

    /**
     * Get all test results (for custom reporting)
     *
     * @return Vector of all test results
     */
    const std::vector<IntegrationTestResult>& GetResults() const {
        return results_;
    }

    /**
     * Clear all test results (reset runner)
     *
     * Use case: Run multiple test suites with same runner
     */
    void Clear() {
        results_.clear();
        total_duration_ = std::chrono::milliseconds(0);
    }

private:
    std::vector<IntegrationTestResult> results_;
    std::chrono::milliseconds total_duration_;
};

/**
 * RAII helper for test section timing
 *
 * Usage:
 *   {
 *       TestTimer timer("Block connection");
 *       // ... operations ...
 *   }  // Prints "Block connection: 123 ms"
 */
class TestTimer {
public:
    explicit TestTimer(const std::string& section_name)
        : section_name_(section_name)
        , start_(std::chrono::high_resolution_clock::now())
    {}

    ~TestTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);
        std::cout << "  [" << section_name_ << ": " << duration.count() << " ms]\n";
    }

private:
    std::string section_name_;
    std::chrono::high_resolution_clock::time_point start_;
};

/**
 * Assertion helpers for integration tests
 */
#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            throw std::runtime_error( \
                "Assertion failed: " #condition \
                " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__) \
            ); \
        } \
    } while (0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (_a != _b) { \
            throw std::runtime_error( \
                "Assertion failed: " #a " == " #b \
                " (values: " + std::to_string(_a) + " != " + std::to_string(_b) + ")" \
                " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__) \
            ); \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (_a == _b) { \
            throw std::runtime_error( \
                "Assertion failed: " #a " != " #b \
                " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__) \
            ); \
        } \
    } while (0)

} // namespace test
} // namespace dinero
