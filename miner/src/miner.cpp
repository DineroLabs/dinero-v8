#include "solo_miner/miner.h"
#include "solo_miner/hash_engine.h"
#include <algorithm>
#include <cstring>

#ifdef DINERO_SOLO_HAS_GPU
#include "solo_miner/gpu_backend.h"
// v8.0.0 monorepo port: solo miner uses its own dinero::solo::IGpuBackend
// (NVRTC-based CUDA + MSL-based Metal) instead of the daemon's
// dinero::gpu::IComputeBackend. The two stacks coexist; see
// miner/CMakeLists.txt for the architectural reasoning.
namespace dinero {
namespace solo {
namespace gpu {
#ifdef MINER_ENABLE_CUDA
std::unique_ptr<IGpuBackend> CreateCudaBackend();
#endif
#ifdef MINER_ENABLE_METAL
std::unique_ptr<IGpuBackend> CreateMetalBackend();
#endif
#ifdef MINER_ENABLE_OPENCL
std::unique_ptr<IGpuBackend> CreateOpenClBackend();
#endif
}  // namespace gpu
}  // namespace solo
}  // namespace dinero
#endif

namespace dinero {
namespace solo {

namespace {

std::string inferNetworkFromAddress(const std::string& address) {
    if (address.rfind("din1", 0) == 0) {
        return "mainnet";
    }
    if (address.rfind("tdin1", 0) == 0) {
        return "testnet";
    }
    if (address.rfind("rdin1", 0) == 0) {
        return "regtest";
    }
    return "";
}

std::string describeTemplateFailure(const nlohmann::json& tmpl) {
    if (!tmpl.is_object()) {
        return "Failed to parse block template";
    }

    const std::string code = tmpl.value("code", "");
    const std::string error = tmpl.value("error", "");
    const std::string reason = tmpl.value("reason", "");

    if (!error.empty()) {
        if (!code.empty()) {
            return error + " (" + code + ")";
        }
        return error;
    }

    if (!reason.empty()) {
        if (!code.empty()) {
            return "Mining paused: " + reason + " (" + code + ")";
        }
        return "Mining paused: " + reason;
    }

    if (!code.empty()) {
        return "Template unavailable: " + code;
    }

    return "Failed to parse block template";
}

#ifdef DINERO_SOLO_HAS_GPU
// dinero::solo::IGpuBackend takes raw byte arrays directly; no word-array
// conversion or BackendType enum mapping needed (removed v8 port — these
// helpers were artifacts of the dinero::gpu::IComputeBackend integration).
MinerBackend backendNameToEnum(const std::string& name) {
    if (name == "cuda")   return MinerBackend::Cuda;
    if (name == "metal")  return MinerBackend::Metal;
    if (name == "opencl") return MinerBackend::OpenCl;
    return MinerBackend::Cpu;
}
#endif

} // namespace

SoloMiner::SoloMiner() = default;

SoloMiner::~SoloMiner() {
    if (running_) {
        stop();
    }
}

bool SoloMiner::start(const MinerConfig& config) {
    if (running_) {
        reportError("Miner is already running");
        return false;
    }

    config_ = config;
    active_backend_ = MinerBackend::Cpu;

    // Validate configuration
    if (config_.payout_address.empty()) {
        reportError("Payout address is required");
        return false;
    }

    // Create RPC client
    RpcConfig rpc_config;
    rpc_config.url = config_.rpc_url;
    rpc_config.cookie_path = config_.cookie_path;
    rpc_config.user = config_.rpc_user;
    rpc_config.password = config_.rpc_password;

    rpc_ = std::make_unique<RpcClient>(rpc_config);

    // Test connection
    if (!rpc_->isConnected()) {
        reportError("Cannot connect to daemon: " + rpc_->getLastError());
        return false;
    }

    ChainSafetyCheck safety = rpc_->verifyChainSafety();
    if (!safety.safe) {
        reportError("Chain safety check failed: " + safety.error);
        return false;
    }

    const std::string address_network = inferNetworkFromAddress(config_.payout_address);
    if (!address_network.empty() && address_network != safety.network) {
        reportError("Address/network mismatch: payout address is " + address_network +
                    " but daemon is " + safety.network);
        return false;
    }

    // Determine thread count
    int threads = config_.threads;
    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
        if (threads <= 0) threads = 1;
    }

