#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>
#include "storage_interface.h"

namespace dinero {
namespace storage {

/**
 * Types of corruption that can be detected
 */
enum class CorruptionType {
    CHECKSUM_MISMATCH,      // Data checksum doesn't match expected
    INVALID_FORMAT,         // Data format is invalid or unrecognizable
    MISSING_DATA,           // Expected data is missing
    INCONSISTENT_STATE,     // Database state is internally inconsistent
    CHAIN_BREAK,            // Blockchain chain is broken or invalid
    INDEX_CORRUPTION,       // Index doesn't match actual data
    METADATA_CORRUPTION,    // Metadata is corrupted or invalid
    FILE_CORRUPTION,        // Underlying file system corruption
    MEMORY_CORRUPTION,      // In-memory data structures corrupted
    UNKNOWN                 // Unknown or unclassified corruption
};

/**
 * Severity levels for corruption
 */
enum class CorruptionSeverity {
    LOW,        // Minor corruption, system can continue
    MEDIUM,     // Moderate corruption, degraded operation
    HIGH,       // Serious corruption, limited functionality
    CRITICAL    // Critical corruption, system must halt
};

/**
 * Corruption detection result
 */
struct CorruptionEvent {
    CorruptionType type;
    CorruptionSeverity severity;
    std::string location;           // Where corruption was detected
    std::string description;        // Human-readable description
    std::string error_code;         // Specific error code for logging
    std::chrono::system_clock::time_point detected_at;
    std::vector<uint8_t> corrupted_data;  // Sample of corrupted data
    std::unordered_map<std::string, std::string> metadata;  // Additional context
    bool is_recoverable = false;    // Whether corruption can be recovered
    std::string recovery_suggestion; // How to recover from this corruption
};

/**
 * Corruption containment actions
 */
enum class ContainmentAction {
    CONTINUE,           // Continue operation despite corruption
    DEGRADE,            // Degrade functionality but continue
    ISOLATE,            // Isolate corrupted component
    HALT_WRITES,        // Stop all write operations
    HALT_READS,         // Stop all read operations  
    HALT_SYSTEM,        // Halt entire system
    EMERGENCY_BACKUP,   // Create emergency backup before halt
    NOTIFY_ADMIN        // Send alert to administrators
};

/**
 * Corruption detector interface
 */
class CorruptionDetector {
public:
    virtual ~CorruptionDetector() = default;
    
    /**
     * Check data for corruption
     */
    virtual std::vector<CorruptionEvent> checkData(const std::string& key, 
                                                  const std::vector<uint8_t>& data) = 0;
    
    /**
     * Check block for corruption
     */
    virtual std::vector<CorruptionEvent> checkBlock(const std::string& hash, 
                                                   const Block& block) = 0;
    
    /**
     * Check UTXO for corruption
     */
    virtual std::vector<CorruptionEvent> checkUTXO(const std::string& outpoint,
                                                   const std::vector<uint8_t>& utxo_data) = 0;
    
    /**
     * Check chain consistency
     */
    virtual std::vector<CorruptionEvent> checkChainConsistency() = 0;
    
    /**
     * Check storage backend integrity
     */
    virtual std::vector<CorruptionEvent> checkStorageIntegrity() = 0;
    
    /**
     * Get detector name
     */
    virtual std::string name() const = 0;
};

/**
 * Corruption containment manager
 * 
 * Handles detection, classification, and containment of storage corruption.
 * Provides configurable responses to different types and severities of corruption.
 */
class CorruptionContainment {
public:
    explicit CorruptionContainment(StorageInterface* storage);
    ~CorruptionContainment();
    
    // === Detection Registration ===
    
    /**
     * Register corruption detector
     */
    void registerDetector(std::unique_ptr<CorruptionDetector> detector);
    
    /**
     * Remove detector by name
     */
    void removeDetector(const std::string& name);
    
    // === Corruption Checking ===
    
    /**
     * Check data for corruption using all registered detectors
     */
    std::vector<CorruptionEvent> checkData(const std::string& key, 
                                          const std::vector<uint8_t>& data);
    
    /**
     * Check block for corruption
     */
    std::vector<CorruptionEvent> checkBlock(const std::string& hash, 
                                           const Block& block);
    
