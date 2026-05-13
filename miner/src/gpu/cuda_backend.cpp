// CUDA implementation of IGpuBackend.
//
// Build-gated: only compiled when -DMINER_ENABLE_CUDA=ON. The kernel
// source is generated at configure time from shaders/sha256d.cu via
// cmake/sha256d_cu_src.cpp.in and compiled by NVRTC at runtime, so no
// nvcc invocation is required (this sidesteps the CUDA 12.2 vs MSVC
// 14.44 toolchain mismatch).

#ifdef MINER_ENABLE_CUDA

#include "solo_miner/gpu_backend.h"

#include <cuda.h>
#include <nvrtc.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dinero {
namespace solo {
namespace gpu {

// Embedded kernel source — defined by the generated cmake/sha256d_cu_src.cpp.in
// at configure time so the .cu file's contents ship inside the binary as a raw
// string literal. NVRTC compiles it on init().
extern const char* SHA256D_CU_SRC;

namespace {

constexpr unsigned int THREADS_PER_BLOCK = 256u;
constexpr const char* KERNEL_NAME = "sha256d_mine";

// Small helpers that turn a CUDA Driver API or NVRTC error into a one-line
// message on std::cerr. The miner is best-effort — if anything in init() or
// dispatch() goes wrong we report and the caller falls back to CPU rather
// than throw.
bool checkCu(CUresult code, const char* what) {
    if (code == CUDA_SUCCESS) return true;
    const char* name = nullptr;
    const char* desc = nullptr;
    cuGetErrorName(code, &name);
    cuGetErrorString(code, &desc);
    std::cerr << "[cuda] " << what << " failed: "
              << (name ? name : "?") << " — "
              << (desc ? desc : "(no description)") << std::endl;
    return false;
}

bool checkNvrtc(nvrtcResult code, const char* what) {
    if (code == NVRTC_SUCCESS) return true;
    std::cerr << "[cuda] " << what << " failed: "
              << nvrtcGetErrorString(code) << std::endl;
    return false;
}

class CudaBackend : public IGpuBackend {
public:
    static std::unique_ptr<CudaBackend> create();

    ~CudaBackend() override;

    GpuDispatchOutcome dispatch(const uint8_t header[128],
                                const uint8_t target[32],
                                uint32_t nonce_start,
                                uint32_t batch_size) override;

    const std::string& deviceName() const override { return device_name_; }
    const std::string& computeArch() const override { return compute_arch_; }
    std::string backendName() const override { return "cuda"; }

private:
    CudaBackend() = default;

    CUdevice device_ = 0;
    CUcontext context_ = nullptr;
    CUmodule module_ = nullptr;
    CUfunction kernel_ = nullptr;

    // Reusable device buffers — allocated once, reused per dispatch.
    CUdeviceptr d_header_ = 0;          // 128 bytes (32 × u32 LE)
    CUdeviceptr d_target_ = 0;          // 32 bytes (8 × u32 BE)
    CUdeviceptr d_result_nonces_ = 0;   // GPU_RESULT_CAPACITY × u32
    CUdeviceptr d_result_count_ = 0;    // 4 bytes

    std::string device_name_;
    std::string compute_arch_;

