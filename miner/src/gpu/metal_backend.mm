// Metal implementation of IGpuBackend.
//
// Build-gated: only compiled when -DMINER_ENABLE_METAL=ON on macOS. The
// Metal shader source is embedded from shaders/sha256d.metal at configure
// time, so the standalone miner does not need to ship a separate .metal or
// .metallib file.

#ifdef MINER_ENABLE_METAL

#include "solo_miner/gpu_backend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace dinero {
namespace solo {
namespace gpu {

constexpr uint32_t THREADS_PER_GROUP = 256u;

extern const char* SHA256D_METAL_SRC;

namespace {

class MetalBackend : public IGpuBackend {
public:
    static std::unique_ptr<MetalBackend> create();

    ~MetalBackend() override = default;

    GpuDispatchOutcome dispatch(const uint8_t header[128],
                                const uint8_t target[32],
                                uint32_t nonce_start,
                                uint32_t batch_size) override;

    const std::string& deviceName() const override { return device_name_; }
    const std::string& computeArch() const override { return compute_arch_; }
    std::string backendName() const override { return "metal"; }

private:
    MetalBackend() = default;

    __strong id<MTLDevice> device_ = nil;
    __strong id<MTLCommandQueue> command_queue_ = nil;
    __strong id<MTLLibrary> library_ = nil;
    __strong id<MTLComputePipelineState> pipeline_ = nil;
    __strong id<MTLBuffer> header_buffer_ = nil;
    __strong id<MTLBuffer> target_buffer_ = nil;
    __strong id<MTLBuffer> result_nonces_buffer_ = nil;
    __strong id<MTLBuffer> result_count_buffer_ = nil;

    uint32_t threads_per_group_ = THREADS_PER_GROUP;
    std::string device_name_;
    std::string compute_arch_ = "metal";
    std::mutex dispatch_mu_;
};

std::unique_ptr<MetalBackend> MetalBackend::create() {
    @autoreleasepool {
        auto backend = std::unique_ptr<MetalBackend>(new MetalBackend());

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "[metal] no Metal-capable device detected" << std::endl;
            return nullptr;
        }
        backend->device_ = device;
        backend->device_name_ = [[device name] UTF8String];

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            std::cerr << "[metal] failed to create command queue" << std::endl;
            return nullptr;
        }
        backend->command_queue_ = queue;

        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:SHA256D_METAL_SRC];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];

        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                      options:options
                                                        error:&error];
        if (!library) {
            std::cerr << "[metal] failed to compile kernel: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return nullptr;
        }
        backend->library_ = library;

        id<MTLFunction> function = [library newFunctionWithName:@"sha256d_mine"];
        if (!function) {
            std::cerr << "[metal] failed to find sha256d_mine in Metal library"
                      << std::endl;
            return nullptr;
        }

        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            std::cerr << "[metal] failed to create pipeline: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return nullptr;
        }
        backend->pipeline_ = pipeline;

        const NSUInteger max_threads = [pipeline maxTotalThreadsPerThreadgroup];
        backend->threads_per_group_ =
            std::max<uint32_t>(1, std::min<uint32_t>(THREADS_PER_GROUP,
                                                     static_cast<uint32_t>(max_threads)));
        backend->compute_arch_ = "metal_3";

        backend->header_buffer_ =
            [device newBufferWithLength:128 options:MTLResourceStorageModeShared];
        backend->target_buffer_ =
            [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
        backend->result_nonces_buffer_ =
            [device newBufferWithLength:GPU_RESULT_CAPACITY * sizeof(uint32_t)
                                options:MTLResourceStorageModeShared];
        backend->result_count_buffer_ =
            [device newBufferWithLength:sizeof(uint32_t)
                                options:MTLResourceStorageModeShared];

        if (!backend->header_buffer_ || !backend->target_buffer_ ||
            !backend->result_nonces_buffer_ || !backend->result_count_buffer_) {
            std::cerr << "[metal] failed to allocate shared buffers" << std::endl;
            return nullptr;
        }

        return backend;
    }
}

