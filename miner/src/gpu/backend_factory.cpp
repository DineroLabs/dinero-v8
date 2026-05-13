// Factory that tries available GPU backends in priority order. Each
// backend implementation provides a `Create<Name>Backend()` that returns
// nullptr if it can't initialise (no compatible device, missing runtime,
// etc.) so the factory can fall through to the next.

#include "solo_miner/gpu_backend.h"

namespace dinero {
namespace solo {

#ifdef MINER_ENABLE_CUDA
namespace gpu {
std::unique_ptr<IGpuBackend> CreateCudaBackend();
}
#endif

#ifdef MINER_ENABLE_METAL
namespace gpu {
std::unique_ptr<IGpuBackend> CreateMetalBackend();
}
#endif

#ifdef MINER_ENABLE_OPENCL
namespace gpu {
std::unique_ptr<IGpuBackend> CreateOpenClBackend();
}
#endif

std::unique_ptr<IGpuBackend> CreateGpuBackend() {
    // Priority order, platform-specific. On macOS, Metal is the native API
    // and is preferred over OpenCL (Apple deprecated OpenCL in favour of
    // Metal). On Windows/Linux, CUDA is preferred for NVIDIA GPUs (the
    // validated 739 MH/s path), with OpenCL as the fallback for AMD/Intel
    // hardware and for NVIDIA when CUDA Toolkit isn't compiled in.
#ifdef MINER_ENABLE_METAL
    if (auto backend = gpu::CreateMetalBackend()) {
        return backend;
    }
#endif
#ifdef MINER_ENABLE_CUDA
    if (auto backend = gpu::CreateCudaBackend()) {
        return backend;
    }
#endif
#ifdef MINER_ENABLE_OPENCL
    if (auto backend = gpu::CreateOpenClBackend()) {
        return backend;
    }
#endif
    return nullptr;
}

} // namespace solo
} // namespace dinero