    // dispatch() is not re-entrant — the device buffers are shared. The
    // miner currently uses one GPU thread, but the mutex keeps the API
    // safe if a future hybrid design dispatches from multiple host threads.
    std::mutex dispatch_mu_;
};

std::unique_ptr<CudaBackend> CudaBackend::create() {
    if (!checkCu(cuInit(0), "cuInit")) return nullptr;

    int device_count = 0;
    if (!checkCu(cuDeviceGetCount(&device_count), "cuDeviceGetCount")) return nullptr;
    if (device_count == 0) {
        std::cerr << "[cuda] no CUDA-capable device detected" << std::endl;
        return nullptr;
    }

    auto backend = std::unique_ptr<CudaBackend>(new CudaBackend());

    if (!checkCu(cuDeviceGet(&backend->device_, 0), "cuDeviceGet(0)")) return nullptr;

    char name_buf[256] = {0};
    if (checkCu(cuDeviceGetName(name_buf, sizeof(name_buf) - 1, backend->device_),
                "cuDeviceGetName")) {
        backend->device_name_ = name_buf;
    } else {
        backend->device_name_ = "unknown CUDA device";
    }

    int cc_major = 0;
    int cc_minor = 0;
    cuDeviceGetAttribute(&cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                         backend->device_);
    cuDeviceGetAttribute(&cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                         backend->device_);
    backend->compute_arch_ =
        "sm_" + std::to_string(cc_major) + std::to_string(cc_minor);

    if (!checkCu(cuCtxCreate(&backend->context_, 0, backend->device_), "cuCtxCreate"))
        return nullptr;

    // NVRTC compile the embedded kernel source against the actual device's
    // compute capability so the PTX matches the hardware exactly.
    nvrtcProgram prog = nullptr;
    if (!checkNvrtc(
            nvrtcCreateProgram(&prog, SHA256D_CU_SRC, "sha256d.cu", 0, nullptr, nullptr),
            "nvrtcCreateProgram"))
        return nullptr;

    std::string arch_flag = "--gpu-architecture=compute_" +
                            std::to_string(cc_major) + std::to_string(cc_minor);
    const char* opts[] = { arch_flag.c_str() };
    nvrtcResult compile = nvrtcCompileProgram(prog, 1, opts);
    if (compile != NVRTC_SUCCESS) {
        // Surface the NVRTC log — otherwise NVRTC just returns
        // NVRTC_ERROR_COMPILATION with no detail.
        size_t log_size = 0;
        nvrtcGetProgramLogSize(prog, &log_size);
        std::vector<char> log(log_size);
        nvrtcGetProgramLog(prog, log.data());
        std::cerr << "[cuda] nvrtcCompileProgram failed for " << arch_flag << ":\n"
                  << log.data() << std::endl;
        nvrtcDestroyProgram(&prog);
        return nullptr;
    }

    size_t ptx_size = 0;
    if (!checkNvrtc(nvrtcGetPTXSize(prog, &ptx_size), "nvrtcGetPTXSize")) {
        nvrtcDestroyProgram(&prog);
        return nullptr;
    }
    std::vector<char> ptx(ptx_size);
    if (!checkNvrtc(nvrtcGetPTX(prog, ptx.data()), "nvrtcGetPTX")) {
        nvrtcDestroyProgram(&prog);
        return nullptr;
    }
    nvrtcDestroyProgram(&prog);

    if (!checkCu(cuModuleLoadData(&backend->module_, ptx.data()), "cuModuleLoadData"))
        return nullptr;
    if (!checkCu(cuModuleGetFunction(&backend->kernel_, backend->module_, KERNEL_NAME),
                 "cuModuleGetFunction(sha256d_mine)"))
        return nullptr;

    if (!checkCu(cuMemAlloc(&backend->d_header_, 128), "cuMemAlloc header")) return nullptr;
    if (!checkCu(cuMemAlloc(&backend->d_target_, 32),  "cuMemAlloc target")) return nullptr;
    if (!checkCu(cuMemAlloc(&backend->d_result_nonces_,
                            GPU_RESULT_CAPACITY * sizeof(uint32_t)),
                 "cuMemAlloc result_nonces"))
        return nullptr;
    if (!checkCu(cuMemAlloc(&backend->d_result_count_, sizeof(uint32_t)),
                 "cuMemAlloc result_count"))
        return nullptr;

    return backend;
}

CudaBackend::~CudaBackend() {
    if (d_result_count_)  cuMemFree(d_result_count_);
    if (d_result_nonces_) cuMemFree(d_result_nonces_);
    if (d_target_)        cuMemFree(d_target_);
    if (d_header_)        cuMemFree(d_header_);
    if (module_)          cuModuleUnload(module_);
    if (context_)         cuCtxDestroy(context_);
}

GpuDispatchOutcome CudaBackend::dispatch(const uint8_t header[128],
                                         const uint8_t target[32],
                                         uint32_t nonce_start,
                                         uint32_t batch_size) {
    GpuDispatchOutcome result{};
    if (batch_size == 0) return result;

    std::lock_guard<std::mutex> lock(dispatch_mu_);
    cuCtxSetCurrent(context_);

    // Pack header bytes as 32 little-endian u32 words — matches the kernel's
    // input expectation and the dinero-sv2-gpu-miner host packing exactly.
    uint32_t header_words[32];
    for (int i = 0; i < 32; i++) {
        header_words[i] =
            (uint32_t)header[i * 4]            |
            ((uint32_t)header[i * 4 + 1] << 8) |
            ((uint32_t)header[i * 4 + 2] << 16)|
            ((uint32_t)header[i * 4 + 3] << 24);
    }

    // Pack target as 8 big-endian u32 words. target_words[0] is the MSW
    // (u32_from_be_bytes of target_bytes[0..4]) — kernel walks MSW→LSW.
    uint32_t target_words[8];
    for (int i = 0; i < 8; i++) {
        target_words[i] =
            ((uint32_t)target[i * 4]     << 24) |
            ((uint32_t)target[i * 4 + 1] << 16) |
            ((uint32_t)target[i * 4 + 2] <<  8) |
             (uint32_t)target[i * 4 + 3];
    }

    if (!checkCu(cuMemcpyHtoD(d_header_, header_words, sizeof(header_words)),
                 "cuMemcpyHtoD header")) return result;
    if (!checkCu(cuMemcpyHtoD(d_target_, target_words, sizeof(target_words)),
                 "cuMemcpyHtoD target")) return result;

    // Reset only result_count — result_nonces is overwritten only at the
    // slots actually used, and the host reads at most `count` of them so
    // stale data past count is never observed.
    uint32_t zero_count = 0;
    if (!checkCu(cuMemcpyHtoD(d_result_count_, &zero_count, sizeof(zero_count)),
                 "cuMemcpyHtoD result_count=0")) return result;

    // Round grid up to a whole number of blocks. The kernel returns early
    // for indices past batch_size so tail batches never hash outside the
    // caller-owned nonce range.
    unsigned int grid = (batch_size + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;

    uint32_t capacity = GPU_RESULT_CAPACITY;
    void* args[] = {
        &d_header_,
        &d_target_,
        &nonce_start,
        &batch_size,
        &d_result_nonces_,
        &d_result_count_,
        &capacity,
    };

    auto t0 = std::chrono::steady_clock::now();
    if (!checkCu(cuLaunchKernel(kernel_,
                                grid, 1, 1,
                                THREADS_PER_BLOCK, 1, 1,
                                0,         // shared_mem_bytes
                                nullptr,   // default stream
                                args,
                                nullptr),
                 "cuLaunchKernel")) return result;
    if (!checkCu(cuCtxSynchronize(), "cuCtxSynchronize")) return result;
    auto t1 = std::chrono::steady_clock::now();

    uint32_t total_count = 0;
    if (!checkCu(cuMemcpyDtoH(&total_count, d_result_count_, sizeof(total_count)),
                 "cuMemcpyDtoH result_count")) return result;
    result.total_count = total_count;
    result.found_count = std::min<uint32_t>(total_count, GPU_RESULT_CAPACITY);

    if (result.found_count > 0) {
        if (!checkCu(cuMemcpyDtoH(result.nonces, d_result_nonces_,
                                  result.found_count * sizeof(uint32_t)),
                     "cuMemcpyDtoH result_nonces")) {
            // Partial read — drop everything to be safe; caller treats
            // outcome.found_count == 0 as no winner.
            result.found_count = 0;
            return result;
        }
    }

    if (total_count > GPU_RESULT_CAPACITY) {
        std::cerr << "[cuda] dispatch produced " << total_count
                  << " winners (>" << GPU_RESULT_CAPACITY
                  << " capacity); " << (total_count - GPU_RESULT_CAPACITY)
                  << " dropped. Share target may be too loose; consider"
                  << " a smaller batch_size." << std::endl;
    }

    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

} // namespace

std::unique_ptr<IGpuBackend> CreateCudaBackend() {
    return CudaBackend::create();
}

} // namespace gpu
} // namespace solo
} // namespace dinero

#endif // MINER_ENABLE_CUDA
