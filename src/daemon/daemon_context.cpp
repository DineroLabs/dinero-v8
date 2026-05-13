#include "daemon/daemon_context.h"
#include <atomic>

namespace {
    // Thread-safe singleton storage
    // Using std::atomic to ensure thread-safe reads/writes
    std::atomic<DaemonContext*> g_daemon_context_instance{nullptr};
}

DaemonContext* DaemonContext::instance() {
    return g_daemon_context_instance.load(std::memory_order_acquire);
}

void DaemonContext::setInstance(DaemonContext* ctx) {
    g_daemon_context_instance.store(ctx, std::memory_order_release);
}
