#pragma once
#include <QString>

// Only the runtime-selected backend may be described as active.
inline QString miningSessionStatus(bool running, const QString& backend, bool gpuRequested) {
    if (!running) return QStringLiteral("Not running");
    if (backend == QStringLiteral("cpu"))
        return gpuRequested ? QStringLiteral("CPU active (GPU unavailable; CPU fallback)")
                            : QStringLiteral("CPU active");
    if (backend == QStringLiteral("metal")) return QStringLiteral("Metal active");
    if (backend == QStringLiteral("cuda")) return QStringLiteral("CUDA active");
    if (backend == QStringLiteral("opencl")) return QStringLiteral("OpenCL active");
    return QStringLiteral("Backend unavailable");
}
