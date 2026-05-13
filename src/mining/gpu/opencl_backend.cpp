#include "mining/gpu/opencl_backend.h"
#include "mining/header_layout.h"  // DINERO_HEADER_SIZE_BYTES constant
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════
// GPU MINING - Phase 3 BlockHeader v1 Support (128 bytes)
// ═══════════════════════════════════════════════════════════════════════════
// UPDATED: OpenCL kernels now use 128-byte BlockHeader v1 format
//
// BlockHeader v1 layout:
// - version (4) + prevHash (32) + merkleRoot (32) + utreexo (32)
// - timestamp (8) + difficulty (4) + nonce (4) + reserved (12) = 128 bytes
//
// Kernel updated for correct:
// - Header size: 128 bytes
// - Nonce offset: 112
// - SHA-256 message length: 1024 bits
//             Kernels will be rewritten cleanly for Phase 3 layout
//
// All public methods return errors explicitly stating GPU mining is disabled
// ═══════════════════════════════════════════════════════════════════════════

// Phase 10b guard REMOVED — GPU mining re-enabled for OpenCL builds

namespace dinero::gpu {

// Forward declaration - kernel source will be loaded from file
extern std::string loadOpenCLKernelSource();

OpenCLBackend::OpenCLBackend()
    : initialized_(false)
    , hashrate_(0.0)
{
#ifdef ENABLE_OPENCL
    platform_ = nullptr;
    device_ = nullptr;
    context_ = nullptr;
    queue_ = nullptr;
    program_ = nullptr;
    kernel_ = nullptr;
    d_header_ = nullptr;
    d_target_ = nullptr;
    d_result_nonce_ = nullptr;
    d_result_found_ = nullptr;
#endif
}

OpenCLBackend::~OpenCLBackend() {
    stop();
}

std::vector<GPUDevice> OpenCLBackend::enumerateDevices() {
#ifdef GPU_MINING_DISABLED_PHASE_10B
    std::cerr << "[OpenCL] GPU mining DISABLED - see Phase 10b audit report" << std::endl;
    return std::vector<GPUDevice>();
#elif defined(ENABLE_OPENCL)
    std::vector<GPUDevice> devices;

    // Get all OpenCL platforms
    cl_uint num_platforms = 0;
    cl_int err = clGetPlatformIDs(0, nullptr, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        std::cerr << "[OpenCL] No platforms found (error: " << err << ")" << std::endl;
        return devices;
    }

    std::vector<cl_platform_id> platforms(num_platforms);
    clGetPlatformIDs(num_platforms, platforms.data(), nullptr);

    // Enumerate devices for each platform
    for (cl_uint p = 0; p < num_platforms; p++) {
        char platform_name[256];
        clGetPlatformInfo(platforms[p], CL_PLATFORM_NAME, sizeof(platform_name), platform_name, nullptr);

        // Get GPU devices for this platform
        cl_uint num_devices = 0;
        err = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
        if (err != CL_SUCCESS || num_devices == 0) {
            continue; // No GPUs on this platform
        }

        std::vector<cl_device_id> cl_devices(num_devices);
        clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, num_devices, cl_devices.data(), nullptr);

        for (cl_uint d = 0; d < num_devices; d++) {
            GPUDevice dev;
            dev.backend = BackendType::OPENCL;
            dev.device_id = static_cast<uint32_t>(devices.size()); // Sequential ID

            // Get device name
            char device_name[256];
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_NAME, sizeof(device_name), device_name, nullptr);
            dev.name = device_name;

            // Get vendor
            char vendor_name[256];
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_VENDOR, sizeof(vendor_name), vendor_name, nullptr);
            dev.vendor = vendor_name;

            // Get global memory size
            cl_ulong global_mem;
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(global_mem), &global_mem, nullptr);
            dev.global_memory_mb = static_cast<size_t>(global_mem / (1024 * 1024));

            // Get compute units
            cl_uint compute_units;
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(compute_units), &compute_units, nullptr);
            dev.compute_units = compute_units;

            // Get max clock frequency
            cl_uint max_clock;
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(max_clock), &max_clock, nullptr);
            dev.max_clock_mhz = max_clock;

            // Check if device is available
            cl_bool available;
            clGetDeviceInfo(cl_devices[d], CL_DEVICE_AVAILABLE, sizeof(available), &available, nullptr);
            dev.available = (available == CL_TRUE);

            devices.push_back(dev);

            std::cout << "[OpenCL] Found device " << dev.device_id << ": "
                      << dev.name << " (" << dev.vendor << ") "
                      << "- " << dev.compute_units << " CUs @ " << dev.max_clock_mhz << " MHz, "
                      << dev.global_memory_mb << " MB" << std::endl;
        }
    }

    return devices;
#else
    return std::vector<GPUDevice>();
#endif
}

