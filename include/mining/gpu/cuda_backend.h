#pragma once

#include "mining/gpu/compute_backend.h"

// Driver API + NVRTC: this header uses CUdevice/CUcontext/CUmodule/CUfunction
// and CUdeviceptr (which is uintptr_t on all supported platforms). Including
// the full <cuda.h> header here would pull a substantial amount of CUDA into
// every TU that mines, so we keep the public interface to plain pointers and
// forward-declare what we can. Implementation TU includes <cuda.h> + <nvrtc.h>.
#ifdef ENABLE_CUDA
#include <cstdint>
struct CUctx_st;
struct CUmod_st;
struct CUfunc_st;
#endif

namespace dinero::gpu {

/**
 * @brief CUDA backend for NVIDIA GPUs
 *
 * Uses the CUDA Driver API + NVRTC (NVIDIA Runtime Compilation) instead of
 * nvcc-precompiled .cu kernels. This sidesteps the CUDA-12.2 + MSVC-14.44
 * toolchain mismatch (see NATIVE-MSVC-PORT-BUGS.md Bug #4) — no
 * enable_language(CUDA) at configure time, no nvcc invocation at build time.
 * The kernel is embedded in the binary as a C-string at configure time
 * (via cmake/sha256d_cuda_src.cpp.in) and NVRTC-compiled on initDevice()
 * against the running device's exact compute capability.
 *
 * Supports: NVIDIA GPUs with compute capability 6.1+ (Pascal, Volta, Turing,
 * Ampere, Ada, Hopper). PTX targets the device's actual compute_XX so any
 * supported card runs an optimal kernel without ahead-of-time per-arch builds.
 *
 * Implements IComputeBackend (stateful, single-winner mine()): the underlying
 * kernel is multi-winner (same kernel solo-miner uses for IGpuBackend), but
 * mine() returns the first winning nonce and discards the rest. The kernel's
 * multi-winner contract is preserved so consumers that adopt the multi-winner
 * pattern in the future can switch interfaces without re-porting the kernel.
 */
class CUDABackend : public IComputeBackend {
public:
    CUDABackend();
    ~CUDABackend() override;

    std::vector<GPUDevice> enumerateDevices() override;
    bool initDevice(uint32_t device_id) override;
    bool compileKernel(const std::string& kernel_source) override;
    bool mine(const WorkPackage& work, MiningResult& result) override;
    void stop() override;
    double getHashrate() const override;
    std::string getDeviceName() const override;
    BackendType getBackendType() const override { return BackendType::CUDA; }

private:
#ifdef ENABLE_CUDA
    // Driver API state. CUdevice is int, CUcontext/CUmodule/CUfunction are
    // opaque struct pointers, CUdeviceptr is uintptr_t. We hold them as
    // void*/uintptr_t to avoid forcing every including TU to take <cuda.h>.
    int          device_handle_ = -1;     // CUdevice
    CUctx_st*    context_       = nullptr;
    CUmod_st*    module_        = nullptr;
    CUfunc_st*   kernel_        = nullptr;
    std::uintptr_t d_header_        = 0;  // CUdeviceptr — 128 bytes
    std::uintptr_t d_target_        = 0;  // CUdeviceptr — 32 bytes
    std::uintptr_t d_result_nonces_ = 0;  // CUdeviceptr — 64 × uint32_t
    std::uintptr_t d_result_count_  = 0;  // CUdeviceptr — 4 bytes

    int compute_cap_major_ = 0;
    int compute_cap_minor_ = 0;
#endif

    bool        initialized_ = false;
    std::string device_name_;
    double      hashrate_    = 0.0;
};

} // namespace dinero::gpu
