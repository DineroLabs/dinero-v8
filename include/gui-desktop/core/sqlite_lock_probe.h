#pragma once

#include <string>

namespace dinero {

// Exit codes you already use/expect
enum ProbeExit {
    PROBE_OK          = 0,  // unlocked
    PROBE_LOCKED_BUSY = 2,  // locked/busy
    PROBE_IO_ERROR    = 74, // EX_IOERR
    PROBE_INTERNAL    = 70  // EX_SOFTWARE
};

// Human-friendly status for JSON/text
enum class ProbeStatus { Unlocked, LockedBusy, IoError, Internal };

struct ProbeResult {
    ProbeStatus status;
    int sqlite_code;          // raw sqlite code (or 0)
    std::string sqlite_msg;   // raw sqlite message (optional)
};

/// Try to take a write reservation instantly (BEGIN IMMEDIATE) and roll it back.
/// - Returns PROBE_OK if DB is currently writeable (no competing writer)
/// - Returns PROBE_LOCKED_BUSY if a writer holds the lock
/// - Returns PROBE_IO_ERROR / PROBE_INTERNAL for other failures
int sqlite_instant_write_probe(const std::string& db_path, ProbeResult& out);

} // namespace dinero
