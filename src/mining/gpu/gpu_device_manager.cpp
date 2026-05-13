#include "mining/gpu/gpu_device_manager.h"
#include "mining/gpu/opencl_backend.h"
#include "mining/gpu/cuda_backend.h"
#ifdef ENABLE_METAL
#include "mining/gpu/metal_backend.h"
#endif

namespace dinero::gpu {

/**
 * @brief GPU Device Manager - Phase 1 Stub Implementation
 *
 * Manages GPU device detection across all backends (OpenCL, CUDA, Metal).
 * Phase 1: Returns empty device lists (stubs)
 * Phase 2+: Real device enumeration
 */

GPUDeviceManager::GPUDeviceManager()
    : has_opencl_(false)
    , has_cuda_(false)
    , has_metal_(false)
{
}

GPUDeviceManager::~GPUDeviceManager() {
}

std::vector<GPUDevice> GPUDeviceManager::detectAllDevices() {
    detected_devices_.clear();

#ifdef ENABLE_OPENCL
    auto opencl_devices = detectOpenCLDevices();
    detected_devices_.insert(detected_devices_.end(), opencl_devices.begin(), opencl_devices.end());
    has_opencl_ = !opencl_devices.empty();
#endif

#ifdef ENABLE_CUDA
    auto cuda_devices = detectCUDADevices();
    detected_devices_.insert(detected_devices_.end(), cuda_devices.begin(), cuda_devices.end());
    has_cuda_ = !cuda_devices.empty();
#endif

#ifdef ENABLE_METAL
    auto metal_devices = detectMetalDevices();
    detected_devices_.insert(detected_devices_.end(), metal_devices.begin(), metal_devices.end());
    has_metal_ = !metal_devices.empty();
#endif

    return detected_devices_;
}

BackendType GPUDeviceManager::getBestAvailableBackend() const {
    // Priority: CUDA > Metal > OpenCL
    // CUDA: best for NVIDIA GPUs (Windows/Linux)
    // Metal: native Apple Silicon GPU (macOS) — preferred over deprecated OpenCL
    // OpenCL: portable fallback (AMD, Intel)

    if (has_cuda_) {
        return BackendType::CUDA;
    }
    if (has_metal_) {
        return BackendType::METAL;
    }
    if (has_opencl_) {
        return BackendType::OPENCL;
    }
    return BackendType::NONE;
}

bool GPUDeviceManager::hasGPU() const {
    return has_opencl_ || has_cuda_ || has_metal_;
}

size_t GPUDeviceManager::getDeviceCount() const {
    return detected_devices_.size();
}

GPUDevice GPUDeviceManager::getDevice(uint32_t index) const {
    if (index >= detected_devices_.size()) {
        return GPUDevice(); // Return empty device if index out of range
    }
    return detected_devices_[index];
}

std::vector<GPUDevice> GPUDeviceManager::detectOpenCLDevices() {
    // Phase 1: Stub - returns empty vector
    // Phase 2: Real OpenCL device enumeration
#ifdef ENABLE_OPENCL
    OpenCLBackend backend;
    return backend.enumerateDevices();
#else
    return std::vector<GPUDevice>();
#endif
}

std::vector<GPUDevice> GPUDeviceManager::detectCUDADevices() {
    // Phase 1: Stub - returns empty vector
    // Phase 3: Real CUDA device enumeration
#ifdef ENABLE_CUDA
    CUDABackend backend;
    return backend.enumerateDevices();
#else
    return std::vector<GPUDevice>();
#endif
}

std::vector<GPUDevice> GPUDeviceManager::detectMetalDevices() {
#ifdef ENABLE_METAL
    MetalBackend backend;
    return backend.enumerateDevices();
#else
    return std::vector<GPUDevice>();
#endif
}

} // namespace dinero::gpu
