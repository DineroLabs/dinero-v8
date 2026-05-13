#include "daemon/gbt_work_manager.h"  // Must be first to get WorkTemplate definition
#include "daemon/mining_engine.h"
#include "primitives/amount.h"
#include "daemon/mining_safety_gates.h"
#include "mining/block_assembler.h"  // BlockAssembler::CreateJob()
#include "common/sha256d.h"
#include "common/logger.h"
#include "events/event_sink.h"
#include "metrics/metrics_registry.h"
#include "consensus/subsidy.h"  // Phase M.5.3: Consensus subsidy query
#include <algorithm>
#include <random>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <iostream>

// ============================================================================
// Canonical Serialization Helpers (Deterministic, Endian-Explicit)
// ============================================================================
namespace {

// Write uint32_t in little-endian format (explicit byte-by-byte)
inline void WriteLE32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

// Write uint64_t in little-endian format (explicit byte-by-byte)
inline void WriteLE64(std::vector<uint8_t>& data, uint64_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 32) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 40) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 48) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 56) & 0xff));
}

} // namespace

// ============================================================================
// MiningEngine Implementation
// ============================================================================

MiningEngine::MiningEngine() {
    m_stats.startTime = std::chrono::steady_clock::now();
    
    // Phase 1: Generate unique miner ID for metrics labels
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    miner_id_ = "miner_" + std::to_string(timestamp);
}

MiningEngine::~MiningEngine() {
    Stop();
}

bool MiningEngine::Start(const MiningConfig& config) {
    if (m_stats.running.load()) {
        return false; // Already running
    }
    
    m_config = config;
    m_currentThrottle = config.throttle;
    m_currentThreads = config.numThreads;
    m_shouldStop = false;
    
    // Initialize stats
    m_stats.running = true;
    m_stats.isPaused = false;
    m_stats.hashesComputed = 0;
    m_stats.blocksFound = 0;
    m_stats.hashrateHps = 0.0;
    m_stats.hashrateMA = 0.0;
    m_stats.startTime = std::chrono::steady_clock::now();
    m_stats.pauseReason.clear();
    
    // Initialize metrics
    m_metricsStartTime = std::chrono::steady_clock::now();
    if (m_metricsEnabled.load()) {
        // Phase 1: Use labels for per-miner metrics
        dinero::metrics::LabelMap labels = {{"miner_id", miner_id_}};
        dinero::metrics::MetricsRegistry::SetMiningThreads(config.numThreads, labels);
        dinero::metrics::MetricsRegistry::SetMiningUptime(0.0, labels);
    }
    
    // Start worker threads
    m_miningThreads.reserve(config.numThreads);
    for (unsigned i = 0; i < config.numThreads; ++i) {
        m_miningThreads.emplace_back(&MiningEngine::MiningWorker, this, i);
    }
    
    // Start support threads
    m_statsThread = std::thread(&MiningEngine::StatsWorker, this);
    m_safetyThread = std::thread(&MiningEngine::SafetyMonitorWorker, this);
    m_samplerThread = std::thread(&MiningEngine::SamplerWorker, this);
    
    // Broadcast mining started event
    if (m_eventSink) {
        nlohmann::json event;
        event["threads"] = static_cast<int>(config.numThreads);
        event["address"] = config.miningAddress;
        event["throttle"] = config.throttle;
        m_eventSink->publish("mining.started", event);
    }
    
    // Force create a job immediately on startup
    refreshJob("startup");
    
    return true;
}

void MiningEngine::Stop() {
    if (!m_stats.running.load()) {
        return; // Not running
    }
    
    // Signal all threads to stop
    m_shouldStop = true;
    m_stats.running = false;
    
    // Broadcast mining stopped event
    if (m_eventSink) {
        nlohmann::json event;
        event["reason"] = "user_stop";
        m_eventSink->publish("mining.stopped", event);
    }
    
    // Wake up any waiting threads
    m_workCondition.notify_all();
    
    // Join all mining threads
    for (auto& thread : m_miningThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    m_miningThreads.clear();
    
    // Join support threads
    if (m_statsThread.joinable()) {
        m_statsThread.join();
    }
    if (m_safetyThread.joinable()) {
        m_safetyThread.join();
    }
    if (m_samplerThread.joinable()) {
        m_samplerThread.join();
    }
    
    // Clear work state
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        m_currentWork.reset();
    }
}