    /**
     * Check UTXO for corruption
     */
    std::vector<CorruptionEvent> checkUTXO(const std::string& outpoint,
                                          const std::vector<uint8_t>& utxo_data);
    
    /**
     * Perform comprehensive integrity check
     */
    std::vector<CorruptionEvent> performIntegrityCheck();
    
    /**
     * Check chain consistency
     */
    std::vector<CorruptionEvent> checkChainConsistency();
    
    // === Event Handling ===
    
    /**
     * Report corruption event
     */
    void reportCorruption(const CorruptionEvent& event);
    
    /**
     * Get all corruption events
     */
    std::vector<CorruptionEvent> getCorruptionEvents() const;
    
    /**
     * Get corruption events by severity
     */
    std::vector<CorruptionEvent> getCorruptionEvents(CorruptionSeverity min_severity) const;
    
    /**
     * Clear corruption event history
     */
    void clearCorruptionHistory();
    
    // === Containment Configuration ===
    
    /**
     * Set containment action for corruption type and severity
     */
    void setContainmentAction(CorruptionType type, 
                             CorruptionSeverity severity, 
                             ContainmentAction action);
    
    /**
     * Set default containment action for severity level
     */
    void setDefaultContainmentAction(CorruptionSeverity severity, 
                                    ContainmentAction action);
    
    /**
     * Set corruption event callback
     */
    void setCorruptionCallback(std::function<void(const CorruptionEvent&)> callback);
    
    /**
     * Set containment action callback
     */
    void setContainmentCallback(std::function<void(ContainmentAction, const CorruptionEvent&)> callback);
    
    // === System State Management ===
    
    /**
     * Check if system is in safe state
     */
    bool isSystemSafe() const;
    
    /**
     * Check if reads are allowed
     */
    bool areReadsAllowed() const;
    
    /**
     * Check if writes are allowed
     */
    bool areWritesAllowed() const;
    
    /**
     * Get current system state
     */
    std::string getSystemState() const;
    
    /**
     * Force system halt with reason
     */
    void forceSystemHalt(const std::string& reason);
    
    /**
     * Attempt system recovery
     */
    bool attemptRecovery();
    
    // === Statistics ===
    
    /**
     * Get corruption statistics
     */
    struct CorruptionStats {
        uint64_t total_events = 0;
        uint64_t events_by_type[static_cast<int>(CorruptionType::UNKNOWN) + 1] = {0};
        uint64_t events_by_severity[static_cast<int>(CorruptionSeverity::CRITICAL) + 1] = {0};
        uint64_t recoverable_events = 0;
        uint64_t system_halts = 0;
        std::chrono::system_clock::time_point last_event_time;
        std::chrono::system_clock::time_point last_halt_time;
    };
    
    CorruptionStats getStatistics() const;
    
    /**
     * Reset statistics
     */
    void resetStatistics();
    
    // === Configuration ===
    
    /**
     * Enable/disable automatic containment
     */
    void setAutomaticContainment(bool enabled);
    
    /**
     * Set maximum corruption events before halt
     */
    void setMaxCorruptionEvents(uint64_t max_events);
    
    /**
     * Set corruption event time window
     */
    void setCorruptionTimeWindow(std::chrono::seconds window);
    
    /**
     * Enable/disable emergency backup on critical corruption
     */
    void setEmergencyBackupEnabled(bool enabled);
    
private:
    StorageInterface* storage_;
    
    // Detectors
    std::vector<std::unique_ptr<CorruptionDetector>> detectors_;
    mutable std::mutex detectors_mutex_;
    
    // Event storage
    std::vector<CorruptionEvent> corruption_events_;
    mutable std::mutex events_mutex_;
    
    // Containment configuration
    std::unordered_map<std::string, ContainmentAction> containment_actions_;
    std::unordered_map<CorruptionSeverity, ContainmentAction> default_actions_;
    mutable std::mutex config_mutex_;
    
    // Callbacks
    std::function<void(const CorruptionEvent&)> corruption_callback_;
    std::function<void(ContainmentAction, const CorruptionEvent&)> containment_callback_;
    
