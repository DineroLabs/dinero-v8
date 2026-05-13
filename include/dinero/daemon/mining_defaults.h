#pragma once
#include <thread>
#include <algorithm>
#include <cmath>

/**
 * @brief CPU-friendly mining defaults for desktop applications
 * 
 * Implements smart defaults that work well on desktop systems:
 * - Auto mode uses 80% of available cores (never all cores)
 * - Always leaves at least 1 core for UI and system tasks
 * - Throttle defaults to 35% duty cycle for sustainable operation
 * - Hard limits prevent resource abuse
 */
class MiningDefaults {
public:
    // CPU detection and allocation
    static unsigned detectCpuCores();
    static unsigned suggestedThreadsAuto();
    static unsigned clampThreads(unsigned requested);
    
    // Throttle management
    static double clampThrottle(double throttle);
    
    // Default values
    static constexpr double DEFAULT_THROTTLE = 0.35;  // 35% duty cycle
    static constexpr double MIN_THROTTLE = 0.15;      // 15% minimum
    static constexpr double MAX_THROTTLE = 0.90;      // 90% maximum
    static constexpr double AUTO_CPU_PERCENT = 0.80;  // 80% of cores in auto mode
};

// Inline implementations for performance-critical code
inline unsigned detectCpuCores() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 2; // conservative fallback
    return n;
}

inline unsigned suggestedThreadsAuto() {
    const unsigned n = detectCpuCores();
    const unsigned cap = (n > 1) ? n - 1 : 1;                // never starve UI
    const unsigned auto80 = std::max(1u, (unsigned)std::floor(n * 0.80));
    return std::clamp(auto80, 1u, cap);
}

inline unsigned clampThreads(unsigned req) {
    const unsigned n = detectCpuCores();
    const unsigned cap = (n > 1) ? n - 1 : 1;
    return std::clamp(req, 1u, cap);
}

inline double clampThrottle(double t) {
    if (!std::isfinite(t)) t = 0.35;
    return std::clamp(t, 0.15, 0.90);
}