void MiningEngine::Pause(const std::string& reason) {
    m_stats.isPaused = true;
    m_stats.pauseReason = reason;
}

void MiningEngine::Resume() {
    m_stats.isPaused = false;
    m_stats.pauseReason.clear();
    m_workCondition.notify_all();
}

void MiningEngine::UpdateThrottle(double throttle) {
    m_currentThrottle = std::clamp(throttle, 0.15, 0.90);
}

void MiningEngine::UpdateThreadCount(unsigned threads) {
    // Dynamic thread count adjustment would be implemented here
    // For now, store the new value - full implementation requires thread management
    m_currentThreads = std::max(1u, threads);
}

bool MiningEngine::UpdateWorkTemplate(const WorkTemplate& template_) {
    dinero::g_logger.info("🔧 UpdateWorkTemplate called with height: " + std::to_string(template_.height));
    
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        
        // Mark old template as stale
        if (m_currentWork) {
            m_currentWork->stale = true;
        }
        
        // Set new work (create new instance to avoid atomic copy issues)
        m_currentWork = std::make_shared<WorkTemplate>();
        m_currentWork->blockHash = template_.blockHash;
        m_currentWork->merkleRoot = template_.merkle_root;
        m_currentWork->version = template_.version;
        m_currentWork->bits = template_.bits;
        m_currentWork->timestamp = template_.timestamp;
        m_currentWork->height = template_.height;
        m_currentWork->coinbaseValue = template_.coinbaseValue;
        m_currentWork->transactions = template_.transactions;
        m_currentWork->templateId = template_.templateId;
        m_currentWork->createdAt = template_.createdAt;
        m_currentWork->stale = false;
        
        dinero::g_logger.info("🔧 Work template updated successfully, notifying workers");
    }
    
    // Update metrics for new job
    if (m_metricsEnabled.load()) {
        // Phase 1: Use labels for per-miner metrics
        dinero::metrics::LabelMap labels = {{"miner_id", miner_id_}};
        dinero::metrics::MetricsRegistry::SetMiningJobHeight(template_.height, labels);
        dinero::metrics::MetricsRegistry::SetMiningCurrentBits(template_.bits, labels);
    }
    
    // Wake up all mining threads
    m_workCondition.notify_all();
    return true;
}

void MiningEngine::MarkTemplateStale(uint64_t templateId) {
    std::lock_guard<std::mutex> lock(m_workMutex);
    if (m_currentWork && m_currentWork->templateId == templateId) {
        m_currentWork->stale = true;
    }
}

