#pragma once

#include "mining/gpu/compute_backend.h"

#ifdef ENABLE_OPENCL
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#endif

namespace dinero::gpu {

/**
 * @brief OpenCL backend for AMD/Intel GPUs
 *
 * This backend uses OpenCL 1.2+ for GPU mining.
 * Supports: AMD Radeon (RX series), Intel integrated GPUs, and NVIDIA (portable mode)
 *
 * Phase 1: Stub implementation (returns "not available")
 * Phase 2: Full implementation with SHA-256d kernel
 */
class OpenCLBackend : public IComputeBackend {
public:
    OpenCLBackend();
    ~OpenCLBackend() override;

    std::vector<GPUDevice> enumerateDevices() override;
    bool initDevice(uint32_t device_id) override;
    bool compileKernel(const std::string& kernel_source) override;
    bool mine(const WorkPackage& work, MiningResult& result) override;
    void stop() override;
    double getHashrate() const override;
    std::string getDeviceName() const override;
    BackendType getBackendType() const override { return BackendType::OPENCL; }

private:
#ifdef ENABLE_OPENCL
    cl_platform_id platform_;
    cl_device_id device_;
    cl_context context_;
    cl_command_queue queue_;
    cl_program program_;
    cl_kernel kernel_;
    cl_mem d_header_;
    cl_mem d_target_;
    cl_mem d_result_nonce_;
    cl_mem d_result_found_;
#endif

    bool initialized_;
    std::string device_name_;
    double hashrate_;
};

} // namespace dinero::gpu
