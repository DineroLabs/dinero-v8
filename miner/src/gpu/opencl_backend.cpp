// OpenCL implementation of IGpuBackend.
//
// Build-gated: only compiled when -DMINER_ENABLE_OPENCL=ON. The kernel
// source is generated at configure time from shaders/sha256d.cl via
// cmake/sha256d_cl_src.cpp.in and compiled by clBuildProgram() at runtime,
// so no offline kernel compiler is required.
//
// Targets: AMD GCN/RDNA, Intel Xe/HD, NVIDIA via the OpenCL ICD. On NVIDIA
// the CUDA backend will typically outperform this by ~2-3× (NVIDIA's OpenCL
// driver doesn't get the same optimization investment as their CUDA path),
// but OpenCL is the only realistic option for non-CUDA hardware.

#ifdef MINER_ENABLE_OPENCL

#include "solo_miner/gpu_backend.h"

// macOS deprecated the OpenCL framework in 10.14 in favour of Metal. The
// API still works; the deprecation warnings are noisy but harmless. We
// silence them locally so the miner build stays warning-clean.
#if defined(__APPLE__)
#define CL_SILENCE_DEPRECATION
#include <OpenCL/cl.h>
#else
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#endif

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

// Embedded kernel source — defined by the generated cmake/sha256d_cl_src.cpp.in
// at configure time so the .cl file's contents ship inside the binary as a raw
// string literal. clBuildProgram compiles it on init().
extern const char* SHA256D_CL_SRC;

namespace {

constexpr size_t LOCAL_WORK_SIZE = 256;
constexpr const char* KERNEL_NAME = "sha256d_mine";

bool checkCl(cl_int code, const char* what) {
    if (code == CL_SUCCESS) return true;
    std::cerr << "[opencl] " << what << " failed: " << code << std::endl;
    return false;
}

class OpenClBackend : public IGpuBackend {
public:
    static std::unique_ptr<OpenClBackend> create();

    ~OpenClBackend() override;

    GpuDispatchOutcome dispatch(const uint8_t header[128],
                                const uint8_t target[32],
                                uint32_t nonce_start,
                                uint32_t batch_size) override;

    const std::string& deviceName() const override { return device_name_; }
    const std::string& computeArch() const override { return compute_arch_; }
    std::string backendName() const override { return "opencl"; }

private:
    OpenClBackend() = default;

    cl_platform_id   platform_   = nullptr;
    cl_device_id     device_     = nullptr;
    cl_context       context_    = nullptr;
    cl_command_queue queue_      = nullptr;
    cl_program       program_    = nullptr;
    cl_kernel        kernel_     = nullptr;

    // Reusable device buffers — allocated once, reused per dispatch.
    cl_mem d_header_        = nullptr;  // 128 bytes (32 × u32 LE)
    cl_mem d_target_        = nullptr;  // 32 bytes (8 × u32 BE)
    cl_mem d_result_nonces_ = nullptr;  // GPU_RESULT_CAPACITY × u32
    cl_mem d_result_count_  = nullptr;  // 4 bytes

    std::string device_name_;
    std::string compute_arch_;

