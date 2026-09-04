#pragma once

#include "types.h"
#include "build_identity.h"
#include "rpc_client.h"
#include "work_template.h"
#include <functional>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>
#include <string>

namespace dinero {
namespace solo {

#ifdef DINERO_SOLO_HAS_GPU
class IGpuBackend;  // forward-declared from solo_miner/gpu_backend.h
#endif

enum class MinerBackend {
    Auto,
    Cpu,
    Metal,
    Cuda,
    OpenCl,
};

std::string minerBackendToString(MinerBackend backend);
MinerBackend minerBackendFromString(const std::string& backend);

/**
 * Miner configuration
 */
struct MinerConfig {
    std::string rpc_url = "http://127.0.0.1:20998";
    std::string cookie_path;
    std::string rpc_user;
    std::string rpc_password;
    std::string payout_address;     // Required - mining rewards go here
    int threads = 0;                // 0 = auto-detect (hardware_concurrency)
    // Template refresh poll interval. The daemon's longpollid field in
    // getblocktemplate is currently cosmetic (returns current best hash,
    // does NOT hold the response open until tip changes), so the miner
    // polls on this interval to detect tip advance.
    //
    // The original 5000ms default meant a 2+ miner setup wasted up to 5
    // full seconds hashing stale work after every competitor's block was
    // accepted (orphan-rate cost proportional to poll interval / block
    // interval). Reduced to 500ms to cut that window by 10x. Proper fix
    // is server-side real long-polling on the daemon's getblocktemplate;
    // until that lands, 500ms is the reasonable floor balancing latency
    // against local-RPC load.
    int template_refresh_ms = 500;

    // Runtime backend selection. Auto prefers CUDA on Windows/Linux NVIDIA,
    // Metal on macOS, then OpenCL for other GPUs, with CPU fallback if no GPU
    // backend is compiled in or detected.
    MinerBackend backend = MinerBackend::Auto;
    uint32_t gpu_device_id = 0;
    uint32_t gpu_batch_size = 1u << 20; // nonces per GPU dispatch
};

/**
 * Mining statistics
 */
struct MinerStats {
    double hashrate = 0.0;           // Current hashrate (H/s)
    uint64_t hashes_total = 0;       // Total hashes computed
    uint32_t blocks_found = 0;       // Blocks found (locally valid)
    uint32_t blocks_accepted = 0;    // Blocks accepted by network
    uint32_t blocks_rejected = 0;    // Blocks rejected by network
    uint32_t current_height = 0;     // Current mining height
    uint32_t difficulty_bits = 0;    // Current difficulty (compact)
    MinerBackend active_backend = MinerBackend::Cpu;
    std::chrono::steady_clock::time_point start_time;
};

/**
 * Block found details passed to the BlockFoundCb callback
 */
struct BlockFoundInfo {
    std::string block_hash;
    uint32_t height = 0;
    uint32_t nonce = 0;
    std::string prev_hash;
    std::string merkle_root;
    std::string utreexo_root;
    uint32_t version = 0;
    uint64_t timestamp = 0;
    uint32_t nbits = 0;
    double hashrate = 0.0;  // Snapshot in H/s when the valid block was found.
};

/**
 * One genuine candidate from the miner's current template.  The nonce hint is
 * published infrequently by the hot loop; sampleCandidate() performs the one
 * display-only SHA256d away from that loop.
 */
struct CandidateSample {
    uint32_t nonce = 0;
    Hash256 hash{};
    uint32_t version = 0;
    std::string prev_hash;
    std::string merkle_root;
    std::string utreexo_root;
    uint64_t timestamp = 0;
    uint32_t height = 0;
    uint32_t difficulty_bits = 0;
};

/**
 * Callback types
 */
using HashrateCb = std::function<void(double hashrate)>;
using BlockFoundCb = std::function<void(const BlockFoundInfo& info)>;
using ErrorCb = std::function<void(const std::string& error)>;
using TemplateCb = std::function<void(uint32_t height, uint32_t difficulty_bits)>;

/**
 * Solo miner - mines directly via RPC without stratum
 *
 * Library-first design:
 * - Embeddable in Qt, CLI, or any C++ application
 * - Reports progress via callbacks
 * - Thread-safe start/stop lifecycle
 */
class SoloMiner {
public:
    enum class SubmitResult {
        Accepted,
        Rejected,
        Unknown,
    };

    SoloMiner();
    ~SoloMiner();

    // Disable copy
    SoloMiner(const SoloMiner&) = delete;
    SoloMiner& operator=(const SoloMiner&) = delete;