void MiningEngine::refreshJob(std::string_view reason) {
    dinero::g_logger.info("Refreshing mining job (reason: " + std::string(reason) + ")");

    if (!m_blockAssembler) {
        dinero::g_logger.error("No BlockAssembler — cannot create block template");
        m_stats.pauseReason = "no-block-assembler";
        return;
    }

    auto mining_job = m_blockAssembler->CreateJob();
    if (!mining_job) {
        dinero::g_logger.error("BlockAssembler::CreateJob() failed");
        m_stats.pauseReason = "template-creation-failed";
        return;
    }

    // Convert MiningJob → WorkTemplate
    auto workTemplate = std::make_shared<WorkTemplate>();
    workTemplate->header = mining_job->header;
    workTemplate->height = mining_job->height;
    auto coinbase_value = dinero::CheckedAddUna(mining_job->block_reward, mining_job->total_fees);
    if (!coinbase_value) {
        dinero::g_logger.error("Mining job produced invalid coinbase value");
        m_stats.pauseReason = "invalid-coinbase-value";
        return;
    }
    workTemplate->coinbaseValue = std::to_string(*coinbase_value);
    workTemplate->transactions.reserve(mining_job->transactions.size());
    for (const auto& tx : mining_job->transactions) {
        workTemplate->transactions.push_back(tx.GetTxid().AsUint256().GetHex());
    }
    workTemplate->templateId = ++m_nextExtraNonce;
    workTemplate->createdAt = std::chrono::steady_clock::now();
    workTemplate->stale.store(false);

    if (UpdateWorkTemplate(*workTemplate)) {
        dinero::g_logger.info("Mining job refreshed: height=" + std::to_string(mining_job->height)
                             + " txs=" + std::to_string(mining_job->transactions.size())
                             + " fees=" + std::to_string(mining_job->total_fees));
        m_stats.pauseReason.clear();

        if (m_eventSink) {
            nlohmann::json event;
            event["id"] = workTemplate->templateId;
            event["height"] = mining_job->height;
            event["bits"] = mining_job->target_bits;
            event["reward"] = workTemplate->coinbaseValue;
            event["txs"] = mining_job->transactions.size();
            event["fees"] = mining_job->total_fees;
            event["reason"] = std::string(reason);
            m_eventSink->publish("mining.job", event);
        }
    } else {
        dinero::g_logger.error("Failed to update work template");
        m_stats.pauseReason = "template-update-failed";
    }
}