    // Fetch initial template synchronously so "start" only succeeds once
    // the miner has actual work to hash.
    {
        auto tmpl = rpc_->getBlockTemplate(config_.payout_address);
        if (!tmpl) {
            reportError("Failed to get initial block template: " + rpc_->getLastError());
            return false;
        }

        auto work = WorkTemplate::fromJson(*tmpl);
        if (!work || !work->isValid()) {
            reportError(describeTemplateFailure(*tmpl));
            return false;
        }

        std::lock_guard<std::mutex> lock(work_mutex_);
        current_work_ = std::make_shared<WorkTemplate>(std::move(*work));
    }

    if (!initializeGpuBackend()) {
        return false;
    }

    // Initialize state
    running_ = true;
    stopping_ = false;
    hashes_total_ = 0;
    blocks_found_ = 0;
    blocks_accepted_ = 0;
    blocks_rejected_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    last_hashrate_time_ = start_time_;
    last_hashrate_hashes_ = 0;

    if (on_template_ && current_work_) {
        on_template_(current_work_->height, current_work_->difficulty_bits);
    }

    // Start template refresh thread
    template_thread_ = std::thread(&SoloMiner::templateThread, this);

    // Start miner threads
    if (active_backend_ == MinerBackend::Cpu) {
        miner_threads_.reserve(threads);
        for (int i = 0; i < threads; i++) {
            uint32_t start_nonce = static_cast<uint32_t>(i);
            uint32_t stride = static_cast<uint32_t>(threads);
            miner_threads_.emplace_back(&SoloMiner::minerThread, this, i, start_nonce, stride);
        }
    } else {
        miner_threads_.emplace_back(&SoloMiner::gpuMinerThread, this);
    }

