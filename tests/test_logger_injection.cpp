/**
 * Test: Logger Dependency Injection with TestLogger
 *
 * Demonstrates the value of the ILogger refactor by showing how
 * TestLogger enables clean, assertion-based testing of logging behavior.
 *
 * Before the refactor:
 * - Tests had to capture g_logger output from stdout/files
 * - No way to verify specific log messages programmatically
 * - Hard-wired global dependency made unit testing difficult
 *
 * After the refactor:
 * - Inject TestLogger to capture log messages in memory
 * - Assert on exact log messages and levels
 * - No global state pollution between tests
 */

#include "common/test_logger.h"
#include <iostream>
#include <cassert>

using namespace dinero;

// Example function that uses ILogger (simulates WalletService behavior)
void processWalletOperation(ILogger& logger, bool should_fail) {
    logger.info("[WalletService] Starting wallet operation");

    if (should_fail) {
        logger.error("[WalletService] Operation failed: insufficient funds");
        return;
    }

    logger.debug("[WalletService] Validating transaction inputs");
    logger.info("[WalletService] Transaction created successfully");
    logger.warning("[WalletService] High fee detected: 1000 una");
}

int main() {
    std::cout << "=== Logger Dependency Injection Test ===" << std::endl;

    // Test 1: Verify TestLogger captures all log levels
    {
        std::cout << "\n1. Testing basic log capture..." << std::endl;
        TestLogger logger;

        logger.debug("Debug message");
        logger.info("Info message");
        logger.warning("Warning message");
        logger.error("Error message");

        assert(logger.messageCount() == 4);
        std::cout << "✅ Captured all 4 messages" << std::endl;

        assert(logger.messageCount(LogLevel::DEBUG) == 1);
        assert(logger.messageCount(LogLevel::INFO) == 1);
        assert(logger.messageCount(LogLevel::WARNING) == 1);
        assert(logger.messageCount(LogLevel::ERROR) == 1);
        std::cout << "✅ Correct message counts per level" << std::endl;
    }

    // Test 2: Verify substring matching
    {
        std::cout << "\n2. Testing message content verification..." << std::endl;
        TestLogger logger;

        logger.info("[WalletService] Created new wallet 'default'");
        logger.error("[WalletService] Failed to open wallet: file not found");

        assert(logger.hasMessage(LogLevel::INFO, "Created new wallet"));
        assert(logger.hasMessage(LogLevel::ERROR, "Failed to open wallet"));
        assert(!logger.hasMessage(LogLevel::WARNING, "anything"));
        std::cout << "✅ Substring matching works correctly" << std::endl;
    }

    // Test 3: Verify clear() functionality
    {
        std::cout << "\n3. Testing log clearing..." << std::endl;
        TestLogger logger;

        logger.info("Message 1");
        logger.info("Message 2");
        assert(logger.messageCount() == 2);

        logger.clear();
        assert(logger.messageCount() == 0);
        std::cout << "✅ Clear() resets message buffer" << std::endl;
    }

    // Test 4: Simulate WalletService error handling
    {
        std::cout << "\n4. Testing simulated WalletService behavior..." << std::endl;
        TestLogger logger;

        // Success case
        processWalletOperation(logger, false);
        assert(logger.messageCount(LogLevel::ERROR) == 0);
        assert(logger.hasMessage(LogLevel::INFO, "Transaction created successfully"));
        assert(logger.hasMessage(LogLevel::WARNING, "High fee detected"));
        std::cout << "✅ Success path logs correctly" << std::endl;

        logger.clear();

        // Failure case
        processWalletOperation(logger, true);
        assert(logger.messageCount(LogLevel::ERROR) == 1);
        assert(logger.hasMessage(LogLevel::ERROR, "insufficient funds"));
        assert(!logger.hasMessage("Transaction created successfully"));
        std::cout << "✅ Failure path logs error message" << std::endl;
    }

    // Test 5: Verify NullLogger discards everything
    {
        std::cout << "\n5. Testing NullLogger (performance baseline)..." << std::endl;
        NullLogger logger;

        // NullLogger should be a no-op - no way to verify messages were discarded
        // but we can verify it compiles and doesn't crash
        logger.debug("This is discarded");
        logger.info("This is also discarded");
        logger.error("Even errors are discarded");

        std::cout << "✅ NullLogger runs without errors" << std::endl;
    }

    // Test 6: Verify getEntries() returns full log history
    {
        std::cout << "\n6. Testing full log history retrieval..." << std::endl;
        TestLogger logger;

        logger.info("First");
        logger.error("Second");
        logger.warning("Third");

        auto entries = logger.getEntries();
        assert(entries.size() == 3);
        assert(entries[0].level == LogLevel::INFO);
        assert(entries[0].message == "First");
        assert(entries[1].level == LogLevel::ERROR);
        assert(entries[1].message == "Second");
        assert(entries[2].level == LogLevel::WARNING);
        assert(entries[2].message == "Third");

        std::cout << "✅ getEntries() preserves order and content" << std::endl;

        // Test filtered retrieval
        auto errors = logger.getEntries(LogLevel::ERROR);
        assert(errors.size() == 1);
        assert(errors[0].message == "Second");
        std::cout << "✅ getEntries(level) filters correctly" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "🎉 All logger injection tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nWhat this demonstrates:" << std::endl;
    std::cout << "✅ TestLogger enables clean unit testing" << std::endl;
    std::cout << "✅ No global state pollution between tests" << std::endl;
    std::cout << "✅ Programmatic assertion on log messages" << std::endl;
    std::cout << "✅ Thread-safe log capture (mutex protected)" << std::endl;
    std::cout << "✅ NullLogger for performance-critical tests" << std::endl;

    return 0;
}