void MiningEngine::MiningWorker(unsigned threadId) {
    dinero::g_logger.info("🔧 Mining worker " + std::to_string(threadId) + " started");
    
    std::random_device rd;
    std::mt19937 gen(rd() ^ threadId);
    std::uniform_int_distribution<uint32_t> nonceDist;
    
    while (!m_shouldStop.load()) {
        // Check if paused
        if (m_stats.isPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Get current work
        std::shared_ptr<WorkTemplate> work;
        {
            std::unique_lock<std::mutex> lock(m_workMutex);
            
            // If no work available and we have a work manager, try to get work
            if (!m_currentWork && m_workManager) {
                dinero::g_logger.info("🔧 Worker " + std::to_string(threadId) + " trying to get work from work manager");
                m_currentWork = m_workManager->GetCurrentWork();
                if (m_currentWork) {
                    dinero::g_logger.info("🔧 Worker " + std::to_string(threadId) + " got work from work manager");
                } else {
                    dinero::g_logger.warning("🔧 Worker " + std::to_string(threadId) + " failed to get work from work manager");
                }
            }
            
            dinero::g_logger.info("🔧 Worker " + std::to_string(threadId) + " waiting for work (current work: " + 
                                (m_currentWork ? "available" : "none") + ")");
            
            m_workCondition.wait(lock, [this] {
                return m_shouldStop.load() || m_currentWork != nullptr;
            });
            
            if (m_shouldStop.load()) {
                break;
            }
            
            work = m_currentWork;
        }
        
        if (!work || work->stale.load()) {
            dinero::g_logger.info("🔧 Worker " + std::to_string(threadId) + " no work available, sleeping");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        dinero::g_logger.info("🔧 Worker " + std::to_string(threadId) + " starting to mine batch");
        
        // Get unique extranonce for this thread
        uint32_t extraNonce = m_nextExtraNonce.fetch_add(1);
        
        // Mine for a batch of nonces
        const uint32_t batchSize = 1000;
        uint32_t startNonce = nonceDist(gen);
        
        auto workStart = std::chrono::steady_clock::now();
        
        for (uint32_t i = 0; i < batchSize && !work->stale.load() && !m_shouldStop.load() && !m_stats.isPaused.load(); ++i) {
            uint32_t nonce = startNonce + i;
            
            // Process this work unit
            if (ProcessWork(threadId, *work, nonce)) {
                // Found a block!
                m_stats.blocksFound.fetch_add(1);
                m_stats.lastBlockTime = std::chrono::steady_clock::now();
                
                // Update metrics
                if (m_metricsEnabled.load()) {
                    // Phase 1: Use labels for per-miner metrics
                    dinero::metrics::LabelMap labels = {{"miner_id", miner_id_}};
                    dinero::metrics::MetricsRegistry::IncrementMiningBlocksFound(labels);
                }
                
                // Publish block found event (with deduplication)
                if (m_eventSink) {
                    // Build the complete block to get the actual hash
                    std::string blockHex = BuildCompleteBlock(*work, nonce, extraNonce);

                    nlohmann::json event;
                    event["hash"] = blockHex; // Use actual block hash for deduplication
                    event["height"] = work->height;
                    event["bits"] = work->bits;
                    event["reward"] = 100.0; // Simplified reward
                    event["ts"] = std::time(nullptr); // Add timestamp for ordering

                    // Use proper deduplication - only publish if this is a unique block
                    static std::unordered_map<std::string, uint64_t> published_blocks;
                    static std::mutex block_mutex;

                    std::lock_guard<std::mutex> lock(block_mutex);
                    auto it = published_blocks.find(blockHex);
                    if (it == published_blocks.end() || it->second < event["ts"]) {
                        published_blocks[blockHex] = event["ts"];
                        m_eventSink->publish("mining.block_found", event);
                    }
                }
                
                if (m_blockFoundCallback) {
                    std::string blockHex = BuildCompleteBlock(*work, nonce, extraNonce);
                    m_blockFoundCallback(blockHex, work->templateId);
                }
                
                // Also submit through work manager if available
                if (m_workManager) {
                    std::string blockHex = BuildCompleteBlock(*work, nonce, extraNonce);
                    m_workManager->SubmitBlock(blockHex);
                }
            }
            
            m_stats.hashesComputed.fetch_add(1);
        }
        
        auto workEnd = std::chrono::steady_clock::now();
        
        // Implement duty cycle throttling
        auto workDuration = std::chrono::duration_cast<std::chrono::milliseconds>(workEnd - workStart);
        DutyCycleSleep(m_currentThrottle.load());
        
        // Update hashrate for this thread
        double workSeconds = workDuration.count() / 1000.0;
        if (workSeconds > 0) {
            double threadHashrate = batchSize / workSeconds;
            // Thread-local hashrate will be aggregated by stats worker
        }
    }
}

void MiningEngine::StatsWorker() {
    while (!m_shouldStop.load()) {
        std::this_thread::sleep_for(STATS_UPDATE_INTERVAL);
        
        if (!m_stats.running.load()) {
            continue;
        }
        
        UpdateHashrateStats();
        
        // Update metrics (rate-limited to ~4Hz)
        if (m_metricsEnabled.load()) {
            // Phase 1: Use labels for per-miner metrics
            dinero::metrics::LabelMap labels = {{"miner_id", miner_id_}};
            dinero::metrics::MetricsRegistry::SetMiningHashrate(m_stats.hashrateHps.load(), labels);
            dinero::metrics::MetricsRegistry::SetMiningThreads(static_cast<int>(m_currentThreads.load()), labels);
            
            // Update uptime
            auto uptime = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_metricsStartTime).count();
            dinero::metrics::MetricsRegistry::SetMiningUptime(uptime, labels);
        }
        
        if (m_statsUpdateCallback) {
            m_statsUpdateCallback(m_stats);
        }
        
        // Publish hashrate update event (rate-limited)
        if (m_eventSink) {
            nlohmann::json event;
            event["hps"] = m_stats.hashrateHps.load();
            event["hps_ma"] = m_stats.hashrateMA.load();
            event["threads"] = static_cast<int>(m_currentThreads.load());
            event["blocks_found"] = static_cast<int64_t>(m_stats.blocksFound.load());
            event["total_hashes"] = static_cast<int64_t>(m_stats.hashesComputed.load());
            m_eventSink->publish("mining.hashrate", event);
        }
    }
}

void MiningEngine::SafetyMonitorWorker() {
    while (!m_shouldStop.load()) {
        std::this_thread::sleep_for(SAFETY_CHECK_INTERVAL);
        
        if (!m_stats.running.load()) {
            continue;
        }
        
        CheckSafetyConditions();
    }
}

void MiningEngine::SamplerWorker() {
    dinero::g_logger.info("📊 Starting mining sampler thread");
    
    while (!m_shouldStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        if (!m_stats.running.load()) {
            continue;
        }
        
        // Calculate current hashrate
        auto currentTime = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - m_stats.startTime).count();
        
        double hashrate = 0.0;
        if (uptime > 0) {
            hashrate = static_cast<double>(m_stats.hashesComputed.load()) / uptime;
        }
        
        // Update metrics
        if (m_metricsEnabled.load()) {
            // Phase 1: Use labels for per-miner metrics
            dinero::metrics::LabelMap labels = {{"miner_id", miner_id_}};
            dinero::metrics::MetricsRegistry::SetMiningHashrate(hashrate, labels);
            dinero::metrics::MetricsRegistry::SetMiningUptime(static_cast<double>(uptime), labels);
        }
        
        // Update internal stats
        m_stats.hashrateHps = hashrate;
        
        // Broadcast hashrate event
        if (m_eventSink) {
            nlohmann::json event;
            event["hps"] = hashrate;
            event["uptime"] = uptime;
            event["total_hashes"] = m_stats.hashesComputed.load();
            event["threads"] = m_currentThreads.load();
            m_eventSink->publish("mining.hashrate", event);
        }
        
        // Log periodic status (every 10 seconds)
        static int logCounter = 0;
        if (++logCounter >= 20) { // 20 * 500ms = 10 seconds
            dinero::g_logger.info("📊 Mining Status: " + std::to_string(hashrate) + " H/s, " + 
                                std::to_string(m_stats.hashesComputed.load()) + " total hashes");
            logCounter = 0;
        }
    }
    
    dinero::g_logger.info("📊 Mining sampler thread stopped");
}

