#pragma once

#include <atomic>
#include <thread>
#include <chrono>
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
/// Set on shutdown so any thread parked on a barrier is freed rather than
/// waiting out its timeout while the daemon tries to exit.
inline std::atomic<bool>& BarriersAborted() {
    static std::atomic<bool> aborted{false};
    return aborted;
}

inline std::atomic<bool>& CrashHooksEnabled() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

inline bool CrashHookAllowed(bool enabled_for_this_process) {
    return enabled_for_this_process;
}

/**
 * Deterministic rendezvous for race tests. NOT a sleep.
 *
 * A timed delay cannot prove a lock is load-bearing: it only makes a race more
 * likely, so a passing run proves nothing and a failing one is flaky. This
 * blocks at an exact instruction boundary until the test explicitly releases
 * it, which makes the interleaving deterministic in both directions — with the
 * lock the other thread must wait, without it the other thread races ahead.
 *
 * Protocol (all paths bounded, so CI cannot hang):
 *   1. daemon reaches the barrier, writes "<dir>/<name>.arrived"
 *   2. test observes that file, performs its concurrent action
 *   3. test creates "<dir>/<name>.release"
 *   4. daemon proceeds
 *
 * Armed only when DINERO_BARRIER_AT names this barrier AND
 * `enabled_for_this_process` is true (regtest). Inert otherwise: no file I/O,
 * no waiting, not even a getenv beyond the first check.
 *
 * Deliberately takes the enable flag as a parameter rather than consulting
 * chainparams: gating a hook on dinero::Params() inside a low-level consensus
 * TU previously pulled ParamsImpl() into four test targets and broke them at
 * link time.
 */
inline void MaybeBarrierAt(std::string_view barrier_name, bool enabled_for_this_process) {
    if (!CrashHookAllowed(enabled_for_this_process)) return;

    const char* configured = std::getenv("DINERO_BARRIER_AT");
    if (!configured || *configured == '\0') return;
    if (barrier_name != configured) return;

    const char* dir = std::getenv("DINERO_BARRIER_DIR");
    if (!dir || *dir == '\0') return;

    // Bounded. Default 30s; a barrier that is never released must let the
    // daemon continue and the test fail on its own assertions, never hang CI.
    unsigned long timeout_s = 30;
    if (const char* t = std::getenv("DINERO_BARRIER_TIMEOUT_S"); t && *t) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(t, &end, 10);
        if (end != t && parsed > 0) timeout_s = parsed;
    }

    const std::string base = std::string(dir) + "/" + std::string(barrier_name);
    { std::FILE* f = std::fopen((base + ".arrived").c_str(), "w");
      if (f) { std::fputs("arrived\n", f); std::fclose(f); } }

    const std::string release = base + ".release";
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        if (BarriersAborted().load()) break;   // shutdown must free waiters
        if (std::FILE* r = std::fopen(release.c_str(), "r")) { std::fclose(r); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    { std::FILE* f = std::fopen((base + ".left").c_str(), "w");
      if (f) { std::fputs("left\n", f); std::fclose(f); } }
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
