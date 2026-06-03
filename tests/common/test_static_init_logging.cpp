// Regression test for the macOS daemon-start crash (issue #224 follow-up).
//
// #224 added an unconditional `std::lock_guard<std::mutex> lock(file_mutex_)`
// to Logger::log(). A static initializer in the daemon (methods_mining_extras.cpp's
// MiningExtrasAutoRegistration ctor) calls g_logger.info() during dyld startup,
// BEFORE g_logger's own dynamic initialization runs. At that point file_mutex_ is
// still zero-initialized. On macOS PTHREAD_MUTEX_INITIALIZER is non-zero (signature
// 0x32AAABA7), so locking a zero-initialized std::mutex throws std::system_error
// (EINVAL) -> uncaught during static init -> abort(). Result: every macOS full
// (mining) build of dinerod aborted at launch, so the dinero-qt embedded daemon
// never started. Linux's all-zero PTHREAD_MUTEX_INITIALIZER masked it.
//
// The fix gates the file_mutex_ acquisition on an atomic has_file_dest_ flag that
// is only set by setLogFile() (called from main(), after all static init). Atomics
// are valid zero-initialized, so a log() from a global constructor takes the
// console-only path and never touches the uninitialized mutex.
//
// This test forces the exact bad init order with a high-priority constructor and
// asserts the process survives to main(). Without the fix it aborts at load on
// macOS; with the fix it prints "ok" and exits 0.

#include "common/logger.h"

#include <cstdio>

#if !defined(_WIN32)
// constructor(101): runs during image load, ahead of normal C++ dynamic
// initialization of g_logger (faithfully reproducing the daemon's crash order).
__attribute__((constructor(101))) static void LogFromGlobalConstructor() {
    dinero::g_logger.info("[regression] static-init log must not abort the process");
}
#endif

int main() {
    // Reaching here means the load-time log() above did not abort.
    dinero::g_logger.info("[regression] reached main()");
    std::puts("ok");
    return 0;
}
