#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dinero::testing {

/**
 * Process-wide enable for crash hooks, set once by the daemon at startup when
 * it knows the network.
 *
 * Exists so low-level consensus translation units can carry a hook WITHOUT
 * referencing dinero::Params(). Gating a hook on Params() inside
 * shielded_validation.cpp pulled ParamsImpl() into every target that links
 * that TU and broke four of them at link time; the dependency belongs at the
 * daemon layer that already knows the network, not in consensus code.
 *
 * Defaults to false, so a tool or test binary that never sets it cannot abort.
 */
inline std::atomic<bool>& CrashHooksEnabled() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

inline bool CrashHookAllowed(bool enabled_for_this_process) {
    return enabled_for_this_process;
}

inline void MaybeAbortAt(std::string_view hook_name, bool enabled_for_this_process) {
    if (!CrashHookAllowed(enabled_for_this_process)) {
        return;
    }

    const char* configured_hook = std::getenv("DINERO_CRASH_AT");
    if (!configured_hook || *configured_hook == '\0') {
        return;
    }

    if (hook_name != configured_hook) {
        return;
    }

    unsigned long target_hit = 1;
    if (const char* countdown = std::getenv("DINERO_CRASH_AFTER_N");
        countdown && *countdown != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(countdown, &end, 10);
        if (end != countdown && parsed > 0) {
            target_hit = parsed;
        }
    }

    static std::mutex hits_mutex;
    static std::unordered_map<std::string, unsigned long> hits_by_hook;

    unsigned long hit = 0;
    {
        std::lock_guard<std::mutex> lock(hits_mutex);
        hit = ++hits_by_hook[std::string(hook_name)];
    }

    if (hit < target_hit) {
        return;
    }

    std::fprintf(stderr,
                 "\n[DINERO_CRASH] aborting at hook '%s' (hit %lu / target %lu)\n",
                 std::string(hook_name).c_str(),
                 hit,
                 target_hit);
    std::fflush(stderr);
    std::abort();
}

}  // namespace dinero::testing
