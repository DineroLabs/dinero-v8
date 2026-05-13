#pragma once

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "primitives/block.h"  // Phase 3: BlockHeader v1 (128 bytes)

// Forward declarations
class MiningEngine;
struct DaemonContext;

namespace dinero {
namespace consensus {
class BlockValidator;  // For Utreexo root computation
}
}

// ═══════════════════════════════════════════════════════════════════════════════
// WorkTemplate structure (Phase 3: Wraps BlockHeader)
// ═══════════════════════════════════════════════════════════════════════════════
// ARCHITECTURAL RULE: Only BlockHeader::SerializeForHash() produces hashing bytes.
// WorkTemplate wraps a complete BlockHeader + metadata for miners.
// ═══════════════════════════════════════════════════════════════════════════════
struct WorkTemplate {
    // ✅ AUTHORITATIVE: All header fields come from here
    BlockHeader header;             // Complete 128-byte BlockHeader v1 (Phase 3)

    // Metadata (NOT part of header hash):
    int64_t height;                 // Block height (not in header)
    std::string coinbaseValue;      // Coinbase reward + fees (not in header)
    std::vector<std::string> transactions; // Transaction hashes (not in header)
    uint64_t templateId;            // Unique template identifier
    std::chrono::steady_clock::time_point createdAt;
    std::atomic<bool> stale{false}; // Template is stale
};

/**
 * @brief GetBlockTemplate work manager for solo mining
 * 
 * Features:
 * - Real blockchain integration with database queries
 * - Automatic work refresh on tip changes
 * - Longpoll support for immediate updates
 * - Coinbase transaction construction
 * - Transaction selection and fee optimization
 * - Stale work detection and cleanup
 */
class GBTWorkManager {
public:
    struct GBTConfig {
        std::string miningAddress;      // Where to send block rewards
        uint32_t maxBlockSize = 4000000; // Max block size in bytes
        uint32_t maxTransactions = 2000; // Max transactions per block
        bool includeFees = true;         // Include transaction fees
        std::chrono::seconds refreshInterval{30}; // Work refresh interval
    };

    struct BlockCandidate {
        // ✅ AUTHORITATIVE: All header fields come from here
        BlockHeader header;             // Complete 128-byte BlockHeader v1 (Phase 3)

        // Metadata (NOT part of header hash):
        int64_t height;                 // Block height (not in header)
        uint64_t coinbaseValue;         // Base reward + fees (not in header)
        std::vector<std::string> transactions; // Selected transaction hashes (not in header)
        std::vector<std::string> transactionData; // Full transaction data (not in header)
        size_t totalSize;               // Total block size (not in header)
        uint64_t totalFees;             // Total transaction fees (not in header)
        uint64_t templateId;            // Unique template ID (not in header)
    };

    using WorkUpdateCallback = std::function<void(const WorkTemplate&)>;
    using BlockSubmitCallback = std::function<bool(const std::string& blockHex)>;

    explicit GBTWorkManager(DaemonContext* ctx = nullptr);
    ~GBTWorkManager();

    // Context injection (Week 3: remove global dependency)
    void SetContext(DaemonContext* ctx) { m_context = ctx; }

    /**
     * @brief Set BlockValidator for Utreexo root computation (single source of truth)
     *
     * Enables GBTWorkManager to call the same ComputeUtreexoRootPure function
     * used by validation, ensuring mining and validation compute identical roots.
     *
     * @param validator BlockValidator instance (non-owning pointer)
     */
    void SetBlockValidator(dinero::consensus::BlockValidator* validator) { m_blockValidator = validator; }

    // Lifecycle management
    bool Start(const GBTConfig& config);
    void Stop();
    bool IsRunning() const { return m_isRunning.load(); }

    // Work generation
    std::shared_ptr<WorkTemplate> GetCurrentWork();
    bool RefreshWork();
    void InvalidateCurrentWork();

    // Blockchain integration
    bool SubmitBlock(const std::string& blockHex);
    int64_t GetCurrentHeight();
    std::string GetBestBlockHash();
    uint32_t GetNetworkDifficulty();

    // Configuration
    void UpdateConfig(const GBTConfig& config);
    const GBTConfig& GetConfig() const { return m_config; }

    // Callbacks
    void SetWorkUpdateCallback(WorkUpdateCallback callback) { m_workUpdateCallback = callback; }
    void SetBlockSubmitCallback(BlockSubmitCallback callback) { m_blockSubmitCallback = callback; }

    // Statistics
    struct GBTStats {
        std::atomic<uint64_t> templatesGenerated{0};
        std::atomic<uint64_t> blocksSubmitted{0};
        std::atomic<uint64_t> blocksAccepted{0};
        std::atomic<uint64_t> staleWorkCount{0};
        std::chrono::steady_clock::time_point lastRefresh;
        std::chrono::steady_clock::time_point lastSubmission;
    };
    const GBTStats& GetStats() const { return m_stats; }

private:
    // Work management threads
    void WorkRefreshLoop();
    void TipMonitorLoop();

    // Block template construction
    BlockCandidate BuildBlockCandidate();
    std::vector<std::string> SelectTransactions(uint64_t& totalFees, size_t& totalSize);
    std::string BuildCoinbaseTransaction(int64_t height, uint64_t reward, const std::string& address);
    std::string CalculateMerkleRoot(const std::vector<std::string>& transactions);
    uint64_t GetNextBlockTimestamp();  // Returns consensus-safe candidate timestamp for the next block

    // Database integration
    bool ReadBlockchainTip(int64_t& height, std::string& hash, uint32_t& bits);
    std::vector<std::string> GetMempoolTransactions(size_t maxCount, uint64_t& totalFees);
    bool ValidateTransaction(const std::string& txHash);
    uint64_t GetTransactionFee(const std::string& txHash);
    size_t GetTransactionSize(const std::string& txHash);

    // Utility functions
    uint64_t GenerateTemplateId();
    bool IsWorkStale(uint64_t templateId);
    void CleanupStaleWork();

    // Configuration and state
    GBTConfig m_config;
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_shouldStop{false};

    // Current work state
    std::shared_ptr<WorkTemplate> m_currentWork;
    std::mutex m_workMutex;
    std::condition_variable m_workCondition;
    std::atomic<uint64_t> m_nextTemplateId{1};

    // Blockchain state tracking
    int64_t m_lastKnownHeight{-1};
    std::string m_lastKnownHash;
    uint32_t m_lastKnownBits{0};

    // Thread management
    std::thread m_refreshThread;
    std::thread m_monitorThread;

    // Statistics
    mutable GBTStats m_stats;

    // Callbacks
    WorkUpdateCallback m_workUpdateCallback;
    BlockSubmitCallback m_blockSubmitCallback;

    // Week 3: Context injection (removes dinero::legacy::g_blockchain() dependency)
    DaemonContext* m_context{nullptr};

    // Utreexo root computation oracle (single source of truth)
    dinero::consensus::BlockValidator* m_blockValidator{nullptr};

    // Constants
    static constexpr std::chrono::seconds DEFAULT_REFRESH_INTERVAL{30};
    static constexpr std::chrono::seconds TIP_MONITOR_INTERVAL{5};
    // Phase M.5.3: MIN_COINBASE_VALUE removed - mining now queries ConsensusSubsidy::GetBlockSubsidy(height)
    // Old value was 5000000000ULL (50 DIN) - WRONG! Consensus defines 100 DIN at height 2+
    static constexpr size_t MAX_COINBASE_SIZE = 100; // Max coinbase script size
};