    /**
     * Set callbacks for mining events
     */
    void setHashrateCallback(HashrateCb cb) { on_hashrate_ = std::move(cb); }
    void setBlockFoundCallback(BlockFoundCb cb) { on_block_found_ = std::move(cb); }
    void setErrorCallback(ErrorCb cb) { on_error_ = std::move(cb); }
    void setTemplateCallback(TemplateCb cb) { on_template_ = std::move(cb); }

    /**
     * Start mining
     * @param config Miner configuration
     * @return true if started successfully
     */
    bool start(const MinerConfig& config);

    /**
     * Stop mining (blocks until stopped)
     */
    void stop();

    /**
     * Check if mining is active
     */
    bool isRunning() const { return running_.load(); }

    /**
     * Get current statistics
     */
    MinerStats getStats() const;

    /** Return a display sample for the active template, or false between jobs. */
    bool sampleCandidate(CandidateSample& out) const;

    /**
     * Get last error message
     */
    std::string getLastError() const;

    /**
     * Benchmark report — one entry per backend exercised.
     */
    struct BenchmarkResult {
        MinerBackend backend = MinerBackend::Cpu;
        std::string backend_label;     // e.g. "cuda", "metal", "opencl", "cpu"
        std::string device_name;       // e.g. "NVIDIA GeForce RTX 4060 Laptop GPU"
        uint64_t total_hashes = 0;
        double duration_seconds = 0.0;
        double hashrate_mhs = 0.0;     // total_hashes / duration / 1e6
    };

    /**
     * Pure throughput measurement — no daemon, no submit, no chain.
     * Initializes the requested backend, hashes a synthetic 128-byte header
     * against a near-impossible target for `duration_seconds`, and reports
     * total hashes / wall-clock. The target is chosen so winners are
     * effectively never found; the GPU just keeps dispatching at full speed.
     *
     * Used by `dinero-solo-miner --benchmark` to close the v8 release-gate
     * gap of "real GPU MH/s unmeasurable at regtest" (every regtest dispatch
     * wins, bottlenecking on RPC, not the kernel).
     *
     * Returns true on success; check `out` for the measurement.
     */
    bool benchmark(MinerBackend backend, double duration_seconds,
                   BenchmarkResult& out);

private:
    /**
     * Template refresh thread - periodically fetches new work
     */
    void templateThread();

    /**
     * Mining worker thread
     */
    void minerThread(int thread_id, uint32_t start_nonce, uint32_t stride);

    /**
     * GPU mining worker thread. Uses the same WorkTemplate/submit path as CPU
     * mining and CPU-verifies any winning nonce before submitting the block.
     */
    void gpuMinerThread();

    bool initializeGpuBackend();

    /**
     * Submit found block using the exact work template that was mined.
     * CRITICAL: Must use the mined template, not current_work_ which may
     * have been refreshed by templateThread (race → bad-pow).
     */
    SubmitResult submitBlock(uint32_t nonce, const std::shared_ptr<WorkTemplate>& work);

    /**
     * Report error via callback
     */
    void reportError(const std::string& error);

    // Configuration
    MinerConfig config_;
    MinerBackend active_backend_ = MinerBackend::Cpu;

    // RPC client
    std::unique_ptr<RpcClient> rpc_;

#ifdef DINERO_SOLO_HAS_GPU
    std::unique_ptr<IGpuBackend> gpu_backend_;
#endif

    // Current work template
    std::shared_ptr<WorkTemplate> current_work_;
    mutable std::mutex work_mutex_;

    // Mining state
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // Statistics
    std::atomic<uint64_t> hashes_total_{0};
    std::atomic<uint32_t> blocks_found_{0};
    std::atomic<uint32_t> blocks_accepted_{0};
    std::atomic<uint32_t> blocks_rejected_{0};
    std::atomic<bool> submit_in_flight_{false};
    std::atomic<uint32_t> candidate_nonce_hint_{0};
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_hashrate_time_;
    uint64_t last_hashrate_hashes_ = 0;
    double current_hashrate_ = 0.0;
    mutable std::mutex stats_mutex_;

    // Error tracking
    std::string last_error_;
    mutable std::mutex error_mutex_;

    // Threads
    std::thread template_thread_;
    std::vector<std::thread> miner_threads_;

    // Callbacks
    HashrateCb on_hashrate_;
    BlockFoundCb on_block_found_;
    ErrorCb on_error_;
    TemplateCb on_template_;
};

} // namespace solo
} // namespace dinero
