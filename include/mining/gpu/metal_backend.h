#pragma once

#include "mining/gpu/compute_backend.h"

#ifdef ENABLE_METAL
#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLComputePipelineState;
@protocol MTLBuffer;
@protocol MTLLibrary;
#else
// Forward declarations for non-Obj-C translation units
typedef void* id;
#endif
#endif

namespace dinero::gpu {

/**
 * @brief Metal backend for Apple Silicon GPUs
 *
 * This backend uses Apple's Metal API for native GPU mining on macOS/iOS.
 * Supports: Apple M1, M2, M3, M4 (and future Apple Silicon)
 *
 * Metal is the only actively supported GPU API on Apple platforms
 * (OpenCL is deprecated since macOS 10.14).
 */
class MetalBackend : public IComputeBackend {
public:
    MetalBackend();
    ~MetalBackend() override;

    std::vector<GPUDevice> enumerateDevices() override;
    bool initDevice(uint32_t device_id) override;
    bool compileKernel(const std::string& kernel_source) override;
    bool mine(const WorkPackage& work, MiningResult& result) override;
    void stop() override;
    double getHashrate() const override;
    std::string getDeviceName() const override;
    BackendType getBackendType() const override { return BackendType::METAL; }

private:
    bool initialized_;
    std::string device_name_;
    double hashrate_;

    // Metal objects stored as opaque pointers (actual Obj-C types in .mm)
    void* device_;          // id<MTLDevice>
    void* command_queue_;   // id<MTLCommandQueue>
    void* pipeline_state_;  // id<MTLComputePipelineState>
    void* library_;         // id<MTLLibrary>
    void* buf_header_;      // id<MTLBuffer>
    void* buf_target_;      // id<MTLBuffer>
    void* buf_result_nonce_; // id<MTLBuffer>
    void* buf_result_found_; // id<MTLBuffer>
    void* buf_nonce_start_;  // id<MTLBuffer>

    uint32_t max_threadgroup_size_;
};

} // namespace dinero::gpu
