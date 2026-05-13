#include "mining/gpu/compute_backend.h"
#include "mining/gpu/opencl_backend.h"
#include "mining/gpu/cuda_backend.h"
#ifdef ENABLE_METAL
#include "mining/gpu/metal_backend.h"
#endif

namespace dinero::gpu {

/**
 * @brief Factory function to create the appropriate GPU backend
 *
 * This function returns the requested backend if it was compiled in.
 * If the backend is not available, returns nullptr.
 *
 * Supported backends:
 * - CUDA:   NVIDIA GPUs (Windows/Linux)
 * - OpenCL: AMD/Intel/NVIDIA portable (Windows/Linux/macOS)
 * - Metal:  Apple Silicon native (macOS/iOS)
 */
std::unique_ptr<IComputeBackend> createBackend(BackendType backend) {
    switch (backend) {
#ifdef ENABLE_OPENCL
        case BackendType::OPENCL:
            return std::make_unique<OpenCLBackend>();
#endif

#ifdef ENABLE_CUDA
        case BackendType::CUDA:
            return std::make_unique<CUDABackend>();
#endif

#ifdef ENABLE_METAL
        case BackendType::METAL:
            return std::make_unique<MetalBackend>();
#endif

        case BackendType::NONE:
        default:
            return nullptr;
    }
}

#ifndef ENABLE_OPENCL
std::string loadOpenCLKernelSource() { return {}; }
#endif

} // namespace dinero::gpu