    return true;
}

void SoloMiner::stop() {
    if (!running_) return;

    stopping_ = true;
    running_ = false;

    // Wait for template thread
    if (template_thread_.joinable()) {
        template_thread_.join();
    }

    // Wait for miner threads
    for (auto& thread : miner_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    miner_threads_.clear();

    rpc_.reset();
    current_work_.reset();
#ifdef DINERO_SOLO_HAS_GPU
    // IGpuBackend has no explicit stop() — destructor releases device
    // resources (CUDA context, Metal device handle, kernel modules).
    gpu_backend_.reset();
#endif
    active_backend_ = MinerBackend::Cpu;
}

MinerStats SoloMiner::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    MinerStats stats;
    stats.hashes_total = hashes_total_.load();
    stats.blocks_found = blocks_found_.load();
    stats.blocks_accepted = blocks_accepted_.load();
    stats.blocks_rejected = blocks_rejected_.load();
    stats.hashrate = current_hashrate_;
    stats.active_backend = active_backend_;
    stats.start_time = start_time_;

    {
        std::lock_guard<std::mutex> work_lock(work_mutex_);
        if (current_work_) {
            stats.current_height = current_work_->height;
            stats.difficulty_bits = current_work_->difficulty_bits;
        }
    }

    return stats;
}

std::string SoloMiner::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool SoloMiner::initializeGpuBackend() {
    active_backend_ = MinerBackend::Cpu;

    if (config_.backend == MinerBackend::Cpu) {
        return true;
    }

#ifndef DINERO_SOLO_HAS_GPU
    // Built without any GPU backend (no -DMINER_ENABLE_CUDA=ON and no
    // -DMINER_ENABLE_METAL=ON). Auto silently falls back to CPU;
    // explicit selection fails loudly.
    if (config_.backend == MinerBackend::Auto) {
        return true;
    }
    reportError("Requested " + minerBackendToString(config_.backend) +
                " backend, but this solo miner was built without GPU support "
                "(rebuild with -DMINER_ENABLE_CUDA=ON or -DMINER_ENABLE_METAL=ON)");
    return false;
#else
    std::unique_ptr<IGpuBackend> backend;
    switch (config_.backend) {
        case MinerBackend::Auto:
            // CreateGpuBackend() walks the priority list (Metal on macOS,
            // CUDA on Windows/Linux NVIDIA, OpenCL fallback for AMD/Intel)
            // and returns the first that initializes a compatible device.
            // nullptr → no GPU available, silent CPU fallback per the
            // "Auto means best effort" contract.
            backend = CreateGpuBackend();
            if (!backend) {
                return true;
            }
            break;

        case MinerBackend::Cuda:
#ifdef MINER_ENABLE_CUDA
            backend = gpu::CreateCudaBackend();
#endif
            if (!backend) {
                reportError("Requested cuda backend is not available "
                            "(binary built without -DMINER_ENABLE_CUDA=ON, "
                            "or no compatible NVIDIA GPU detected)");
                return false;
            }
            break;

        case MinerBackend::Metal:
#ifdef MINER_ENABLE_METAL
            backend = gpu::CreateMetalBackend();
#endif
            if (!backend) {
                reportError("Requested metal backend is not available "
                            "(binary built without -DMINER_ENABLE_METAL=ON, "
                            "or no Metal-capable device detected)");
                return false;
            }
            break;

        case MinerBackend::OpenCl:
#ifdef MINER_ENABLE_OPENCL
            backend = gpu::CreateOpenClBackend();
#endif
            if (!backend) {
                reportError("Requested opencl backend is not available "
                            "(binary built without -DMINER_ENABLE_OPENCL=ON, "
                            "or no OpenCL GPU device detected)");
                return false;
            }
            break;

        case MinerBackend::Cpu:
        default:
            return true;
    }

    active_backend_ = backendNameToEnum(backend->backendName());
    gpu_backend_ = std::move(backend);
    return true;
#endif
}

void SoloMiner::templateThread() {
    // BIP22/BIP23 longpoll token persisted across iterations. Empty on
    // the first call (so the server returns immediately with the current
    // template). On each successful fetch, we take the `longpollid` from
    // the response and echo it back on the next call — the server blocks
    // the next call until the tip advances (or its longpoll timeout
    // fires, ~8s). Latency between "someone mines a block" and
    // "miner gets fresh template" drops from the old poll interval
    // (500ms-5000ms) down to the notify-condvar wake (~milliseconds).
    //
    // See include/rpc/longpoll_notifier.h on the daemon side (dinero
    // p2p-fix 8bad44f15) for the server-side plumbing.
    std::string last_longpollid;

    while (running_ && !stopping_) {
        if (submit_in_flight_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // Fetch template with longpoll. If last_longpollid matches the
        // server's current tip, the server parks this request until a
        // tip change or its longpoll timeout. If the client is behind
        // (tip already advanced past last_longpollid), the server
        // returns immediately with the new template.
        auto tmpl = rpc_->getBlockTemplate(config_.payout_address, last_longpollid);

        bool fetch_ok = false;
        if (tmpl) {
            auto work = WorkTemplate::fromJson(*tmpl);
            if (work && work->isValid()) {
                last_longpollid = work->longpollid;  // remember for next iteration

                {
                    std::lock_guard<std::mutex> lock(work_mutex_);
                    current_work_ = std::make_shared<WorkTemplate>(std::move(*work));
                }

                // Notify via callback
                if (on_template_) {
                    on_template_(current_work_->height, current_work_->difficulty_bits);
                }
                fetch_ok = true;
            } else {
                reportError(describeTemplateFailure(*tmpl));
            }
        } else {
            reportError("Failed to get block template: " + rpc_->getLastError());
        }

        // Calculate and report hashrate
        auto now = std::chrono::steady_clock::now();
        uint64_t current_hashes = hashes_total_.load();
        double elapsed = std::chrono::duration<double>(now - last_hashrate_time_).count();

        if (elapsed >= 1.0) {
            uint64_t hashes_delta = current_hashes - last_hashrate_hashes_;
            double hashrate = static_cast<double>(hashes_delta) / elapsed;

            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                current_hashrate_ = hashrate;
            }

            if (on_hashrate_) {
                on_hashrate_(hashrate);
            }

            last_hashrate_time_ = now;
            last_hashrate_hashes_ = current_hashes;
        }

        // Sleep policy:
        //   - On success: no sleep. The server's longpoll already parked
        //     us for up to ~8s waiting for a tip change; adding our own
        //     sleep on top just adds pointless latency.
        //   - On error: sleep config_.template_refresh_ms as an
        //     error-backoff floor, so we don't tight-loop against a
        //     broken or disconnected daemon.
        if (!fetch_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.template_refresh_ms));
        }
    }
}