bool MiningEngine::ProcessWork(unsigned threadId, const WorkTemplate& work, uint32_t nonce) {
    // Build block header with this nonce
    std::string blockHeader = BuildBlockHeader(work, nonce, threadId);

    // Check if this meets the target
    return CheckProofOfWork(blockHeader, work.header.difficulty);
}

std::string MiningEngine::BuildBlockHeader(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce) {
    // ═══════════════════════════════════════════════════════════════════
    // Phase 3: ARCHITECTURAL RULE ENFORCEMENT
    // ═══════════════════════════════════════════════════════════════════
    // Only BlockHeader::SerializeForHash() produces hashing bytes.
    // This function does NOT build headers - it modifies nonce and serializes.
    // ═══════════════════════════════════════════════════════════════════

    // ✅ Copy header from template (already complete and valid)
    BlockHeader header = work.header;

    // ✅ Modify only the nonce (miner's job)
    header.nonce = nonce;

    // ✅ Use canonical serialization (returns std::array<uint8_t, 128>)
    auto header_bytes = header.SerializeForHash();

    // 🧪 TEMPORARY PHASE 3 ASSERTION (keep until genesis finalized)
    assert(header_bytes.size() == 128 && "FATAL: Header must be exactly 128 bytes");
    assert(header.IsReservedValid() && "FATAL: reserved[12] must be all zeros");

    // Convert to string for return
    return std::string(reinterpret_cast<const char*>(header_bytes.data()), header_bytes.size());
}

std::string MiningEngine::BuildCompleteBlock(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce) {
    // Build complete block: header + transaction count + transactions

    // CANONICAL: Use explicit byte-by-byte serialization (not ostringstream)
    std::vector<uint8_t> block;
    block.reserve(250);  // Approximate: 128 (header v1) + ~120 (coinbase tx)

    // 1. Block header (128 bytes - BlockHeader v1)
    std::string blockHeader = BuildBlockHeader(work, nonce, extraNonce);
    block.insert(block.end(), blockHeader.begin(), blockHeader.end());

    // 2. Transaction count (1 byte VarInt - we only have coinbase)
    block.push_back(0x01);

    // 3. Coinbase transaction
    std::string coinbaseTx = BuildCoinbaseTransaction(work);
    block.insert(block.end(), coinbaseTx.begin(), coinbaseTx.end());

    // Convert to hex string
    return MiningUtils::BytesToHex(block.data(), block.size());
}