    std::mutex dispatch_mu_;
};

std::unique_ptr<OpenClBackend> OpenClBackend::create() {
    cl_uint platform_count = 0;
    if (!checkCl(clGetPlatformIDs(0, nullptr, &platform_count), "clGetPlatformIDs(count)"))
        return nullptr;
    if (platform_count == 0) {
        std::cerr << "[opencl] no OpenCL platforms detected" << std::endl;
        return nullptr;
    }

    std::vector<cl_platform_id> platforms(platform_count);
    if (!checkCl(clGetPlatformIDs(platform_count, platforms.data(), nullptr),
                 "clGetPlatformIDs"))
        return nullptr;

    // Pick the first platform that exposes a GPU device. Iterate in driver
    // order — on multi-vendor machines (Intel iGPU + NVIDIA discrete) this
    // typically gives the discrete GPU first.
    auto backend = std::unique_ptr<OpenClBackend>(new OpenClBackend());
    cl_uint device_count = 0;
    for (cl_platform_id p : platforms) {
        cl_int r = clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &device_count);
        if (r == CL_SUCCESS && device_count > 0) {
            std::vector<cl_device_id> devices(device_count);
            if (clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, device_count, devices.data(),
                               nullptr) == CL_SUCCESS) {
                backend->platform_ = p;
                backend->device_   = devices[0];
                break;
            }
        }
    }
    if (!backend->device_) {
        std::cerr << "[opencl] no OpenCL GPU device found across "
                  << platform_count << " platform(s)" << std::endl;
        return nullptr;
    }

    char name_buf[256] = {0};
    clGetDeviceInfo(backend->device_, CL_DEVICE_NAME,
                    sizeof(name_buf) - 1, name_buf, nullptr);
    backend->device_name_ = name_buf[0] ? name_buf : "unknown OpenCL device";

    char version_buf[128] = {0};
    clGetDeviceInfo(backend->device_, CL_DEVICE_VERSION,
                    sizeof(version_buf) - 1, version_buf, nullptr);
    backend->compute_arch_ = version_buf[0] ? version_buf : "opencl";

    cl_int err = CL_SUCCESS;
    backend->context_ = clCreateContext(nullptr, 1, &backend->device_,
                                        nullptr, nullptr, &err);
    if (!checkCl(err, "clCreateContext")) return nullptr;

    // clCreateCommandQueue is deprecated in OpenCL 2.0+ in favour of
    // clCreateCommandQueueWithProperties, but the deprecated form is the
    // only one that works across OpenCL 1.2 (still common) and 2.0+ drivers
    // without per-version conditionals. The deprecation warning is silenced
    // by the CL_SILENCE_DEPRECATION / CL_TARGET_OPENCL_VERSION defines.
    backend->queue_ = clCreateCommandQueue(backend->context_, backend->device_,
                                           0, &err);
    if (!checkCl(err, "clCreateCommandQueue")) return nullptr;

    const char* src = SHA256D_CL_SRC;
    size_t src_len = std::strlen(src);
    backend->program_ = clCreateProgramWithSource(backend->context_, 1,
                                                  &src, &src_len, &err);
    if (!checkCl(err, "clCreateProgramWithSource")) return nullptr;

    // Build options: tell the compiler we expect OpenCL 1.2 semantics. Most
    // drivers honour this; the kernel doesn't use any 2.0+ features.
    const char* build_opts = "-cl-std=CL1.2 -cl-fast-relaxed-math";
    err = clBuildProgram(backend->program_, 1, &backend->device_,
                         build_opts, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        // Surface the build log — clBuildProgram returns the error code
        // but the diagnostic detail is in the log only.
        size_t log_size = 0;
        clGetProgramBuildInfo(backend->program_, backend->device_,
                              CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::vector<char> log(log_size);
        clGetProgramBuildInfo(backend->program_, backend->device_,
                              CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
        std::cerr << "[opencl] clBuildProgram failed (" << err << "):\n"
                  << log.data() << std::endl;
        return nullptr;
    }

    backend->kernel_ = clCreateKernel(backend->program_, KERNEL_NAME, &err);
    if (!checkCl(err, "clCreateKernel(sha256d_mine)")) return nullptr;

    backend->d_header_ = clCreateBuffer(backend->context_,
                                        CL_MEM_READ_ONLY,
                                        128, nullptr, &err);
    if (!checkCl(err, "clCreateBuffer header")) return nullptr;

    backend->d_target_ = clCreateBuffer(backend->context_,
                                        CL_MEM_READ_ONLY,
                                        32, nullptr, &err);
    if (!checkCl(err, "clCreateBuffer target")) return nullptr;

    backend->d_result_nonces_ = clCreateBuffer(backend->context_,
                                               CL_MEM_WRITE_ONLY,
                                               GPU_RESULT_CAPACITY * sizeof(uint32_t),
                                               nullptr, &err);
    if (!checkCl(err, "clCreateBuffer result_nonces")) return nullptr;

    backend->d_result_count_ = clCreateBuffer(backend->context_,
                                              CL_MEM_READ_WRITE,
                                              sizeof(uint32_t), nullptr, &err);
    if (!checkCl(err, "clCreateBuffer result_count")) return nullptr;

    return backend;
}

OpenClBackend::~OpenClBackend() {
    if (d_result_count_)  clReleaseMemObject(d_result_count_);
    if (d_result_nonces_) clReleaseMemObject(d_result_nonces_);
    if (d_target_)        clReleaseMemObject(d_target_);
    if (d_header_)        clReleaseMemObject(d_header_);
    if (kernel_)          clReleaseKernel(kernel_);
    if (program_)         clReleaseProgram(program_);
    if (queue_)           clReleaseCommandQueue(queue_);
    if (context_)         clReleaseContext(context_);
}

GpuDispatchOutcome OpenClBackend::dispatch(const uint8_t header[128],
                                            const uint8_t target[32],
                                            uint32_t nonce_start,
                                            uint32_t batch_size) {
    GpuDispatchOutcome result{};
    if (batch_size == 0) return result;

    std::lock_guard<std::mutex> lock(dispatch_mu_);

    // Pack header bytes as 32 little-endian u32 words — matches the kernel's
    // input expectation, identical packing to the CUDA backend.
    uint32_t header_words[32];
    for (int i = 0; i < 32; i++) {
        header_words[i] =
            (uint32_t)header[i * 4]            |
            ((uint32_t)header[i * 4 + 1] << 8) |
            ((uint32_t)header[i * 4 + 2] << 16)|
            ((uint32_t)header[i * 4 + 3] << 24);
    }

    // Pack target as 8 big-endian u32 words. target_words[0] is the MSW.
    uint32_t target_words[8];
    for (int i = 0; i < 8; i++) {
        target_words[i] =
            ((uint32_t)target[i * 4]     << 24) |
            ((uint32_t)target[i * 4 + 1] << 16) |
            ((uint32_t)target[i * 4 + 2] <<  8) |
             (uint32_t)target[i * 4 + 3];
    }

    if (!checkCl(clEnqueueWriteBuffer(queue_, d_header_, CL_FALSE, 0,
                                       sizeof(header_words), header_words,
                                       0, nullptr, nullptr),
                 "clEnqueueWriteBuffer header")) return result;
    if (!checkCl(clEnqueueWriteBuffer(queue_, d_target_, CL_FALSE, 0,
                                       sizeof(target_words), target_words,
                                       0, nullptr, nullptr),
                 "clEnqueueWriteBuffer target")) return result;

    uint32_t zero_count = 0;
    if (!checkCl(clEnqueueWriteBuffer(queue_, d_result_count_, CL_FALSE, 0,
                                       sizeof(zero_count), &zero_count,
                                       0, nullptr, nullptr),
                 "clEnqueueWriteBuffer result_count=0")) return result;

    uint32_t capacity = GPU_RESULT_CAPACITY;
    cl_int set_err = CL_SUCCESS;
    set_err |= clSetKernelArg(kernel_, 0, sizeof(cl_mem),   &d_header_);
    set_err |= clSetKernelArg(kernel_, 1, sizeof(cl_mem),   &d_target_);
    set_err |= clSetKernelArg(kernel_, 2, sizeof(uint32_t), &nonce_start);
    set_err |= clSetKernelArg(kernel_, 3, sizeof(uint32_t), &batch_size);
    set_err |= clSetKernelArg(kernel_, 4, sizeof(cl_mem),   &d_result_nonces_);
    set_err |= clSetKernelArg(kernel_, 5, sizeof(cl_mem),   &d_result_count_);
    set_err |= clSetKernelArg(kernel_, 6, sizeof(uint32_t), &capacity);
    if (!checkCl(set_err, "clSetKernelArg")) return result;

    // Global work size rounded up to a whole number of local groups. The
    // kernel returns early for indices past batch_size so tail work-items
    // never hash outside the caller-owned nonce range.
    size_t global = ((batch_size + LOCAL_WORK_SIZE - 1) / LOCAL_WORK_SIZE)
                    * LOCAL_WORK_SIZE;
    size_t local  = LOCAL_WORK_SIZE;

    auto t0 = std::chrono::steady_clock::now();
    if (!checkCl(clEnqueueNDRangeKernel(queue_, kernel_, 1, nullptr,
                                         &global, &local,
                                         0, nullptr, nullptr),
                 "clEnqueueNDRangeKernel")) return result;
    if (!checkCl(clFinish(queue_), "clFinish")) return result;
    auto t1 = std::chrono::steady_clock::now();

    uint32_t total_count = 0;
    if (!checkCl(clEnqueueReadBuffer(queue_, d_result_count_, CL_TRUE, 0,
                                      sizeof(total_count), &total_count,
                                      0, nullptr, nullptr),
                 "clEnqueueReadBuffer result_count")) return result;
    result.total_count = total_count;
    result.found_count = std::min<uint32_t>(total_count, GPU_RESULT_CAPACITY);

    if (result.found_count > 0) {
        if (!checkCl(clEnqueueReadBuffer(queue_, d_result_nonces_, CL_TRUE, 0,
                                          result.found_count * sizeof(uint32_t),
                                          result.nonces,
                                          0, nullptr, nullptr),
                     "clEnqueueReadBuffer result_nonces")) {
            result.found_count = 0;
            return result;
        }
    }

    if (total_count > GPU_RESULT_CAPACITY) {
        std::cerr << "[opencl] dispatch produced " << total_count
                  << " winners (>" << GPU_RESULT_CAPACITY
                  << " capacity); " << (total_count - GPU_RESULT_CAPACITY)
                  << " dropped. Share target may be too loose; consider"
                  << " a smaller batch_size." << std::endl;
    }

    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

} // namespace

std::unique_ptr<IGpuBackend> CreateOpenClBackend() {
    return OpenClBackend::create();
}

} // namespace gpu
} // namespace solo
} // namespace dinero

#endif // MINER_ENABLE_OPENCL