void SoloMiner::minerThread(int thread_id, uint32_t start_nonce, uint32_t stride) {
    uint32_t nonce = start_nonce;
    std::shared_ptr<WorkTemplate> local_work;
    std::array<uint8_t, HEADER_SIZE> header{};
    uint32_t local_height = 0;

    while (running_ && !stopping_) {
        // Get current work
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            if (current_work_ && current_work_->isValid()) {
                if (!local_work || local_work->height != current_work_->height ||
                    local_work->prev_hash != current_work_->prev_hash ||
                    local_work->difficulty_bits != current_work_->difficulty_bits ||
                    local_work->timestamp != current_work_->timestamp ||
                    local_work->version != current_work_->version ||
                    local_work->merkle_root != current_work_->merkle_root ||
                    local_work->utreexo_root != current_work_->utreexo_root) {
                    local_work = current_work_;
                    local_height = local_work->height;
                    // Reset nonce for new work
                    nonce = start_nonce;
                }
            }
        }

        if (!local_work || !local_work->isValid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (submit_in_flight_.load()) {
            local_work.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        // Build header with current nonce
        header = local_work->buildHeader(nonce);

        // Hash and check
        Hash256 hash = HashEngine::hashHeader(header.data());

        // Compare hash to target (hash must be <= target)
        // Both hash (SHA256d output) and target are big-endian (MSB at byte[0]).
        if (hashMeetsTarget(hash, local_work->target)) {
            bool expected = false;
            if (!submit_in_flight_.compare_exchange_strong(expected, true)) {
                local_work.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }

            // Retire this template immediately so other threads stop hashing
            // the same candidate while submitblock is still in flight.
            {
                std::lock_guard<std::mutex> lock(work_mutex_);
                current_work_.reset();
            }

            // Found a valid block!
            blocks_found_.fetch_add(1);

            // Display hash in big-endian (leading zeros visible, like Bitcoin convention)
            std::string hash_str = bytesToHex(hash.data(), 32);

            if (on_block_found_) {
                BlockFoundInfo info;
                info.block_hash = hash_str;
                info.height = local_height;
                info.nonce = nonce;
                info.prev_hash = local_work->prev_hash;
                info.merkle_root = local_work->merkle_root;
                info.utreexo_root = local_work->utreexo_root;
                info.nbits = local_work->difficulty_bits;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    info.hashrate = current_hashrate_;
                }
                on_block_found_(info);
            }

            // Submit block using the SAME work template we mined against.
            // CRITICAL: Do NOT re-read current_work_ here — the template thread
            // may have refreshed it (new timestamp/merkle), causing the submitted
            // block header to differ from what we hashed → "bad-pow".
            const SubmitResult submit_result = submitBlock(nonce, local_work);
            if (submit_result == SubmitResult::Accepted) {
                blocks_accepted_.fetch_add(1);
            } else if (submit_result == SubmitResult::Rejected) {
                blocks_rejected_.fetch_add(1);
            }
            submit_in_flight_.store(false);
            local_work.reset();
        }

        // Update stats
        hashes_total_.fetch_add(1);

        // Next nonce
        nonce += stride;

        // Check for nonce overflow (very unlikely in practice)
        if (nonce < start_nonce) {
            // Wrapped around - wait for new work
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            nonce = start_nonce;
        }
    }
}