std::string MiningEngine::BuildCoinbaseTransaction(const WorkTemplate& work) {
    // CANONICAL: Use explicit byte-by-byte serialization (not ostringstream)
    std::vector<uint8_t> coinbase;
    coinbase.reserve(120);  // Approximate coinbase transaction size

    // Version (4 bytes, little-endian)
    WriteLE32(coinbase, 1);

    // Input count (1 byte)
    coinbase.push_back(0x01);

    // Input: Previous output hash (32 bytes of zeros for coinbase)
    for (int i = 0; i < 32; i++) {
        coinbase.push_back(0x00);
    }

    // Input: Previous output index (4 bytes, 0xffffffff for coinbase)
    WriteLE32(coinbase, 0xffffffff);

    // Input: Script length (8 bytes: height + "DIN\x01")
    coinbase.push_back(8);

    // Input: Coinbase script (height in little-endian + extra data)
    WriteLE32(coinbase, static_cast<uint32_t>(work.height));

    // Extra nonce space
    coinbase.push_back('D');
    coinbase.push_back('N');
    coinbase.push_back('R');
    coinbase.push_back(0x01);

    // Input: Sequence (4 bytes, 0xffffffff)
    WriteLE32(coinbase, 0xffffffff);

    // Output count (1 byte)
    coinbase.push_back(0x01);

    // Output: Value (8 bytes, little-endian)
    WriteLE64(coinbase, 100000000);  // 1 DIN in una (100M)

    // Output: Script length (22 bytes for P2WPKH)
    coinbase.push_back(22);

    // Output: Script (P2WPKH: OP_0 + 20-byte pubkey hash)
    coinbase.push_back(0x00);  // OP_0
    coinbase.push_back(0x14);  // Push 20 bytes

    // Mining address pubkey hash (simplified for regtest)
    for (int i = 0; i < 20; i++) {
        coinbase.push_back(0x01);
    }

    // Lock time (4 bytes)
    WriteLE32(coinbase, 0);

    return std::string(reinterpret_cast<const char*>(coinbase.data()), coinbase.size());
}

bool MiningEngine::CheckProofOfWork(const std::string& blockHeader, uint32_t bits) {
    // Double SHA-256 the block header
    std::string hash = MiningUtils::DoubleSHA256(blockHeader);
    
    // Check if hash meets the target
    return MiningUtils::CheckTarget(hash, bits);
}

void MiningEngine::UpdateHashrateStats() {
    static auto lastUpdate = std::chrono::steady_clock::now();
    static uint64_t lastHashCount = 0;
    
    auto now = std::chrono::steady_clock::now();
    uint64_t currentHashes = m_stats.hashesComputed.load();
    
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
    if (timeDiff.count() > 0) {
        uint64_t hashDiff = currentHashes - lastHashCount;
        double hashrate = (hashDiff * 1000.0) / timeDiff.count(); // Hashes per second
        
        m_stats.hashrateHps = hashrate;
        RecordHashrate(hashrate);
        
        // Calculate 5-minute moving average
        double totalHashrate = 0.0;
        size_t sampleCount = 0;
        
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            for (const auto& sample : m_hashrateHistory) {
                totalHashrate += sample.second;
                sampleCount++;
            }
        }
        
        if (sampleCount > 0) {
            m_stats.hashrateMA = totalHashrate / sampleCount;
        }
    }
    
    lastUpdate = now;
    lastHashCount = currentHashes;
}

