#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declaration
struct DaemonContext;

/**
 * @brief Mainnet mining safety validation system
 * 
 * Implements comprehensive safety gates to protect users from:
 * - Mining while out of sync (wasted work)
 * - Mining on battery (laptop protection)
 * - Mining to invalid addresses (lost rewards)
 * - Mining without understanding risks (user protection)
 */
class MiningSafetyGates {
public:
    struct SyncStatus {
        bool isIBD = false;              // Initial Block Download active
        int64_t blocksBehind = 0;        // Blocks behind network tip
        int64_t currentHeight = 0;       // Our current height
        int64_t networkHeight = 0;       // Estimated network height
        bool fullysynced = false;       // Ready for mining
    };

    struct ThermalStatus {
        double cpuTemp = 0.0;           // CPU temperature in Celsius
        bool thermalThrottling = false;  // System thermal throttling active
        bool safeToMine = true;         // Below thermal limits
        std::string thermalReason;      // Reason if not safe
    };

    struct BatteryStatus {
        bool onBattery = false;         // Running on battery power
        int batteryPercent = 100;       // Battery level 0-100
        bool lowBattery = false;        // < 20% battery
        bool safeToMine = true;         // Safe considering power state
        std::string batteryReason;      // Reason if not safe
    };

    struct AddressValidation {
        std::string address;            // Mining address to validate
        bool isValid = false;           // Address format valid
        bool isWalletAddress = false;   // Address belongs to our wallet
        bool isWatchOnly = false;       // Wallet is watch-only (can't spend)
        bool canReceive = true;         // Can receive mining rewards
        std::string validationError;    // Error message if invalid
    };

    struct SafetyResult {
        bool canStartMining = false;    // Overall safety check result
        std::string blockingReason;     // Primary reason if blocked
        std::vector<std::string> warnings; // Non-blocking warnings
        SyncStatus sync;
        ThermalStatus thermal;
        BatteryStatus battery;
        AddressValidation address;
    };

    // Main safety validation
    static SafetyResult ValidateMiningSafety(
        const std::string& miningAddress,
        const std::string& network,
        bool enableLocalMining,
        bool userUnderstands
    );

    // Individual gate checks
    static SyncStatus CheckSyncStatus();
    static ThermalStatus CheckThermalStatus();
    static BatteryStatus CheckBatteryStatus();
    static AddressValidation ValidateMiningAddress(const std::string& address);

    // Runtime monitoring (for auto-pause)
    static bool ShouldPauseMining();
    static std::string GetPauseReason();

    // Configuration
    static constexpr int64_t MAX_BLOCKS_BEHIND = 2;
    static constexpr double MAX_CPU_TEMP = 85.0;  // Celsius
    static constexpr int MIN_BATTERY_PERCENT = 20;

    // Week 3: Context injection for chainstate/wallet access
    static void SetContext(DaemonContext* ctx) { ctx_ = ctx; }

private:
    // Week 3: Static context pointer (replaces dinero::legacy::g_chain_db_direct() and dinero::legacy::g_wallet_manager() globals)
    static DaemonContext* ctx_;
};

/**
 * @brief Coinbase maturity tracking for UX
 */
class CoinbaseMaturity {
public:
    struct MaturityInfo {
        uint32_t blockHeight;           // Block height of coinbase
        int confirmations;              // Current confirmations
        int requiredConfirmations;      // Required for maturity (100)
        bool isMatured = false;         // Ready to spend
        int blocksRemaining = 0;        // Blocks until matured
        std::string timeEstimate;       // "~2 hours remaining"
    };

    // Get maturity info for a coinbase transaction
    static MaturityInfo GetMaturityInfo(uint32_t coinbaseHeight);
    
    // Get all immature coinbase transactions
    static std::vector<MaturityInfo> GetAllImmatureCoinbases();
    
    // Calculate time estimate for maturity
    static std::string EstimateMaturityTime(int blocksRemaining);

    static constexpr int COINBASE_MATURITY = 100; // Dinero maturity requirement
};
