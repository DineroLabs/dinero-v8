#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>

// Forward declarations
class GBTWorkManager;
struct WorkTemplate;
struct IEventSink;

/**
 * @brief Production-ready CPU mining engine
 * 
 * Features:
 * - CPU-friendly operation with duty-cycle throttling
 * - Real-time hashrate calculation and statistics
 * - Safety-first design with thermal/battery monitoring
 * - Professional work management with GBT integration
 * - Thread-safe operation with clean shutdown
 */
class MiningEngine {
public:
    struct MiningConfig {
        std::string miningAddress;      // Where to send block rewards
        unsigned numThreads = 1;        // Number of mining threads
        double throttle = 0.35;         // CPU duty cycle (0.15-0.90)
        bool lowPowerMode = true;       // Battery-aware throttling
        bool enableThermalProtection = true;  // Auto-pause on overheating
    };

    // Note: WorkTemplate is now defined in gbt_work_manager.h to avoid circular dependency

    struct MiningStats {
        std::atomic<bool> running{false};
        std::atomic<uint64_t> hashesComputed{0};
        std::atomic<uint64_t> blocksFound{0};
        std::atomic<double> hashrateHps{0.0};       // Current hashrate
        std::atomic<double> hashrateMA{0.0};        // 5-minute moving average
        std::chrono::steady_clock::time_point startTime;
        std::chrono::steady_clock::time_point lastBlockTime;
        std::chrono::steady_clock::time_point lastSubmitTime;
        std::atomic<bool> isPaused{false};
        std::string pauseReason;
    };

    using BlockFoundCallback = std::function<void(const std::string& blockHex, uint64_t templateId)>;
    using StatsUpdateCallback = std::function<void(const MiningStats&)>;

    explicit MiningEngine();
    ~MiningEngine();

    // Lifecycle management
    bool Start(const MiningConfig& config);
    void Stop();
    bool IsRunning() const { return m_stats.running.load(); }
    
    // Runtime control
    void Pause(const std::string& reason = "user_paused");
    void Resume();
    bool IsPaused() const { return m_stats.isPaused.load(); }
    
    // Configuration updates (can be changed while running)
    void UpdateThrottle(double throttle);
    void UpdateThreadCount(unsigned threads);
    
    // Work management
    bool UpdateWorkTemplate(const WorkTemplate& template_);
    void MarkTemplateStale(uint64_t templateId);
    void SetWorkManager(std::shared_ptr<GBTWorkManager> workManager) { m_workManager = workManager; }
    void refreshJob(std::string_view reason = "manual");
    
    // Statistics
    const MiningStats& GetStats() const { return m_stats; }
    const MiningConfig& GetConfig() const { return m_config; }
    double GetCurrentHashrate() const { return m_stats.hashrateHps.load(); }
    uint64_t GetHashesComputed() const { return m_stats.hashesComputed.load(); }
    
    // Callbacks
    void SetBlockFoundCallback(BlockFoundCallback callback) { m_blockFoundCallback = callback; }
    void SetStatsUpdateCallback(StatsUpdateCallback callback) { m_statsUpdateCallback = callback; }
    
                // Event publishing
                void SetEventSink(std::shared_ptr<IEventSink> sink) { m_eventSink = sink; }
                
                // Metrics integration
                void enableMetrics(bool enable) { m_metricsEnabled = enable; }
                
                // Public methods for MiningSupervisor
                std::string buildBlockHeader(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce) {
                    return BuildBlockHeader(work, nonce, extraNonce);
                }
                
                bool checkProofOfWork(const std::string& blockHeader, uint32_t bits) {
                    return CheckProofOfWork(blockHeader, bits);
                }

private:
    // Mining thread management
    void MiningWorker(unsigned threadId);
    void StatsWorker();
    void SafetyMonitorWorker();
    void SamplerWorker();
    
    // Work processing
    bool ProcessWork(unsigned threadId, const WorkTemplate& work, uint32_t extraNonce);
    std::string BuildBlockHeader(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce);
    std::string BuildCompleteBlock(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce);
    std::string BuildCoinbaseTransaction(const WorkTemplate& work);
    bool CheckProofOfWork(const std::string& blockHeader, uint32_t bits);
    
    // Hashrate calculation
    void UpdateHashrateStats();
    void RecordHashrate(double hashrate);
    
    // Safety monitoring
    void CheckSafetyConditions();
    bool ShouldPauseForSafety();
    
    // Duty cycle implementation
    void DutyCycleSleep(double throttle);
    std::chrono::milliseconds CalculateWorkTime(double throttle);
    std::chrono::milliseconds CalculateRestTime(double throttle);

    // Configuration
    MiningConfig m_config;
    std::atomic<double> m_currentThrottle{0.35};
    std::atomic<unsigned> m_currentThreads{1};
    
    // Work state
    std::shared_ptr<WorkTemplate> m_currentWork;
    std::mutex m_workMutex;
    std::condition_variable m_workCondition;
    std::atomic<uint64_t> m_nextExtraNonce{1};
    std::shared_ptr<GBTWorkManager> m_workManager;
    
    // Thread management
    std::vector<std::thread> m_miningThreads;
    std::thread m_statsThread;
    std::thread m_safetyThread;
    std::thread m_samplerThread;
    std::atomic<bool> m_shouldStop{false};
    
    // Statistics
    mutable MiningStats m_stats;
    std::mutex m_statsMutex;
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> m_hashrateHistory;
    
    // Callbacks
    BlockFoundCallback m_blockFoundCallback;
    StatsUpdateCallback m_statsUpdateCallback;
    std::shared_ptr<IEventSink> m_eventSink;
    
    // Metrics integration
    std::atomic<bool> m_metricsEnabled{false};
    std::chrono::steady_clock::time_point m_metricsStartTime;
    
    // Constants
    static constexpr std::chrono::milliseconds DUTY_CYCLE_WINDOW{200}; // 200ms work/rest cycle
    static constexpr std::chrono::seconds STATS_UPDATE_INTERVAL{2};    // Update stats every 2 seconds
    static constexpr std::chrono::seconds SAFETY_CHECK_INTERVAL{5};    // Check safety every 5 seconds
    static constexpr size_t MAX_HASHRATE_SAMPLES = 150;                // 5 minutes at 2-second intervals
};

/**
 * @brief SHA-256 mining utilities
 */
class MiningUtils {
public:
    // Hash computation
    static std::string DoubleSHA256(const std::string& data);
    static std::string SHA256(const std::string& data);
    
    // Difficulty utilities
    static uint32_t DifficultyToTarget(double difficulty);
    static double TargetToDifficulty(uint32_t bits);
    static bool CheckTarget(const std::string& hash, uint32_t bits);
    
    // Block header utilities
    static std::string SerializeBlockHeader(
        uint32_t version, const std::string& prevHash, const std::string& merkleRoot,
        uint32_t timestamp, uint32_t bits, uint32_t nonce
    );
    
    // Endian conversion
    static uint32_t SwapEndian32(uint32_t value);
    static std::string BytesToHex(const uint8_t* data, size_t length);
    static std::vector<uint8_t> HexToBytes(const std::string& hex);
};
