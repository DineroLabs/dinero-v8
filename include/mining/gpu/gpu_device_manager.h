#pragma once

#include "mining/gpu/compute_backend.h"
#include <vector>
#include <memory>

namespace dinero::gpu {

/**
 * @brief GPU Device Manager - enumerates all available GPUs
 *
 * This class detects all available GPU devices across all backends
 * (OpenCL, CUDA, Metal) and provides a unified interface for device
 * enumeration and selection.
 */
class GPUDeviceManager {
public:
    GPUDeviceManager();
    ~GPUDeviceManager();

    /**
     * @brief Detect all available GPU devices from all backends
     * @return Vector of all detected GPU devices
     */
    std::vector<GPUDevice> detectAllDevices();

    /**
     * @brief Get best available backend (prefer CUDA > OpenCL > Metal)
     * @return BackendType enum value
     */
    BackendType getBestAvailableBackend() const;

    /**
     * @brief Check if any GPU is available
     * @return true if at least one GPU device detected
     */
    bool hasGPU() const;

    /**
     * @brief Get total number of detected devices
     * @return Number of GPU devices
     */
    size_t getDeviceCount() const;

    /**
     * @brief Get device info by index
     * @param index Device index (0-based)
     * @return GPUDevice info, or null device if invalid index
     */
    GPUDevice getDevice(uint32_t index) const;

private:
    std::vector<GPUDevice> detected_devices_;
    bool has_opencl_;
    bool has_cuda_;
    bool has_metal_;

    std::vector<GPUDevice> detectOpenCLDevices();
    std::vector<GPUDevice> detectCUDADevices();
    std::vector<GPUDevice> detectMetalDevices();
};

} // namespace dinero::gpu