    // System state
    std::atomic<bool> system_safe_{true};
    std::atomic<bool> reads_allowed_{true};
    std::atomic<bool> writes_allowed_{true};
    std::atomic<bool> system_halted_{false};
    std::string halt_reason_;
    mutable std::mutex state_mutex_;
    
    // Configuration
    std::atomic<bool> automatic_containment_{true};
    std::atomic<uint64_t> max_corruption_events_{100};
    std::atomic<uint64_t> corruption_time_window_seconds_{3600}; // 1 hour
    std::atomic<bool> emergency_backup_enabled_{true};
    
    // Statistics
    mutable CorruptionStats stats_;
    mutable std::mutex stats_mutex_;
    
    // Internal methods
    ContainmentAction getContainmentAction(const CorruptionEvent& event) const;
    void executeContainmentAction(ContainmentAction action, const CorruptionEvent& event);
    void updateStatistics(const CorruptionEvent& event);
    std::string makeContainmentKey(CorruptionType type, CorruptionSeverity severity) const;
    bool shouldTriggerEmergencyHalt() const;
    void performEmergencyBackup();
    void notifyAdministrators(const CorruptionEvent& event);
};

/**
 * Built-in corruption detectors
 */

/**
 * Checksum-based corruption detector
 */
class ChecksumCorruptionDetector : public CorruptionDetector {
public:
    ChecksumCorruptionDetector();
    
    std::vector<CorruptionEvent> checkData(const std::string& key, 
                                          const std::vector<uint8_t>& data) override;
    std::vector<CorruptionEvent> checkBlock(const std::string& hash, 
                                           const Block& block) override;
    std::vector<CorruptionEvent> checkUTXO(const std::string& outpoint,
                                          const std::vector<uint8_t>& utxo_data) override;
    std::vector<CorruptionEvent> checkChainConsistency() override;
    std::vector<CorruptionEvent> checkStorageIntegrity() override;
    
    std::string name() const override { return "checksum_detector"; }
    
private:
    uint32_t calculateChecksum(const std::vector<uint8_t>& data) const;
    bool validateBlockChecksum(const Block& block) const;
};

/**
 * Format validation corruption detector
 */
class FormatCorruptionDetector : public CorruptionDetector {
public:
    FormatCorruptionDetector();
    
    std::vector<CorruptionEvent> checkData(const std::string& key, 
                                          const std::vector<uint8_t>& data) override;
    std::vector<CorruptionEvent> checkBlock(const std::string& hash, 
                                           const Block& block) override;
    std::vector<CorruptionEvent> checkUTXO(const std::string& outpoint,
                                          const std::vector<uint8_t>& utxo_data) override;
    std::vector<CorruptionEvent> checkChainConsistency() override;
    std::vector<CorruptionEvent> checkStorageIntegrity() override;
    
    std::string name() const override { return "format_detector"; }
    
private:
    bool validateBlockFormat(const Block& block) const;
    bool validateUTXOFormat(const std::vector<uint8_t>& utxo_data) const;
    bool validateDataFormat(const std::string& key, const std::vector<uint8_t>& data) const;
};

/**
 * Chain consistency corruption detector
 */
class ChainConsistencyDetector : public CorruptionDetector {
public:
    explicit ChainConsistencyDetector(StorageInterface* storage);
    
    std::vector<CorruptionEvent> checkData(const std::string& key, 
                                          const std::vector<uint8_t>& data) override;
    std::vector<CorruptionEvent> checkBlock(const std::string& hash, 
                                           const Block& block) override;
    std::vector<CorruptionEvent> checkUTXO(const std::string& outpoint,
                                          const std::vector<uint8_t>& utxo_data) override;
    std::vector<CorruptionEvent> checkChainConsistency() override;
    std::vector<CorruptionEvent> checkStorageIntegrity() override;
    
    std::string name() const override { return "chain_consistency_detector"; }
    
private:
    StorageInterface* storage_;
    
    bool validateChainLink(const Block& block, const Block& prev_block) const;
    bool validateBlockHeight(const Block& block) const;
    bool validateUTXOConsistency(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) const;
};

} // namespace storage
} // namespace dinero