void SoloMiner::gpuMinerThread() {
#ifndef DINERO_SOLO_HAS_GPU
    reportError("GPU miner thread started without GPU backend support");
    return;
#else
    if (!gpu_backend_) {
        reportError("GPU miner thread started without an initialized backend");
        return;
    }

    uint32_t nonce_cursor = 0;
    std::shared_ptr<WorkTemplate> local_work;
    uint32_t local_height = 0;

    while (running_ && !stopping_) {
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            if (current_work_ && current_work_->isValid()) {
                if (!local_work || local_work->height != current_work_->height ||
                    local_work->prev_hash != current_work_->prev_hash ||
                    local_work->difficulty_bits != current_work_->difficulty_bits ||
                    local_work->timestamp != current_work_->timestamp ||
                    local_work->version != current_work_->version ||
                    local_work->merkle_root != current_work_->merkle_root ||
                    local_work->utreexo_root != current_work_->utreexo_root) {
                    local_work = current_work_;
                    local_height = local_work->height;
                    nonce_cursor = 0;
                }
            }
        }

        if (!local_work || !local_work->isValid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (submit_in_flight_.load()) {
            local_work.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        const uint32_t batch_size = std::max<uint32_t>(1, config_.gpu_batch_size);
        const uint32_t nonce_start = nonce_cursor;
        const uint64_t nonce_end_64 = static_cast<uint64_t>(nonce_cursor) + batch_size - 1;
        const uint32_t nonce_end = nonce_end_64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(nonce_end_64);

        // The kernel writes its own per-thread nonce at byte offset 112,
        // so the host's nonce field in the header bytes here doesn't
        // matter — pass any value, the kernel substitutes it.
        auto header = local_work->buildHeader(0);

        auto outcome = gpu_backend_->dispatch(header.data(),
                                              local_work->target.data(),
                                              nonce_start,
                                              batch_size);

        // The kernel actually launched batch_size threads, each computing
        // one full SHA-256d. Count them all toward total hashes — the
        // dispatch is the unit of work, not the winners.
        hashes_total_.fetch_add(batch_size);

        // Process winners. Each one is CPU-verified before submitting —
        // a bug in the kernel (target compare direction, byte order, etc.)
        // could produce a false positive that would land us submitting
        // an invalid block and getting it rejected. CPU re-verify catches
        // that immediately and we keep mining instead of churning
        // rejections. This is the test-vector identity safety anchor
        // (see qt design notes 2026-05-12).
        for (uint32_t i = 0; i < outcome.found_count; i++) {
            const uint32_t winning_nonce = outcome.nonces[i];

            auto verify_header = local_work->buildHeader(winning_nonce);
            Hash256 verify_hash = HashEngine::hashHeader(verify_header.data());

            if (!hashMeetsTarget(verify_hash, local_work->target)) {
                reportError("GPU backend false positive: nonce " +
                            std::to_string(winning_nonce) +
                            " did not meet target on CPU re-verify; "
                            "investigate kernel/backend bug");
                continue;
            }

            bool expected = false;
            if (!submit_in_flight_.compare_exchange_strong(expected, true)) {
                // Another worker took the slot; bail out of this batch.
                break;
            }

            {
                std::lock_guard<std::mutex> lock(work_mutex_);
                current_work_.reset();
            }

            blocks_found_.fetch_add(1);

            if (on_block_found_) {
                BlockFoundInfo info;
                info.block_hash = bytesToHex(verify_hash.data(), 32);
                info.height = local_height;
                info.nonce = winning_nonce;
                info.prev_hash = local_work->prev_hash;
                info.merkle_root = local_work->merkle_root;
                info.utreexo_root = local_work->utreexo_root;
                info.nbits = local_work->difficulty_bits;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    info.hashrate = current_hashrate_;
                }
                on_block_found_(info);
            }

            const SubmitResult submit_result = submitBlock(winning_nonce, local_work);
            if (submit_result == SubmitResult::Accepted) {
                blocks_accepted_.fetch_add(1);
            } else if (submit_result == SubmitResult::Rejected) {
                blocks_rejected_.fetch_add(1);
            }
            submit_in_flight_.store(false);
            local_work.reset();
            break;  // template invalidated; pick up new work next loop
        }

        if (nonce_end == UINT32_MAX) {
            local_work.reset();
            nonce_cursor = 0;
        } else {
            nonce_cursor = nonce_end + 1;
        }
    }
#endif
}

SoloMiner::SubmitResult SoloMiner::submitBlock(uint32_t nonce, const std::shared_ptr<WorkTemplate>& work) {
    if (!work) {
        reportError("No work template available for submission");
        return SubmitResult::Rejected;
    }

    std::string block_hex = work->buildBlock(nonce);
    bool accepted = rpc_->submitBlock(block_hex);

    if (accepted) {
        return SubmitResult::Accepted;
    }

    if (rpc_->lastCallTimedOut()) {
        reportError("Block submission timed out; acceptance unknown: " + rpc_->getLastError());
        return SubmitResult::Unknown;
    }

    reportError("Block rejected: " + rpc_->getLastError());
    return SubmitResult::Rejected;
}

