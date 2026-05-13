#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace dinero {
namespace solo {

// Number of winner-nonces a single GPU dispatch can return. The kernel
// records every nonce that satisfies the share target via atomicAdd into
// an array of this size (rather than the older atomicCAS-once shape that
// silently dropped winners past the first). At realistic share targets
// only 0-1 nonces win per dispatch, so 64 is comfortably oversized; the
// per-batch overflow log fires only when the target is unrealistically
// loose (e.g. self-test permissive mode).
inline constexpr uint32_t GPU_RESULT_CAPACITY = 64;

// Result of a single GPU mining dispatch.
struct GpuDispatchOutcome {
    // Total winners detected by the kernel. May exceed GPU_RESULT_CAPACITY
    // when the target is permissive — in that case the host has only the
    // first GPU_RESULT_CAPACITY nonces and the rest are dropped. The
    // backend logs an overflow line in that case so the operator can
    // adjust batch size or target.
    uint32_t total_count = 0;

    // Number of valid entries in `nonces` — min(total_count, capacity).
    uint32_t found_count = 0;

    // Winning nonces, in the order the kernel observed them.
    uint32_t nonces[GPU_RESULT_CAPACITY] = {};

    // Wall-clock dispatch latency for hashrate measurement.
    double elapsed_ms = 0.0;
};

// Abstract GPU mining backend. Concrete implementations live in
// src/gpu/cuda_backend.cpp (Windows + Linux NVIDIA), and
// src/gpu/metal_backend.mm (macOS Apple Silicon).
class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;

    // Dispatch one batch. `header` is the 128-byte block header with the
    // nonce field at byte offset 112 (the kernel overwrites this per
    // thread with `nonce_start + tid`). `target` is the 32-byte big-endian
    // target. `batch_size` is the number of nonces to try; the kernel
    // launches `batch_size` threads and returns early for indices past
    // the bound, so callers may use any batch_size including non-multiples
    // of the kernel's threads-per-block.
    virtual GpuDispatchOutcome dispatch(const uint8_t header[128],
                                        const uint8_t target[32],
                                        uint32_t nonce_start,
                                        uint32_t batch_size) = 0;

    // Human-readable device label, e.g. "NVIDIA GeForce RTX 4060 Laptop GPU".
    virtual const std::string& deviceName() const = 0;

    // Compute architecture string, e.g. "sm_89" for Ada Lovelace, "metal_3"
    // on macOS. Used for diagnostic logs.
    virtual const std::string& computeArch() const = 0;

    // Backend identifier, e.g. "cuda", "metal".
    virtual std::string backendName() const = 0;
};

// Try available GPU backends in priority order and return the first that
// initialises successfully. Priority on each platform:
//   macOS:   metal -> nullptr
//   Windows: cuda  -> nullptr
//   Linux:   cuda  -> nullptr
// Returns nullptr if no backend is compiled in or no compatible device
// is present. The miner falls back to CPU mining in that case.
std::unique_ptr<IGpuBackend> CreateGpuBackend();

} // namespace solo
} // namespace dinero
