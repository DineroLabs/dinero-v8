#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

namespace dinero::gpu {

/**
 * @brief GPU backend types supported by Dinero
 */
enum class BackendType {
    NONE,      // No GPU backend available
    OPENCL,    // OpenCL (AMD, Intel, NVIDIA portable)
    CUDA,      // CUDA (NVIDIA optimized)
    METAL      // Metal (Apple Silicon - future)
};

/**
 * @brief Represents a detected GPU device
 */
struct GPUDevice {
    BackendType backend;
    uint32_t device_id;
    std::string name;
    std::string vendor;
    size_t global_memory_mb;
    uint32_t compute_units;
    uint32_t max_clock_mhz;
    bool available;

    GPUDevice()
        : backend(BackendType::NONE)
        , device_id(0)
        , global_memory_mb(0)
        , compute_units(0)
        , max_clock_mhz(0)
        , available(false)
    {}
};

/**
 * @brief Work package for GPU mining (also used by CPU for cooperative mining)
 *
 * Contains the block header and mining parameters.
 * The miner (CPU or GPU) will hash the header with different nonces
 * until it finds a hash below the target.
 *
 * For cooperative CPU+GPU mining, the nonce_start and nonce_end define
 * disjoint ranges so CPU and GPU don't waste effort on duplicate nonces.
 */
struct WorkPackage {
    uint32_t header[32];    // 128-byte block header (BlockHeader v1: version, prev_hash, merkle_root, utreexo, timestamp, difficulty, nonce, reserved)
    uint32_t target[8];     // 256-bit target (hash must be < target)
    uint32_t nonce_start;   // Starting nonce for this work batch (inclusive)
    uint32_t nonce_end;     // Ending nonce for this work batch (inclusive)
    BackendType backend;    // Which backend is working on this range
    uint32_t device_id;     // Device ID (for multi-GPU)

    WorkPackage()
        : nonce_start(0)
        , nonce_end(UINT32_MAX)
        , backend(BackendType::NONE)
        , device_id(0)
    {
        memset(header, 0, sizeof(header));
        memset(target, 0, sizeof(target));
    }

    // Helper: Get the number of nonces to try in this range
    uint64_t getNonceCount() const {
        return static_cast<uint64_t>(nonce_end) - static_cast<uint64_t>(nonce_start) + 1;
    }
};

/**
 * @brief Result from mining (CPU or GPU)
 */
struct MiningResult {
    bool found;             // True if solution found
    uint32_t nonce;         // Winning nonce
    uint32_t hash[8];       // Resulting hash (for verification)
    uint64_t hashes_tried;  // Number of hashes computed
    BackendType backend;    // Which backend found the solution
    uint32_t device_id;     // Device ID (for multi-GPU)

    MiningResult()
        : found(false)
        , nonce(0)
        , hashes_tried(0)
        , backend(BackendType::NONE)
        , device_id(0)
    {
        memset(hash, 0, sizeof(hash));
    }
};

/**
 * @brief Abstract interface for GPU compute backends
 *
 * This interface is implemented by:
 * - OpenCLBackend (AMD, Intel, portable NVIDIA)
 * - CUDABackend (NVIDIA optimized)
 * - MetalBackend (Apple Silicon - future)
 */
class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;

    /**
     * @brief Enumerate all available devices for this backend
     * @return Vector of detected GPU devices
     */
    virtual std::vector<GPUDevice> enumerateDevices() = 0;

    /**
     * @brief Initialize and select a specific device
     * @param device_id Device index from enumerateDevices()
     * @return true if initialization successful
     */
    virtual bool initDevice(uint32_t device_id) = 0;

    /**
     * @brief Compile and load the mining kernel
     * @param kernel_source Source code or path to kernel
     * @return true if kernel compiled successfully
     */
    virtual bool compileKernel(const std::string& kernel_source) = 0;

    /**
     * @brief Execute mining work on GPU
     * @param work Work package containing header and target
     * @param result Output result if solution found
     * @return true if no errors (check result.found for solution)
     */
    virtual bool mine(const WorkPackage& work, MiningResult& result) = 0;

    /**
     * @brief Stop mining and release resources
     */
    virtual void stop() = 0;

    /**
     * @brief Get current hashrate in H/s
     * @return Hashrate in hashes per second
     */
    virtual double getHashrate() const = 0;

    /**
     * @brief Get human-readable device name
     * @return Device name string
     */
    virtual std::string getDeviceName() const = 0;

    /**
     * @brief Get backend type
     * @return BackendType enum value
     */
    virtual BackendType getBackendType() const = 0;
};

/**
 * @brief Factory function to create appropriate backend
 * @param backend Desired backend type
 * @return Unique pointer to backend implementation, or nullptr if unavailable
 */
std::unique_ptr<IComputeBackend> createBackend(BackendType backend);

/**
 * @brief Convert backend type to string
 */
inline std::string backendToString(BackendType backend) {
    switch (backend) {
        case BackendType::NONE:   return "None";
        case BackendType::OPENCL: return "OpenCL";
        case BackendType::CUDA:   return "CUDA";
        case BackendType::METAL:  return "Metal";
        default:                  return "Unknown";
    }
}

} // namespace dinero::gpu
