#include "daemon/mining_defaults.h"
#include <thread>
#include <algorithm>
#include <cmath>

unsigned MiningDefaults::detectCpuCores() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 2; // conservative fallback
    return n;
}

unsigned MiningDefaults::suggestedThreadsAuto() {
    const unsigned n = detectCpuCores();
    const unsigned cap = (n > 1) ? n - 1 : 1;                // never starve UI
    const unsigned auto80 = std::max(1u, (unsigned)std::floor(n * 0.80));
    return std::clamp(auto80, 1u, cap);
}

unsigned MiningDefaults::clampThreads(unsigned req) {
    const unsigned n = detectCpuCores();
    const unsigned cap = (n > 1) ? n - 1 : 1;
    return std::clamp(req, 1u, cap);
}

double MiningDefaults::clampThrottle(double t) {
    if (!std::isfinite(t)) t = 0.35;
    return std::clamp(t, 0.15, 0.90);
}