bool OpenCLBackend::initDevice(uint32_t device_id) {
#ifdef GPU_MINING_DISABLED_PHASE_10B
    std::cerr << "[OpenCL] GPU mining DISABLED (Phase 10b audit findings)" << std::endl;
    std::cerr << "[OpenCL] Kernels updated for 128-byte headers (BlockHeader v1)" << std::endl;
    std::cerr << "[OpenCL] Re-enable after completing GPU backend integration" << std::endl;
    return false;
#elif defined(ENABLE_OPENCL)
    if (initialized_) {
        std::cerr << "[OpenCL] Device already initialized" << std::endl;
        return false;
    }

    // Enumerate all devices to find the requested one
    auto all_devices = enumerateDevices();
    if (device_id >= all_devices.size()) {
        std::cerr << "[OpenCL] Invalid device ID: " << device_id << std::endl;
        return false;
    }

    // Get platforms and devices again to obtain actual OpenCL handles
    cl_uint num_platforms = 0;
    clGetPlatformIDs(0, nullptr, &num_platforms);
    std::vector<cl_platform_id> platforms(num_platforms);
    clGetPlatformIDs(num_platforms, platforms.data(), nullptr);

    // Find the device by sequential ID
    uint32_t current_id = 0;
    bool found = false;

    for (cl_uint p = 0; p < num_platforms && !found; p++) {
        cl_uint num_devices = 0;
        cl_int err = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
        if (err != CL_SUCCESS || num_devices == 0) continue;

        std::vector<cl_device_id> cl_devices(num_devices);
        clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, num_devices, cl_devices.data(), nullptr);

        for (cl_uint d = 0; d < num_devices; d++) {
            if (current_id == device_id) {
                platform_ = platforms[p];
                device_ = cl_devices[d];
                found = true;

                // Get device name for logging
                char device_name[256];
                clGetDeviceInfo(device_, CL_DEVICE_NAME, sizeof(device_name), device_name, nullptr);
                device_name_ = device_name;
                break;
            }
            current_id++;
        }
    }

    if (!found) {
        std::cerr << "[OpenCL] Failed to find device with ID " << device_id << std::endl;
        return false;
    }

    // Create OpenCL context
    cl_int err;
    context_ = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to create context (error: " << err << ")" << std::endl;
        return false;
    }

    // Create command queue
    queue_ = clCreateCommandQueue(context_, device_, 0, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to create command queue (error: " << err << ")" << std::endl;
        clReleaseContext(context_);
        context_ = nullptr;
        return false;
    }

    std::cout << "[OpenCL] Successfully initialized device: " << device_name_ << std::endl;
    initialized_ = true;
    return true;
#else
    std::cerr << "[OpenCL] OpenCL support not compiled in" << std::endl;
    return false;
#endif
}

bool OpenCLBackend::compileKernel(const std::string& kernel_source) {
#ifdef ENABLE_OPENCL
    if (!initialized_) {
        std::cerr << "[OpenCL] Device not initialized" << std::endl;
        return false;
    }

    // Create program from source
    const char* source_ptr = kernel_source.c_str();
    size_t source_length = kernel_source.length();

    cl_int err;
    program_ = clCreateProgramWithSource(context_, 1, &source_ptr, &source_length, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to create program (error: " << err << ")" << std::endl;
        return false;
    }

    // Build program
    err = clBuildProgram(program_, 1, &device_, "-cl-std=CL1.2", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to build program (error: " << err << ")" << std::endl;

        // Get build log
        size_t log_size;
        clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::vector<char> build_log(log_size);
        clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, log_size, build_log.data(), nullptr);
        std::cerr << "[OpenCL] Build log:\n" << build_log.data() << std::endl;

        clReleaseProgram(program_);
        program_ = nullptr;
        return false;
    }

    // Create kernel
    kernel_ = clCreateKernel(program_, "sha256d_mine", &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to create kernel (error: " << err << ")" << std::endl;
        clReleaseProgram(program_);
        program_ = nullptr;
        return false;
    }

    // Allocate device memory buffers
    d_header_ = clCreateBuffer(context_, CL_MEM_READ_ONLY, DINERO_HEADER_SIZE_BYTES, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to allocate header buffer" << std::endl;
        return false;
    }

    d_target_ = clCreateBuffer(context_, CL_MEM_READ_ONLY, 32, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to allocate target buffer" << std::endl;
        return false;
    }

    d_result_nonce_ = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, sizeof(uint32_t), nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to allocate result_nonce buffer" << std::endl;
        return false;
    }

    d_result_found_ = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, sizeof(uint32_t), nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to allocate result_found buffer" << std::endl;
        return false;
    }

    std::cout << "[OpenCL] Kernel compiled successfully" << std::endl;
    return true;
#else
    std::cerr << "[OpenCL] OpenCL support not compiled in" << std::endl;
    return false;
#endif
}