GpuDispatchOutcome MetalBackend::dispatch(const uint8_t header[128],
                                          const uint8_t target[32],
                                          uint32_t nonce_start,
                                          uint32_t batch_size) {
    GpuDispatchOutcome result{};
    if (batch_size == 0) return result;

    std::lock_guard<std::mutex> lock(dispatch_mu_);

    @autoreleasepool {
        uint32_t header_words[32];
        for (int i = 0; i < 32; i++) {
            header_words[i] =
                static_cast<uint32_t>(header[i * 4]) |
                (static_cast<uint32_t>(header[i * 4 + 1]) << 8) |
                (static_cast<uint32_t>(header[i * 4 + 2]) << 16) |
                (static_cast<uint32_t>(header[i * 4 + 3]) << 24);
        }

        uint32_t target_words[8];
        for (int i = 0; i < 8; i++) {
            target_words[i] =
                (static_cast<uint32_t>(target[i * 4]) << 24) |
                (static_cast<uint32_t>(target[i * 4 + 1]) << 16) |
                (static_cast<uint32_t>(target[i * 4 + 2]) << 8) |
                static_cast<uint32_t>(target[i * 4 + 3]);
        }

        std::memcpy([header_buffer_ contents], header_words, sizeof(header_words));
        std::memcpy([target_buffer_ contents], target_words, sizeof(target_words));

        uint32_t zero_count = 0;
        std::memcpy([result_count_buffer_ contents], &zero_count, sizeof(zero_count));

        id<MTLCommandBuffer> command_buffer = [command_queue_ commandBuffer];
        if (!command_buffer) {
            std::cerr << "[metal] failed to create command buffer" << std::endl;
            return result;
        }

        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (!encoder) {
            std::cerr << "[metal] failed to create compute encoder" << std::endl;
            return result;
        }

        uint32_t capacity = GPU_RESULT_CAPACITY;
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:header_buffer_ offset:0 atIndex:0];
        [encoder setBuffer:target_buffer_ offset:0 atIndex:1];
        [encoder setBytes:&nonce_start length:sizeof(nonce_start) atIndex:2];
        [encoder setBytes:&batch_size length:sizeof(batch_size) atIndex:3];
        [encoder setBuffer:result_nonces_buffer_ offset:0 atIndex:4];
        [encoder setBuffer:result_count_buffer_ offset:0 atIndex:5];
        [encoder setBytes:&capacity length:sizeof(capacity) atIndex:6];

        NSUInteger thread_count = static_cast<NSUInteger>(batch_size);
        NSUInteger tg = std::min<NSUInteger>(threads_per_group_, thread_count);
        MTLSize grid_size = MTLSizeMake(thread_count, 1, 1);
        MTLSize tg_size = MTLSizeMake(tg, 1, 1);

        auto t0 = std::chrono::steady_clock::now();
        [encoder dispatchThreads:grid_size threadsPerThreadgroup:tg_size];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        auto t1 = std::chrono::steady_clock::now();

        if ([command_buffer status] == MTLCommandBufferStatusError) {
            NSError* error = [command_buffer error];
            std::cerr << "[metal] command buffer failed: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return result;
        }

        uint32_t total_count = 0;
        std::memcpy(&total_count, [result_count_buffer_ contents], sizeof(total_count));
        result.total_count = total_count;
        result.found_count = std::min<uint32_t>(total_count, GPU_RESULT_CAPACITY);

        if (result.found_count > 0) {
            std::memcpy(result.nonces,
                        [result_nonces_buffer_ contents],
                        result.found_count * sizeof(uint32_t));
        }

        if (total_count > GPU_RESULT_CAPACITY) {
            std::cerr << "[metal] dispatch produced " << total_count
                      << " winners (>" << GPU_RESULT_CAPACITY
                      << " capacity); " << (total_count - GPU_RESULT_CAPACITY)
                      << " dropped. Share target may be too loose; consider"
                      << " a smaller batch_size." << std::endl;
        }

        result.elapsed_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        return result;
    }
}

} // namespace

std::unique_ptr<IGpuBackend> CreateMetalBackend() {
    return MetalBackend::create();
}

} // namespace gpu
} // namespace solo
} // namespace dinero

#endif // MINER_ENABLE_METAL
