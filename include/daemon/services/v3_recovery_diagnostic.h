#pragma once
// Diagnostic only: says why an automatic v3 wallet recovery pass gave up.
// These passes retry forever and used to refuse without a word, so a wallet
// that never enrolls (balance stays at zero) could not be diagnosed from a
// user's log. No behaviour depends on anything in this file.
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

namespace dinero {
inline void V3RecoveryDiagnostic(const std::string& where, const std::string& what) {
    static std::mutex mutex;
    static std::string last;
    static uint64_t repeats = 0;
    const std::string line = where + ": " + what;
    std::lock_guard<std::mutex> lock(mutex);
    if (line == last) {
        // The worker retries about every two minutes: say it again now and then.
        if (++repeats % 20 != 0) return;
        std::cerr << "[V3Recovery] " << line << " [the same " << repeats << " more times]" << std::endl;
        return;
    }
    last = line;
    repeats = 0;
    std::cerr << "[V3Recovery] " << line << std::endl;
}
}  // namespace dinero