bool OpenCLBackend::mine(const WorkPackage& work, MiningResult& result) {
#ifdef ENABLE_OPENCL
    if (!initialized_ || !kernel_) {
        std::cerr << "[OpenCL] Device or kernel not initialized" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // Write work data to device
    cl_int err;
    err = clEnqueueWriteBuffer(queue_, d_header_, CL_TRUE, 0, DINERO_HEADER_SIZE_BYTES, work.header, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to write header" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    err = clEnqueueWriteBuffer(queue_, d_target_, CL_TRUE, 0, 32, work.target, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to write target" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    // Initialize result flag to 0
    uint32_t found_flag = 0;
    err = clEnqueueWriteBuffer(queue_, d_result_found_, CL_TRUE, 0, sizeof(uint32_t), &found_flag, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to initialize result flag" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    // Set kernel arguments
    err  = clSetKernelArg(kernel_, 0, sizeof(cl_mem), &d_header_);
    err |= clSetKernelArg(kernel_, 1, sizeof(cl_mem), &d_target_);
    err |= clSetKernelArg(kernel_, 2, sizeof(uint32_t), &work.nonce_start);
    err |= clSetKernelArg(kernel_, 3, sizeof(cl_mem), &d_result_nonce_);
    err |= clSetKernelArg(kernel_, 4, sizeof(cl_mem), &d_result_found_);

    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to set kernel arguments (error: " << err << ")" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    // Launch kernel (1D work group)
    // Week 9.5 - Cooperative CPU+GPU: Use getNonceCount() for nonce range
    uint64_t nonce_count = work.getNonceCount();
    size_t global_work_size = static_cast<size_t>(nonce_count);
    size_t local_work_size = 256; // Workgroup size (tunable)

    err = clEnqueueNDRangeKernel(queue_, kernel_, 1, nullptr, &global_work_size, &local_work_size, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to launch kernel (error: " << err << ")" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    // Wait for completion
    clFinish(queue_);

    // Read results
    err = clEnqueueReadBuffer(queue_, d_result_found_, CL_TRUE, 0, sizeof(uint32_t), &found_flag, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "[OpenCL] Failed to read result flag" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    result.found = (found_flag != 0);
    result.hashes_tried = work.getNonceCount();  // Week 9.5 - Cooperative CPU+GPU

    if (result.found) {
        // Read winning nonce
        err = clEnqueueReadBuffer(queue_, d_result_nonce_, CL_TRUE, 0, sizeof(uint32_t), &result.nonce, 0, nullptr, nullptr);
        if (err != CL_SUCCESS) {
            std::cerr << "[OpenCL] Failed to read result nonce" << std::endl;
            result.found = false;
            return false;
        }

        std::cout << "[OpenCL] Solution found! Nonce: 0x" << std::hex << result.nonce << std::dec << std::endl;
    }

    // Calculate hashrate
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    if (duration > 0) {
        hashrate_ = (static_cast<double>(work.getNonceCount()) / duration) * 1000000.0; // Hashes per second
    }

    return true;
#else
    std::cerr << "[OpenCL] OpenCL support not compiled in" << std::endl;
    result.found = false;
    result.hashes_tried = 0;
    return false;
#endif
}

void OpenCLBackend::stop() {
#ifdef ENABLE_OPENCL
    if (d_result_found_) {
        clReleaseMemObject(d_result_found_);
        d_result_found_ = nullptr;
    }
    if (d_result_nonce_) {
        clReleaseMemObject(d_result_nonce_);
        d_result_nonce_ = nullptr;
    }
    if (d_target_) {
        clReleaseMemObject(d_target_);
        d_target_ = nullptr;
    }
    if (d_header_) {
        clReleaseMemObject(d_header_);
        d_header_ = nullptr;
    }
    if (kernel_) {
        clReleaseKernel(kernel_);
        kernel_ = nullptr;
    }
    if (program_) {
        clReleaseProgram(program_);
        program_ = nullptr;
    }
    if (queue_) {
        clReleaseCommandQueue(queue_);
        queue_ = nullptr;
    }
    if (context_) {
        clReleaseContext(context_);
        context_ = nullptr;
    }
#endif
    initialized_ = false;
    hashrate_ = 0.0;
}

double OpenCLBackend::getHashrate() const {
    return hashrate_;
}

std::string OpenCLBackend::getDeviceName() const {
    return device_name_.empty() ? "OpenCL (not initialized)" : device_name_;
}

// Helper function to load OpenCL kernel source from embedded file
std::string loadOpenCLKernelSource() {
    // In production, this would embed the kernel source at compile time
    // For now, we'll read from the file directly

    // Try multiple paths to find the kernel file:
    // 1. Relative to working directory (for installed binaries)
    // 2. Relative to source tree (for development builds)
    std::vector<std::string> search_paths = {
        "src/mining/gpu/kernels/sha256d_opencl.cl",           // Relative to project root
        "../src/mining/gpu/kernels/sha256d_opencl.cl",        // From build dir
        "../../src/mining/gpu/kernels/sha256d_opencl.cl",     // From build/bin
        "./kernels/sha256d_opencl.cl"                         // Installed location
    };

    for (const auto& path : search_paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::cerr << "[OpenCL] Loaded kernel from: " << path << std::endl;
            return buffer.str();
        }
    }

    std::cerr << "[OpenCL] Failed to load kernel source file (searched " << search_paths.size() << " paths)" << std::endl;
    return "";
}

} // namespace dinero::gpu
