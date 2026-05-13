#include "storage/corruption_detector.h"
#include "storage/storage_metrics.h"
#include "storage/backup_manager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <crc32c/crc32c.h>

namespace dinero {
namespace storage {

// CorruptionContainment implementation

CorruptionContainment::CorruptionContainment(StorageInterface* storage) 
    : storage_(storage) {
    
    // Set default containment actions
    default_actions_[CorruptionSeverity::LOW] = ContainmentAction::CONTINUE;
    default_actions_[CorruptionSeverity::MEDIUM] = ContainmentAction::DEGRADE;
    default_actions_[CorruptionSeverity::HIGH] = ContainmentAction::HALT_WRITES;
    default_actions_[CorruptionSeverity::CRITICAL] = ContainmentAction::HALT_SYSTEM;
    
    // Register built-in detectors
    registerDetector(std::make_unique<ChecksumCorruptionDetector>());
    registerDetector(std::make_unique<FormatCorruptionDetector>());
    registerDetector(std::make_unique<ChainConsistencyDetector>(storage_));
}

CorruptionContainment::~CorruptionContainment() = default;

void CorruptionContainment::registerDetector(std::unique_ptr<CorruptionDetector> detector) {
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    detectors_.push_back(std::move(detector));
}

void CorruptionContainment::removeDetector(const std::string& name) {
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    detectors_.erase(
        std::remove_if(detectors_.begin(), detectors_.end(),
                      [&name](const auto& detector) { return detector->name() == name; }),
        detectors_.end());
}

std::vector<CorruptionEvent> CorruptionContainment::checkData(const std::string& key, 
                                                             const std::vector<uint8_t>& data) {
    std::vector<CorruptionEvent> events;
    
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    for (const auto& detector : detectors_) {
        try {
            auto detector_events = detector->checkData(key, data);
            events.insert(events.end(), detector_events.begin(), detector_events.end());
        } catch (const std::exception& e) {
            // Log detector failure but continue with other detectors
            if (g_storage_metrics) {
                g_storage_metrics->recordError("corruption_detection", "detector_failure");
            }
        }
    }
    
    // Process detected corruption events
    for (const auto& event : events) {
        reportCorruption(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> CorruptionContainment::checkBlock(const std::string& hash, 
                                                              const Block& block) {
    std::vector<CorruptionEvent> events;
    
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    for (const auto& detector : detectors_) {
        try {
            auto detector_events = detector->checkBlock(hash, block);
            events.insert(events.end(), detector_events.begin(), detector_events.end());
        } catch (const std::exception& e) {
            if (g_storage_metrics) {
                g_storage_metrics->recordError("corruption_detection", "block_check_failure");
            }
        }
    }
    
    for (const auto& event : events) {
        reportCorruption(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> CorruptionContainment::checkUTXO(const std::string& outpoint,
                                                             const std::vector<uint8_t>& utxo_data) {
    std::vector<CorruptionEvent> events;
    
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    for (const auto& detector : detectors_) {
        try {
            auto detector_events = detector->checkUTXO(outpoint, utxo_data);
            events.insert(events.end(), detector_events.begin(), detector_events.end());
        } catch (const std::exception& e) {
            if (g_storage_metrics) {
                g_storage_metrics->recordError("corruption_detection", "utxo_check_failure");
            }
        }
    }
    
    for (const auto& event : events) {
        reportCorruption(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> CorruptionContainment::performIntegrityCheck() {
    std::vector<CorruptionEvent> all_events;
    
    std::lock_guard<std::mutex> lock(detectors_mutex_);
    for (const auto& detector : detectors_) {
        try {
            auto storage_events = detector->checkStorageIntegrity();
            all_events.insert(all_events.end(), storage_events.begin(), storage_events.end());
            
            auto chain_events = detector->checkChainConsistency();
            all_events.insert(all_events.end(), chain_events.begin(), chain_events.end());
        } catch (const std::exception& e) {
            if (g_storage_metrics) {
                g_storage_metrics->recordError("corruption_detection", "integrity_check_failure");
            }
        }
    }
    
    for (const auto& event : all_events) {
        reportCorruption(event);
    }
    
    return all_events;
}

void CorruptionContainment::reportCorruption(const CorruptionEvent& event) {
    // Store event
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        corruption_events_.append(event);
    }
    
    // Update statistics
    updateStatistics(event);
    
    // Execute callback if set
    if (corruption_callback_) {
        try {
            corruption_callback_(event);
        } catch (...) {
            // Ignore callback failures
        }
    }
    
    // Execute containment action if automatic containment is enabled
    if (automatic_containment_) {
        ContainmentAction action = getContainmentAction(event);
        executeContainmentAction(action, event);
    }
    
    // Check if emergency halt should be triggered
    if (shouldTriggerEmergencyHalt()) {
        forceSystemHalt("Too many corruption events detected");
    }
}

ContainmentAction CorruptionContainment::getContainmentAction(const CorruptionEvent& event) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    // Check for specific type+severity combination
    std::string key = makeContainmentKey(event.type, event.severity);
    auto it = containment_actions_.find(key);
    if (it != containment_actions_.end()) {
        return it->second;
    }
    
    // Fall back to default action for severity
    auto default_it = default_actions_.find(event.severity);
    if (default_it != default_actions_.end()) {
        return default_it->second;
    }
    
    // Ultimate fallback
    return ContainmentAction::HALT_SYSTEM;
}

void CorruptionContainment::executeContainmentAction(ContainmentAction action, const CorruptionEvent& event) {
    // Execute callback if set
    if (containment_callback_) {
        try {
            containment_callback_(action, event);
        } catch (...) {
            // Ignore callback failures
        }
    }
    
    switch (action) {
        case ContainmentAction::CONTINUE:
            // No action needed
            break;
            
        case ContainmentAction::DEGRADE:
            // System continues but with degraded functionality
            // This could involve disabling certain features
            break;
            
        case ContainmentAction::ISOLATE:
            // Isolate the corrupted component
            // Implementation depends on what was corrupted
            break;
            
        case ContainmentAction::HALT_WRITES:
            writes_allowed_ = false;
            system_safe_ = false;
            break;
            
        case ContainmentAction::HALT_READS:
            reads_allowed_ = false;
            system_safe_ = false;
            break;
            
        case ContainmentAction::HALT_SYSTEM:
            forceSystemHalt("Critical corruption detected: " + event.description);
            break;
            
        case ContainmentAction::EMERGENCY_BACKUP:
            if (emergency_backup_enabled_) {
                performEmergencyBackup();
            }
            break;
            
        case ContainmentAction::NOTIFY_ADMIN:
            notifyAdministrators(event);
            break;
    }
    
    // Record metrics
    if (g_storage_metrics) {
        g_storage_metrics->recordCorruptionEvent(static_cast<int>(event.type), 
                                               static_cast<int>(event.severity),
                                               static_cast<int>(action));
    }
}

bool CorruptionContainment::isSystemSafe() const {
    return system_safe_ && !system_halted_;
}

bool CorruptionContainment::areReadsAllowed() const {
    return reads_allowed_ && !system_halted_;
}

bool CorruptionContainment::areWritesAllowed() const {
    return writes_allowed_ && !system_halted_;
}

void CorruptionContainment::forceSystemHalt(const std::string& reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    system_halted_ = true;
    system_safe_ = false;
    reads_allowed_ = false;
    writes_allowed_ = false;
    halt_reason_ = reason;
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.system_halts++;
        stats_.last_halt_time = std::chrono::system_clock::now();
    }
    
    // Record metrics
    if (g_storage_metrics) {
        g_storage_metrics->recordSystemHalt(reason);
    }
}

// ChecksumCorruptionDetector implementation

ChecksumCorruptionDetector::ChecksumCorruptionDetector() = default;

std::vector<CorruptionEvent> ChecksumCorruptionDetector::checkData(const std::string& key, 
                                                                  const std::vector<uint8_t>& data) {
    std::vector<CorruptionEvent> events;
    
    // Basic checksum validation
    if (data.empty()) {
        CorruptionEvent event;
        event.type = CorruptionType::MISSING_DATA;
        event.severity = CorruptionSeverity::MEDIUM;
        event.location = key;
        event.description = "Data is empty";
        event.error_code = "CHECKSUM_001";
        event.detected_at = std::chrono::system_clock::now();
        event.is_recoverable = false;
        events.append(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> ChecksumCorruptionDetector::checkBlock(const std::string& hash, 
                                                                   const Block& block) {
    std::vector<CorruptionEvent> events;
    
    if (!validateBlockChecksum(block)) {
        CorruptionEvent event;
        event.type = CorruptionType::CHECKSUM_MISMATCH;
        event.severity = CorruptionSeverity::HIGH;
        event.location = "block:" + hash;
        event.description = "Block checksum validation failed";
        event.error_code = "CHECKSUM_002";
        event.detected_at = std::chrono::system_clock::now();
        event.is_recoverable = false;
        event.recovery_suggestion = "Re-download block from network";
        events.append(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> ChecksumCorruptionDetector::checkUTXO(const std::string& outpoint,
                                                                  const std::vector<uint8_t>& utxo_data) {
    std::vector<CorruptionEvent> events;
    
    // Validate UTXO data integrity
    if (utxo_data.size() < 8) { // Minimum UTXO size
        CorruptionEvent event;
        event.type = CorruptionType::INVALID_FORMAT;
        event.severity = CorruptionSeverity::HIGH;
        event.location = "utxo:" + outpoint;
        event.description = "UTXO data too small";
        event.error_code = "CHECKSUM_003";
        event.detected_at = std::chrono::system_clock::now();
        event.corrupted_data = utxo_data;
        event.is_recoverable = false;
        events.append(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> ChecksumCorruptionDetector::checkChainConsistency() {
    // Chain consistency is handled by ChainConsistencyDetector
    return {};
}

std::vector<CorruptionEvent> ChecksumCorruptionDetector::checkStorageIntegrity() {
    // Storage integrity checks would go here
    return {};
}

uint32_t ChecksumCorruptionDetector::calculateChecksum(const std::vector<uint8_t>& data) const {
    return crc32c::Crc32c(data.data(), data.size());
}

bool ChecksumCorruptionDetector::validateBlockChecksum(const Block& block) const {
    // TODO: Implement actual block checksum validation
    return true;
}

// FormatCorruptionDetector implementation

FormatCorruptionDetector::FormatCorruptionDetector() = default;

std::vector<CorruptionEvent> FormatCorruptionDetector::checkData(const std::string& key, 
                                                                const std::vector<uint8_t>& data) {
    std::vector<CorruptionEvent> events;
    
    if (!validateDataFormat(key, data)) {
        CorruptionEvent event;
        event.type = CorruptionType::INVALID_FORMAT;
        event.severity = CorruptionSeverity::MEDIUM;
        event.location = key;
        event.description = "Data format validation failed";
        event.error_code = "FORMAT_001";
        event.detected_at = std::chrono::system_clock::now();
        event.corrupted_data = data.size() > 100 ? 
            std::vector<uint8_t>(data.begin(), data.begin() + 100) : data;
        event.is_recoverable = false;
        events.append(event);
    }
    
    return events;
}

std::vector<CorruptionEvent> FormatCorruptionDetector::checkBlock(const std::string& hash, 
                                                                 const Block& block) {
    std::vector<CorruptionEvent> events;
    
    if (!validateBlockFormat(block)) {
        CorruptionEvent event;
        event.type = CorruptionType::INVALID_FORMAT;
        event.severity = CorruptionSeverity::CRITICAL;
        event.location = "block:" + hash;
        event.description = "Block format is invalid";
        event.error_code = "FORMAT_002";
        event.detected_at = std::chrono::system_clock::now();
        event.is_recoverable = false;
        event.recovery_suggestion = "Re-download block from network";
        events.append(event);
    }
    
    return events;
}

bool FormatCorruptionDetector::validateBlockFormat(const Block& block) const {
    // TODO: Implement actual block format validation
    return true;
}

bool FormatCorruptionDetector::validateUTXOFormat(const std::vector<uint8_t>& utxo_data) const {
    // TODO: Implement UTXO format validation
    return utxo_data.size() >= 8;
}

bool FormatCorruptionDetector::validateDataFormat(const std::string& key, const std::vector<uint8_t>& data) const {
    // Basic format validation based on key prefix
    if (key.starts_with("block:")) {
        return data.size() > 80; // Minimum block header size
    } else if (key.starts_with("utxo:")) {
        return validateUTXOFormat(data);
    } else if (key.starts_with("tx:")) {
        return data.size() > 10; // Minimum transaction size
    }
    
    return true; // Unknown format, assume valid
}

// ChainConsistencyDetector implementation

ChainConsistencyDetector::ChainConsistencyDetector(StorageInterface* storage) 
    : storage_(storage) {}

std::vector<CorruptionEvent> ChainConsistencyDetector::checkChainConsistency() {
    std::vector<CorruptionEvent> events;
    
    // TODO: Implement comprehensive chain consistency checks
    // This would involve:
    // 1. Verifying block chain links
    // 2. Checking UTXO set consistency
    // 3. Validating transaction references
    // 4. Ensuring no double spends
    
    return events;
}

// Utility methods

void CorruptionContainment::updateStatistics(const CorruptionEvent& event) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_events++;
    stats_.events_by_type[static_cast<int>(event.type)]++;
    stats_.events_by_severity[static_cast<int>(event.severity)]++;
    
    if (event.is_recoverable) {
        stats_.recoverable_events++;
    }
    
    stats_.last_event_time = event.detected_at;
}

std::string CorruptionContainment::makeContainmentKey(CorruptionType type, CorruptionSeverity severity) const {
    return std::to_string(static_cast<int>(type)) + ":" + std::to_string(static_cast<int>(severity));
}

bool CorruptionContainment::shouldTriggerEmergencyHalt() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Check if too many events in time window
    auto now = std::chrono::system_clock::now();
    auto window = std::chrono::seconds(corruption_time_window_seconds_);
    
    uint64_t recent_events = 0;
    for (const auto& event : corruption_events_) {
        if (now - event.detected_at < window) {
            recent_events++;
        }
    }
    
    return recent_events >= max_corruption_events_;
}

void CorruptionContainment::performEmergencyBackup() {
    try {
        if (storage_) {
            auto backup_manager = BackupManagerFactory::create(storage_);
            std::string emergency_path = "/tmp/emergency_backup_" + 
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            
            BackupOptions options;
            options.backup_id = "emergency_corruption_backup";
            options.verify_backup = false; // Skip verification for speed
            
            backup_manager->createBackup(emergency_path, options);
        }
    } catch (...) {
        // Emergency backup failed, but don't let this stop the halt
    }
}

void CorruptionContainment::notifyAdministrators(const CorruptionEvent& event) {
    // TODO: Implement administrator notification
    // This could involve:
    // 1. Sending email alerts
    // 2. Writing to syslog
    // 3. Triggering monitoring system alerts
    // 4. Creating incident tickets
}

} // namespace storage
} // namespace dinero