bool SoloMiner::benchmark(MinerBackend backend, double duration_seconds,
                          BenchmarkResult& out) {
    out = BenchmarkResult{};
    out.backend = backend;
    out.backend_label = minerBackendToString(backend);

    if (duration_seconds <= 0.0) {
        reportError("benchmark duration must be > 0 seconds");
        return false;
    }

    // Synthetic 128-byte header. Contents don't matter for throughput
    // measurement — we just need 128 bytes that the kernel will hash.
    // The nonce field at offset 112 gets overwritten per-thread by the
    // GPU kernel; the CPU path increments its own counter.
    std::array<uint8_t, 128> header{};
    for (size_t i = 0; i < header.size(); ++i) {
        header[i] = static_cast<uint8_t>(i * 7u);
    }

    // Near-impossible target: 0x0000...0001. Winners are effectively never
    // found, so the kernel runs at full speed and the dispatch loop is
    // pure throughput — no submit overhead, no result-buffer overflow
    // warnings, no chain interaction.
    std::array<uint8_t, 32> target{};
    target[31] = 0x01;

    const auto duration = std::chrono::duration<double>(duration_seconds);
    const auto t_start = std::chrono::steady_clock::now();
    uint64_t total_hashes = 0;

    if (backend == MinerBackend::Cpu) {
        out.device_name = "CPU";
        uint32_t nonce = 0;
        while (std::chrono::steady_clock::now() - t_start < duration) {
            auto local_header = header;
            // Plant nonce at offset 112 (BlockHeader v1 nonce field).
            local_header[112] = static_cast<uint8_t>(nonce);
            local_header[113] = static_cast<uint8_t>(nonce >> 8);
            local_header[114] = static_cast<uint8_t>(nonce >> 16);
            local_header[115] = static_cast<uint8_t>(nonce >> 24);
            // hashHeader returns the SHA-256d — we don't check the result,
            // we just count completed hashes. HashEngine internally is the
            // same code path the CPU mining loop exercises.
            (void)HashEngine::hashHeader(local_header.data());
            ++nonce;
            ++total_hashes;
        }
    } else {
#ifndef DINERO_SOLO_HAS_GPU
        reportError("benchmark requested GPU backend " +
                    minerBackendToString(backend) +
                    " but binary was built without GPU support");
        return false;
#else
        // Reuse the same backend selection logic as start() but without
        // touching the live miner state. Build a temporary config and call
        // the same factory functions.
        MinerConfig saved = config_;
        config_.backend = backend;
        MinerBackend saved_active = active_backend_;
        if (!initializeGpuBackend()) {
            config_ = saved;
            active_backend_ = saved_active;
            return false;
        }
        if (!gpu_backend_) {
            reportError("benchmark could not initialize GPU backend " +
                        minerBackendToString(backend));
            config_ = saved;
            active_backend_ = saved_active;
            return false;
        }

        out.device_name = gpu_backend_->deviceName();
        out.backend_label = gpu_backend_->backendName();

        const uint32_t batch_size = config_.gpu_batch_size > 0
            ? config_.gpu_batch_size
            : (1u << 20);
        uint32_t nonce_start = 0;
        while (std::chrono::steady_clock::now() - t_start < duration) {
            auto outcome = gpu_backend_->dispatch(header.data(),
                                                  target.data(),
                                                  nonce_start,
                                                  batch_size);
            total_hashes += batch_size;
            nonce_start += batch_size;  // wraps cleanly; benchmark doesn't care
            // outcome.found_count is expected to be 0 with this target;
            // not worth reporting.
            (void)outcome;
        }

        gpu_backend_.reset();
        config_ = saved;
        active_backend_ = saved_active;
#endif
    }

    const auto t_end = std::chrono::steady_clock::now();
    out.total_hashes = total_hashes;
    out.duration_seconds =
        std::chrono::duration<double>(t_end - t_start).count();
    out.hashrate_mhs =
        out.duration_seconds > 0
            ? static_cast<double>(total_hashes) / out.duration_seconds / 1.0e6
            : 0.0;
    return true;
}

void SoloMiner::reportError(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error;
    }

    if (on_error_) {
        on_error_(error);
    }
}

} // namespace solo
} // namespace dinero
