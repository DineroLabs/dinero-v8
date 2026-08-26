#include "mining/gpu/metal_backend.h"
#include "mining/header_layout.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>

#ifdef ENABLE_METAL
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSArray<id<MTLDevice>>* copyAllMetalDevicesIfAvailable() {
    // MTLCopyAllDevices predates every supported macOS target, but was only
    // introduced on iOS 18. NodeCore supports iOS 15, where the default Metal
    // device remains the safe single-device fallback.
    if (@available(macOS 10.11, iOS 18.0, *)) {
        return MTLCopyAllDevices();
    }
    return nil;
}

static size_t metalWorkingSetMB(id<MTLDevice> device) {
    // This property is unavailable on iOS 15. Memory size is informational;
    // report zero rather than calling through an unavailable selector.
    if (@available(macOS 10.12, iOS 16.0, *)) {
        return static_cast<size_t>([device recommendedMaxWorkingSetSize] /
                                   (1024 * 1024));
    }
    return 0;
}
#endif

namespace dinero::gpu {

// Embedded Metal kernel source — compiled into binary so no file I/O needed at runtime
static std::string getEmbeddedKernelSource() {
    return R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

constant uint32_t H_INIT[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

inline uint32_t swap_endian(uint32_t x) {
    return ((x << 24) & 0xFF000000) | ((x << 8) & 0x00FF0000) |
           ((x >> 8) & 0x0000FF00) | ((x >> 24) & 0x000000FF);
}

void sha256_transform(thread uint32_t* state, thread uint32_t* block) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++) W[i] = block[i];
    for (int i = 16; i < 64; i++) W[i] = sig1(W[i-2]) + W[i-7] + sig0(W[i-15]) + W[i-16];
    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + ep1(e) + ch(e,f,g) + K[i] + W[i];
        t2 = ep0(a) + maj(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

// Compare SHA-256 output (state[0]=H0=MSW) against target (target[0]=MSW)
// Both arrays: index 0 = most significant word, index 7 = least significant
bool hash_meets_target(thread uint32_t* hash, device const uint32_t* target) {
    for (int i = 0; i < 8; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return false;
}

kernel void sha256d_mine(
    device const uint32_t* header        [[buffer(0)]],
    device const uint32_t* target        [[buffer(1)]],
    device atomic_uint*    result_nonce  [[buffer(2)]],
    device atomic_uint*    result_found  [[buffer(3)]],
    device const uint32_t* nonce_start_buf [[buffer(4)]],
    uint                   gid           [[thread_position_in_grid]])
{
    uint32_t nonce = nonce_start_buf[0] + gid;
    uint32_t block1[16];
    for (int i = 0; i < 16; i++) block1[i] = swap_endian(header[i]);
    uint32_t block2[16];
    for (int i = 0; i < 12; i++) block2[i] = swap_endian(header[16 + i]);
    block2[12] = swap_endian(nonce);
    block2[13] = swap_endian(header[29]);
    block2[14] = swap_endian(header[30]);
    block2[15] = swap_endian(header[31]);
    uint32_t state1[8];
    for (int i = 0; i < 8; i++) state1[i] = H_INIT[i];
    sha256_transform(state1, block1);
    sha256_transform(state1, block2);
    uint32_t pad_block[16];
    pad_block[0] = 0x80000000;
    for (int i = 1; i < 14; i++) pad_block[i] = 0;
    pad_block[14] = 0;
    pad_block[15] = 1024;
    sha256_transform(state1, pad_block);
    uint32_t block3[16];
    for (int i = 0; i < 8; i++) block3[i] = state1[i];
    block3[8] = 0x80000000;
    for (int i = 9; i < 14; i++) block3[i] = 0;
    block3[14] = 0;
    block3[15] = 256;
    uint32_t state2[8];
    for (int i = 0; i < 8; i++) state2[i] = H_INIT[i];
    sha256_transform(state2, block3);
    if (hash_meets_target(state2, target)) {
        uint expected = 0;
        if (atomic_compare_exchange_weak_explicit(result_found, &expected, 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            atomic_store_explicit(result_nonce, nonce, memory_order_relaxed);
        }
    }
}
)METAL";
}

MetalBackend::MetalBackend()
    : initialized_(false)
    , hashrate_(0.0)
    , device_(nullptr)
    , command_queue_(nullptr)
    , pipeline_state_(nullptr)
    , library_(nullptr)
    , buf_header_(nullptr)
    , buf_target_(nullptr)
    , buf_result_nonce_(nullptr)
    , buf_result_found_(nullptr)
    , buf_nonce_start_(nullptr)
    , max_threadgroup_size_(256)
{
}

MetalBackend::~MetalBackend() {
    stop();
}

std::vector<GPUDevice> MetalBackend::enumerateDevices() {
    std::vector<GPUDevice> devices;

#ifdef ENABLE_METAL
    @autoreleasepool {
        // Get all Metal devices
        NSArray<id<MTLDevice>>* mtl_devices = copyAllMetalDevicesIfAvailable();
        if (!mtl_devices || [mtl_devices count] == 0) {
            // Try the default device (always available on Apple Silicon)
            id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
            if (default_device) {
                GPUDevice dev;
                dev.backend = BackendType::METAL;
                dev.device_id = 0;
                dev.name = [[default_device name] UTF8String];
                dev.vendor = "Apple Inc.";
                // recommendedMaxWorkingSetSize gives us usable GPU memory
                dev.global_memory_mb = metalWorkingSetMB(default_device);
                // maxThreadsPerThreadgroup gives thread capacity
                dev.compute_units = static_cast<uint32_t>([default_device maxThreadsPerThreadgroup].width);
                dev.max_clock_mhz = 0; // Metal doesn't expose clock frequency
                dev.available = true;

                devices.push_back(dev);

                std::cout << "[Metal] Found device 0: " << dev.name
                          << " (Apple) - " << dev.global_memory_mb << " MB working set, "
                          << "max threadgroup " << dev.compute_units << std::endl;
            }
            return devices;
        }

        uint32_t idx = 0;
        for (id<MTLDevice> mtl_dev in mtl_devices) {
            GPUDevice dev;
            dev.backend = BackendType::METAL;
            dev.device_id = idx;
            dev.name = [[mtl_dev name] UTF8String];
            dev.vendor = "Apple Inc.";
            dev.global_memory_mb = metalWorkingSetMB(mtl_dev);
            dev.compute_units = static_cast<uint32_t>([mtl_dev maxThreadsPerThreadgroup].width);
            dev.max_clock_mhz = 0;
            dev.available = true; // All Metal devices are usable

            devices.push_back(dev);

            std::cout << "[Metal] Found device " << idx << ": " << dev.name
                      << " (Apple) - " << dev.global_memory_mb << " MB, "
                      << "max threadgroup " << dev.compute_units << std::endl;
            idx++;
        }
    }
#endif

    return devices;
}

bool MetalBackend::initDevice(uint32_t device_id) {
#ifdef ENABLE_METAL
    if (initialized_) {
        std::cerr << "[Metal] Device already initialized" << std::endl;
        return false;
    }

    @autoreleasepool {
        id<MTLDevice> mtl_device = nil;

        // Try to get specific device by ID
        NSArray<id<MTLDevice>>* mtl_devices = copyAllMetalDevicesIfAvailable();
        if (mtl_devices && [mtl_devices count] > device_id) {
            mtl_device = mtl_devices[device_id];
        } else if (device_id == 0) {
            mtl_device = MTLCreateSystemDefaultDevice();
        }

        if (!mtl_device) {
            std::cerr << "[Metal] Failed to get device with ID " << device_id << std::endl;
            return false;
        }

        // Retain the device (prevent ARC from releasing it)
        device_ = (__bridge_retained void*)mtl_device;
        device_name_ = [[mtl_device name] UTF8String];

        // Create command queue
        id<MTLCommandQueue> queue = [mtl_device newCommandQueue];
        if (!queue) {
            std::cerr << "[Metal] Failed to create command queue" << std::endl;
            device_ = nullptr;
            return false;
        }
        command_queue_ = (__bridge_retained void*)queue;

        // Get max threadgroup size for this device
        max_threadgroup_size_ = static_cast<uint32_t>([mtl_device maxThreadsPerThreadgroup].width);
        if (max_threadgroup_size_ > 1024) max_threadgroup_size_ = 1024;
        if (max_threadgroup_size_ < 32) max_threadgroup_size_ = 32;
        // Use 256 as default (good balance for SHA-256 workload)
        max_threadgroup_size_ = 256;

        std::cout << "[Metal] Successfully initialized device: " << device_name_ << std::endl;
        initialized_ = true;
        return true;
    }
#else
    std::cerr << "[Metal] Metal support not compiled in" << std::endl;
    return false;
#endif
}

bool MetalBackend::compileKernel(const std::string& kernel_source) {
#ifdef ENABLE_METAL
    if (!initialized_) {
        std::cerr << "[Metal] Device not initialized" << std::endl;
        return false;
    }

    @autoreleasepool {
        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
        NSError* error = nil;

        // Strategy: Try compiled metallib first, then fall back to source compilation
        id<MTLLibrary> library = nil;

        // Try loading pre-compiled metallib from various paths
        NSArray* metallib_paths = @[
            @"sha256d_metal.metallib",
            @"../src/mining/gpu/kernels/sha256d_metal.metallib",
            @"../../src/mining/gpu/kernels/sha256d_metal.metallib",
            @"kernels/sha256d_metal.metallib"
        ];

        for (NSString* path in metallib_paths) {
            NSURL* url = [NSURL fileURLWithPath:path];
            library = [mtl_device newLibraryWithURL:url error:&error];
            if (library) {
                std::cout << "[Metal] Loaded pre-compiled metallib from: "
                          << [path UTF8String] << std::endl;
                break;
            }
        }

        // If no metallib found, compile from embedded source
        if (!library) {
            std::string source;
            if (!kernel_source.empty()) {
                source = kernel_source;
            } else {
                // Use embedded kernel source (compiled into binary)
                source = getEmbeddedKernelSource();
                if (source.empty()) {
                    // Fallback: try loading from file (development only)
                    std::vector<std::string> search_paths = {
                        "src/mining/gpu/kernels/sha256d_metal.metal",
                        "../src/mining/gpu/kernels/sha256d_metal.metal",
                    };
                    for (const auto& path : search_paths) {
                        std::ifstream file(path);
                        if (file.is_open()) {
                            std::stringstream buffer;
                            buffer << file.rdbuf();
                            source = buffer.str();
                            std::cout << "[Metal] Loaded kernel source from file: " << path << std::endl;
                            break;
                        }
                    }
                } else {
                    std::cout << "[Metal] Using embedded kernel source" << std::endl;
                }
            }

            if (source.empty()) {
                std::cerr << "[Metal] Failed to find kernel source" << std::endl;
                return false;
            }

            NSString* ns_source = [NSString stringWithUTF8String:source.c_str()];
            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            options.fastMathEnabled = YES;

            library = [mtl_device newLibraryWithSource:ns_source options:options error:&error];
            if (!library) {
                std::cerr << "[Metal] Failed to compile kernel: "
                          << [[error localizedDescription] UTF8String] << std::endl;
                return false;
            }
            std::cout << "[Metal] Kernel compiled from source successfully" << std::endl;
        }

        library_ = (__bridge_retained void*)library;

        // Create compute pipeline from the sha256d_mine function
        id<MTLFunction> function = [library newFunctionWithName:@"sha256d_mine"];
        if (!function) {
            std::cerr << "[Metal] Failed to find 'sha256d_mine' function in library" << std::endl;
            return false;
        }

        id<MTLComputePipelineState> pipeline =
            [mtl_device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            std::cerr << "[Metal] Failed to create pipeline state: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return false;
        }
        pipeline_state_ = (__bridge_retained void*)pipeline;

        // Update max threadgroup size from pipeline
        NSUInteger maxTGS = [pipeline maxTotalThreadsPerThreadgroup];
        if (maxTGS < max_threadgroup_size_) {
            max_threadgroup_size_ = static_cast<uint32_t>(maxTGS);
        }

        // Allocate Metal buffers
        buf_header_ = (__bridge_retained void*)[mtl_device newBufferWithLength:DINERO_HEADER_SIZE_BYTES
                                                                       options:MTLResourceStorageModeShared];
        buf_target_ = (__bridge_retained void*)[mtl_device newBufferWithLength:32
                                                                       options:MTLResourceStorageModeShared];
        buf_result_nonce_ = (__bridge_retained void*)[mtl_device newBufferWithLength:sizeof(uint32_t)
                                                                             options:MTLResourceStorageModeShared];
        buf_result_found_ = (__bridge_retained void*)[mtl_device newBufferWithLength:sizeof(uint32_t)
                                                                             options:MTLResourceStorageModeShared];
        buf_nonce_start_ = (__bridge_retained void*)[mtl_device newBufferWithLength:sizeof(uint32_t)
                                                                            options:MTLResourceStorageModeShared];

        if (!buf_header_ || !buf_target_ || !buf_result_nonce_ || !buf_result_found_ || !buf_nonce_start_) {
            std::cerr << "[Metal] Failed to allocate GPU buffers" << std::endl;
            return false;
        }

        std::cout << "[Metal] Pipeline and buffers ready (threadgroup size: "
                  << max_threadgroup_size_ << ")" << std::endl;
        return true;
    }
#else
    std::cerr << "[Metal] Metal support not compiled in" << std::endl;
    return false;
#endif
}

bool MetalBackend::mine(const WorkPackage& work, MiningResult& result) {
#ifdef ENABLE_METAL
    if (!initialized_ || !pipeline_state_) {
        std::cerr << "[Metal] Device or pipeline not initialized" << std::endl;
        result.found = false;
        result.hashes_tried = 0;
        return false;
    }

    @autoreleasepool {
        auto start_time = std::chrono::high_resolution_clock::now();

        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
        id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)pipeline_state_;
        id<MTLBuffer> mtl_header = (__bridge id<MTLBuffer>)buf_header_;
        id<MTLBuffer> mtl_target = (__bridge id<MTLBuffer>)buf_target_;
        id<MTLBuffer> mtl_result_nonce = (__bridge id<MTLBuffer>)buf_result_nonce_;
        id<MTLBuffer> mtl_result_found = (__bridge id<MTLBuffer>)buf_result_found_;
        id<MTLBuffer> mtl_nonce_start = (__bridge id<MTLBuffer>)buf_nonce_start_;

        // Copy work data to shared buffers
        memcpy([mtl_header contents], work.header, DINERO_HEADER_SIZE_BYTES);
        memcpy([mtl_target contents], work.target, 32);

        // Initialize result to 0
        uint32_t zero = 0;
        memcpy([mtl_result_found contents], &zero, sizeof(uint32_t));
        memcpy([mtl_result_nonce contents], &zero, sizeof(uint32_t));

        // Set nonce start
        memcpy([mtl_nonce_start contents], &work.nonce_start, sizeof(uint32_t));

        // Create command buffer and encoder
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        if (!command_buffer) {
            std::cerr << "[Metal] Failed to create command buffer" << std::endl;
            result.found = false;
            result.hashes_tried = 0;
            return false;
        }

        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (!encoder) {
            std::cerr << "[Metal] Failed to create compute encoder" << std::endl;
            result.found = false;
            result.hashes_tried = 0;
            return false;
        }

        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:mtl_header offset:0 atIndex:0];
        [encoder setBuffer:mtl_target offset:0 atIndex:1];
        [encoder setBuffer:mtl_result_nonce offset:0 atIndex:2];
        [encoder setBuffer:mtl_result_found offset:0 atIndex:3];
        [encoder setBuffer:mtl_nonce_start offset:0 atIndex:4];

        // Dispatch threads
        uint64_t nonce_count = work.getNonceCount();
        NSUInteger thread_count = static_cast<NSUInteger>(nonce_count);
        NSUInteger threadgroup_size = max_threadgroup_size_;

        // Metal requires grid to be a multiple of threadgroup size for non-uniform grids,
        // or we use dispatchThreads (macOS 10.15+ / iOS 13+) for non-uniform
        MTLSize grid_size = MTLSizeMake(thread_count, 1, 1);
        MTLSize tg_size = MTLSizeMake(threadgroup_size, 1, 1);

        [encoder dispatchThreads:grid_size threadsPerThreadgroup:tg_size];
        [encoder endEncoding];

        // Submit and wait
        [command_buffer commit];
        [command_buffer waitUntilCompleted];

        // Check for GPU errors
        if ([command_buffer status] == MTLCommandBufferStatusError) {
            std::cerr << "[Metal] Command buffer error: "
                      << [[[command_buffer error] localizedDescription] UTF8String] << std::endl;
            result.found = false;
            result.hashes_tried = 0;
            return false;
        }

        // Read results
        uint32_t found_flag = 0;
        memcpy(&found_flag, [mtl_result_found contents], sizeof(uint32_t));

        result.found = (found_flag != 0);
        result.hashes_tried = nonce_count;
        result.backend = BackendType::METAL;

        if (result.found) {
            memcpy(&result.nonce, [mtl_result_nonce contents], sizeof(uint32_t));
            std::cout << "[Metal] Solution found! Nonce: 0x" << std::hex << result.nonce << std::dec << std::endl;
        }

        // Calculate hashrate
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        if (duration > 0) {
            hashrate_ = (static_cast<double>(nonce_count) / duration) * 1000000.0;
        }

        return true;
    }
#else
    std::cerr << "[Metal] Metal support not compiled in" << std::endl;
    result.found = false;
    result.hashes_tried = 0;
    return false;
#endif
}

void MetalBackend::stop() {
#ifdef ENABLE_METAL
    // Release Metal objects (bridge_retained -> must CFRelease)
    if (buf_nonce_start_) { CFRelease(buf_nonce_start_); buf_nonce_start_ = nullptr; }
    if (buf_result_found_) { CFRelease(buf_result_found_); buf_result_found_ = nullptr; }
    if (buf_result_nonce_) { CFRelease(buf_result_nonce_); buf_result_nonce_ = nullptr; }
    if (buf_target_) { CFRelease(buf_target_); buf_target_ = nullptr; }
    if (buf_header_) { CFRelease(buf_header_); buf_header_ = nullptr; }
    if (pipeline_state_) { CFRelease(pipeline_state_); pipeline_state_ = nullptr; }
    if (library_) { CFRelease(library_); library_ = nullptr; }
    if (command_queue_) { CFRelease(command_queue_); command_queue_ = nullptr; }
    if (device_) { CFRelease(device_); device_ = nullptr; }
#endif
    initialized_ = false;
    hashrate_ = 0.0;
}

double MetalBackend::getHashrate() const {
    return hashrate_;
}

std::string MetalBackend::getDeviceName() const {
    return device_name_.empty() ? "Metal (not initialized)" : device_name_;
}

} // namespace dinero::gpu
