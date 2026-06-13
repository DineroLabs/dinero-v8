#pragma once
// Portable best-effort current-thread naming for diagnostics (issue #298).
//
// MUST be called from INSIDE the target thread: on both supported platforms
// the call names the *calling* thread. Names are truncated by the OS to its
// limit (Linux: 15 chars + NUL). Naming is a diagnostic aid only (it makes a
// `gdb thread apply all bt` readable) — it is never load-bearing, never
// throws, and silently no-ops on unsupported platforms or on failure.
#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace util {

inline void SetThreadName(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    // macOS only allows a thread to name itself (single-arg form).
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

}  // namespace util