void MiningEngine::RecordHashrate(double hashrate) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    
    auto now = std::chrono::steady_clock::now();
    m_hashrateHistory.push_back({now, hashrate});
    
    // Remove samples older than 5 minutes
    auto cutoff = now - std::chrono::minutes(5);
    while (!m_hashrateHistory.empty() && m_hashrateHistory.front().first < cutoff) {
        m_hashrateHistory.pop_front();
    }
    
    // Limit to maximum samples
    while (m_hashrateHistory.size() > MAX_HASHRATE_SAMPLES) {
        m_hashrateHistory.pop_front();
    }
}

void MiningEngine::CheckSafetyConditions() {
    if (ShouldPauseForSafety()) {
        std::string reason = MiningSafetyGates::GetPauseReason();
        if (!reason.empty() && !m_stats.isPaused.load()) {
            Pause(reason);
        }
    } else if (m_stats.isPaused.load() && m_stats.pauseReason != "user_paused") {
        // Auto-resume if safety conditions are now OK
        Resume();
    }
}

bool MiningEngine::ShouldPauseForSafety() {
    return MiningSafetyGates::ShouldPauseMining();
}

void MiningEngine::DutyCycleSleep(double throttle) {
    if (throttle >= 0.99) {
        return; // No throttling needed
    }
    
    auto workTime = CalculateWorkTime(throttle);
    auto restTime = CalculateRestTime(throttle);
    
    if (restTime.count() > 0) {
        std::this_thread::sleep_for(restTime);
    }
}

std::chrono::milliseconds MiningEngine::CalculateWorkTime(double throttle) {
    return std::chrono::milliseconds(static_cast<int>(DUTY_CYCLE_WINDOW.count() * throttle));
}

std::chrono::milliseconds MiningEngine::CalculateRestTime(double throttle) {
    return std::chrono::milliseconds(static_cast<int>(DUTY_CYCLE_WINDOW.count() * (1.0 - throttle)));
}

// MiningUtils implementation
std::string MiningUtils::DoubleSHA256(const std::string& data) {
    return Dinero::Common::double_sha256(data);
}

std::string MiningUtils::SHA256(const std::string& data) {
    Dinero::Common::sha256 hasher;
    hasher.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    auto hashBytes = hasher.finalize();
    return BytesToHex(hashBytes.data(), hashBytes.size());
}

bool MiningUtils::CheckTarget(const std::string& hash, uint32_t bits) {
    // Convert compact bits to target
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00ffffff;
    
    // Target = mantissa * 256^(exponent - 3)
    // For simplicity, do a string comparison (Bitcoin-style)
    
    // Convert hash to big-endian for comparison
    auto hashBytes = HexToBytes(hash);
    std::reverse(hashBytes.begin(), hashBytes.end()); // Convert to big-endian
    
    // Build target bytes
    std::vector<uint8_t> target(32, 0);
    if (exponent <= 32 && exponent >= 3) {
        int targetIndex = 32 - exponent;
        if (targetIndex >= 0 && targetIndex <= 29) {
            target[targetIndex] = (mantissa >> 16) & 0xff;
            target[targetIndex + 1] = (mantissa >> 8) & 0xff;
            target[targetIndex + 2] = mantissa & 0xff;
        }
    }
    
    // Compare hash <= target (big-endian comparison)
    for (size_t i = 0; i < 32; i++) {
        if (hashBytes[i] < target[i]) {
            return true; // hash < target, valid PoW
        } else if (hashBytes[i] > target[i]) {
            return false; // hash > target, invalid PoW
        }
        // If equal, continue to next byte
    }
    
    return true; // hash == target, valid PoW
}

uint32_t MiningUtils::SwapEndian32(uint32_t value) {
    return ((value >> 24) & 0x000000ff) |
           ((value >> 8)  & 0x0000ff00) |
           ((value << 8)  & 0x00ff0000) |
           ((value << 24) & 0xff000000);
}

std::string MiningUtils::BytesToHex(const uint8_t* data, size_t length) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < length; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::vector<uint8_t> MiningUtils::HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        bytes.push_back(byte);
    }
    
    return bytes;
}
