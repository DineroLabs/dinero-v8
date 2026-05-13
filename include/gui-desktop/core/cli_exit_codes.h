#pragma once

/**
 * Exit codes for CLI operations (shared between GUI and headless modes)
 *
 * These constants ensure consistent error reporting across the application
 * and enable reliable CI automation.
 */
namespace dinero {
namespace gui {

enum class CliExitCode {
    OK = 0,                    // Operation completed successfully
    LOCKED_BUSY = 2,           // Database is locked by another process
    INTEGRITY_FAIL = 3,        // Integrity check failed
    MIGRATION_NEEDED = 4,      // Database migration required
    CANCELED = 5,              // Operation was canceled
    IO_ERROR = 6,              // I/O error (file not found, permissions, etc.)
    TIMEOUT = 7,               // Operation timed out
    NOT_IMPLEMENTED = 9,       // Feature not yet implemented
    BAD_ARGS = 9,              // Invalid command line arguments
    DISK_FULL = 6              // Disk full (mapped to IO_ERROR for compatibility)
};

} // namespace gui
} // namespace dinero
