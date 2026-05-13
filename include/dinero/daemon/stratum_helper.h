#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <queue>

/**
 * @brief Built-in Stratum v1 client for pool mining
 * 
 * Most mainnet users should use pool mining instead of solo mining.
 * This provides a professional Stratum implementation with:
 * - TLS support for secure connections
 * - Variable difficulty (vardiff) support
 * - Automatic reconnection with exponential backoff
 * - Share tracking (accepted/rejected/stale)
 * - Low-latency submission
 */
class StratumClient {
public:
    struct PoolConfig {
        std::string url;                // stratum+tcp://pool.example.com:4444
        std::string username;           // worker username
        std::string password;           // worker password (often "x")
        std::string payoutAddress;      // where pool should send rewards
        bool useTLS = false;           // use stratum+ssl://
        int reconnectDelay = 5;        // seconds between reconnect attempts
        int maxReconnectDelay = 300;   // max backoff delay
        bool vardiffEnabled = true;    // accept difficulty changes
    };

    struct StratumJob {
        std::string jobId;             // Job identifier from pool
        std::string prevHash;          // Previous block hash
        std::string coinb1;            // Coinbase part 1
        std::string coinb2;            // Coinbase part 2
        std::vector<std::string> merkles; // Merkle branches
        std::string version;           // Block version
        std::string nBits;             // Network difficulty bits
        std::string nTime;             // Network time
        bool cleanJobs = false;        // Abandon previous jobs
        double difficulty = 1.0;       // Current job difficulty
        std::chrono::steady_clock::time_point receivedAt;
    };

    struct ShareResult {
        bool accepted;                 // Share was accepted
        std::string error;             // Error message if rejected
        double difficulty;             // Share difficulty
        std::chrono::milliseconds latency; // Submission latency
        std::chrono::steady_clock::time_point submittedAt;
    };

    struct PoolStats {
        bool connected = false;        // Connected to pool
        std::string connectionStatus;  // "Connected", "Connecting", "Disconnected"
        uint64_t sharesAccepted = 0;   // Accepted shares count
        uint64_t sharesRejected = 0;   // Rejected shares count
        uint64_t sharesStale = 0;      // Stale shares count
        double currentDifficulty = 1.0; // Current pool difficulty
        std::chrono::milliseconds avgLatency{0}; // Average submission latency
        std::chrono::steady_clock::time_point lastShareTime;
        std::chrono::steady_clock::time_point connectedAt;
        int reconnectAttempts = 0;     // Current reconnection attempts
    };

    using JobCallback = std::function<void(const StratumJob&)>;
    using ShareCallback = std::function<void(const ShareResult&)>;
    using StatusCallback = std::function<void(const PoolStats&)>;

    explicit StratumClient(const PoolConfig& config);
    ~StratumClient();

    // Connection management
    bool Connect();
    void Disconnect();
    bool IsConnected() const { return m_connected.load(); }

    // Mining operations
    bool SubmitShare(const std::string& jobId, const std::string& extraNonce2, 
                     const std::string& nTime, const std::string& nonce);
    const StratumJob* GetCurrentJob() const;

    // Statistics
    const PoolStats& GetStats() const { return m_stats; }
    void ResetStats();

    // Callbacks
    void SetJobCallback(JobCallback callback) { m_jobCallback = callback; }
    void SetShareCallback(ShareCallback callback) { m_shareCallback = callback; }
    void SetStatusCallback(StatusCallback callback) { m_statusCallback = callback; }

    // Configuration
    void UpdateConfig(const PoolConfig& config);
    const PoolConfig& GetConfig() const { return m_config; }

private:
    // Network operations
    void NetworkWorker();
    bool EstablishConnection();
    void HandleDisconnection();
    
    // Protocol handling
    void SendSubscribe();
    void SendAuthorize();
    void SendSubmit(const std::string& jobId, const std::string& extraNonce2,
                    const std::string& nTime, const std::string& nonce);
    
    // Message processing
    void ProcessMessage(const std::string& message);
    void HandleMethod(const std::string& method, const nlohmann::json& params, 
                      const nlohmann::json& id);
    void HandleResult(const nlohmann::json& result, const nlohmann::json& id);
    void HandleError(const nlohmann::json& error, const nlohmann::json& id);

    // Specific method handlers
    void HandleNotify(const nlohmann::json& params);
    void HandleDifficultyChange(const nlohmann::json& params);
    void HandleSubscribeResult(const nlohmann::json& result);
    void HandleAuthorizeResult(const nlohmann::json& result);
    void HandleSubmitResult(const nlohmann::json& result, const nlohmann::json& id);

    // Reconnection logic
    void ScheduleReconnect();
    int CalculateBackoffDelay();

    // Statistics
    void UpdateLatency(std::chrono::milliseconds latency);
    void RecordShare(bool accepted, const std::string& error = "");

    PoolConfig m_config;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    
    // Network
    std::unique_ptr<class TcpSocket> m_socket;
    std::thread m_networkWorker;
    std::mutex m_sendMutex;
    
    // Current work
    std::unique_ptr<StratumJob> m_currentJob;
    std::mutex m_jobMutex;
    std::string m_extraNonce1;
    int m_extraNonce2Size = 4;
    
    // Statistics
    PoolStats m_stats;
    mutable std::mutex m_statsMutex;
    std::queue<std::chrono::milliseconds> m_latencyHistory;
    
    // Callbacks
    JobCallback m_jobCallback;
    ShareCallback m_shareCallback;
    StatusCallback m_statusCallback;
    
    // Reconnection
    std::atomic<int> m_reconnectDelay{0};
    std::chrono::steady_clock::time_point m_lastReconnectAttempt;
    
    static constexpr size_t MAX_LATENCY_SAMPLES = 100;
    static constexpr int BASE_RECONNECT_DELAY = 5;    // seconds
    static constexpr int MAX_RECONNECT_DELAY = 300;   // 5 minutes
};

/**
 * @brief Helper class for GUI integration
 * 
 * Provides a simplified interface for the GUI to manage pool mining
 */
class PoolMiningController {
public:
    enum class MiningMode {
        Solo,    // Mine directly to daemon (GBT)
        Pool     // Mine via Stratum pool
    };

    struct MiningSession {
        MiningMode mode;
        std::string target;            // Pool URL or "Solo"
        bool active = false;
        std::chrono::steady_clock::time_point startedAt;
        uint64_t hashesComputed = 0;
        double hashrateHps = 0.0;
    };

    bool StartPoolMining(const StratumClient::PoolConfig& config);
    bool StartSoloMining(const std::string& miningAddress);
    void StopMining();
    
    MiningMode GetCurrentMode() const { return m_currentMode; }
    const MiningSession& GetSession() const { return m_session; }
    
    // For GUI display
    std::string GetModeDescription() const;
    std::string GetTargetDescription() const;
    bool ShouldRecommendPool() const; // True for mainnet/testnet

private:
    MiningMode m_currentMode = MiningMode::Solo;
    MiningSession m_session;
    std::unique_ptr<StratumClient> m_stratumClient;
    std::unique_ptr<GBTWorkManager> m_gbtManager;
};
