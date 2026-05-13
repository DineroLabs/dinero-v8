// doctor_types.h - Frozen contract types for dinerod doctor v1
// These types define the stable ABI/API contract for operators and automation.
// Do NOT rename or remove fields after release.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero {
namespace doctor {

// ─── Frozen Contracts ────────────────────────────────────────────────────────

// JSON schema version. Additive-only evolution after v1.0 release.
inline constexpr const char* kSchemaVersion = "1.0";

// Exit codes (stable contract — do NOT renumber)
enum class ExitCode : int {
    HEALTHY          = 0,  // All checks passed
    WARNINGS         = 1,  // Warnings only, no critical findings
    CRITICAL         = 2,  // Critical findings requiring operator action
    INTERNAL_ERROR   = 3,  // Doctor framework error (incomplete run, bug)
};

// ─── Check Metadata ──────────────────────────────────────────────────────────

enum class Severity {
    INFO,
    WARN,
    CRIT,
};

enum class CheckMode {
    QUICK,   // Runs in --quick (default) mode only
    DEEP,    // Runs in --deep mode only
    BOTH,    // Runs in both modes
};

enum class FixRisk {
    NONE,    // Read-only check, no fix available
    LOW,     // Safe auto-fix (directory creation, permission correction)
    MED,     // Requires confirmation (config changes)
    HIGH,    // Destructive or downtime-causing (reindex, DB repair)
};

enum class CheckStatus {
    PASS,
    WARN,
    CRIT,
    ERROR,   // Check itself failed (timeout, exception)
    SKIP,    // Skipped (dependency failed, filtered out)
};

// ─── Core Structures ─────────────────────────────────────────────────────────

struct DoctorCheckMetadata {
    std::string id;                              // e.g. "storage.disk_space"
    std::string description;                     // Human-readable description
    Severity severity_default = Severity::WARN;  // Default severity if check fails
    CheckMode mode = CheckMode::BOTH;
    FixRisk risk = FixRisk::NONE;
    std::vector<std::string> dependencies;       // Check IDs this depends on
    uint32_t timeout_budget_ms = 5000;           // Per-check timeout
};

struct FixAction {
    std::string id;                              // e.g. "storage.disk_space.cleanup_tmp"
    bool safe_to_apply = false;                  // Eligible for --apply-safe-fixes
    FixRisk risk = FixRisk::NONE;
    std::string expected_downtime;               // e.g. "none", "< 1 minute"
    std::vector<std::string> preconditions;      // Human-readable preconditions
    std::vector<std::string> steps;              // Exact deterministic steps/commands
    std::string rollback_notes;                  // Optional: how to undo
};

struct DoctorCheckResult {
    std::string id;                              // Matches DoctorCheckMetadata.id
    CheckStatus status = CheckStatus::PASS;
    std::string message;                         // Human-readable summary
    std::unordered_map<std::string, std::string> evidence;  // Structured k/v
    std::vector<FixAction> fix_plan;
    uint32_t duration_ms = 0;
    std::string started_at;                      // ISO 8601 timestamp
    std::string finished_at;                     // ISO 8601 timestamp
};

// ─── Run Configuration ───────────────────────────────────────────────────────

enum class RunMode {
    QUICK,
    DEEP,
};

struct DoctorConfig {
    RunMode mode = RunMode::QUICK;
    bool json_output = false;
    std::string json_output_path;                // Empty = stdout
    bool list_checks = false;
    std::string explain_check_id;                // Non-empty = explain mode
    std::vector<std::string> check_filter;       // Glob patterns to filter checks
    bool apply_safe_fixes = false;
    std::vector<std::string> fix_ids;            // Specific fixes to apply
    bool force_all_fixes = false;                // --yes-i-know-what-im-doing
};

// ─── Run Summary ─────────────────────────────────────────────────────────────

struct DoctorSummary {
    uint32_t critical = 0;
    uint32_t warnings = 0;
    uint32_t info = 0;
    uint32_t errors = 0;
    uint32_t skipped = 0;
    uint32_t passed = 0;
};

struct DoctorRunResult {
    ExitCode exit_code = ExitCode::HEALTHY;
    DoctorSummary summary;
    std::vector<DoctorCheckResult> results;
    std::string node_version;
    std::string network;
    std::string timestamp;                       // ISO 8601
    RunMode mode = RunMode::QUICK;
    uint32_t total_duration_ms = 0;
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

inline const char* to_string(ExitCode c) {
    switch (c) {
        case ExitCode::HEALTHY:        return "healthy";
        case ExitCode::WARNINGS:       return "warnings";
        case ExitCode::CRITICAL:       return "critical";
        case ExitCode::INTERNAL_ERROR: return "internal_error";
    }
    return "unknown";
}

inline const char* to_string(CheckStatus s) {
    switch (s) {
        case CheckStatus::PASS:  return "PASS";
        case CheckStatus::WARN:  return "WARN";
        case CheckStatus::CRIT:  return "CRIT";
        case CheckStatus::ERROR: return "ERROR";
        case CheckStatus::SKIP:  return "SKIP";
    }
    return "UNKNOWN";
}

inline const char* to_string(Severity s) {
    switch (s) {
        case Severity::INFO: return "INFO";
        case Severity::WARN: return "WARN";
        case Severity::CRIT: return "CRIT";
    }
    return "UNKNOWN";
}

inline const char* to_string(CheckMode m) {
    switch (m) {
        case CheckMode::QUICK: return "QUICK";
        case CheckMode::DEEP:  return "DEEP";
        case CheckMode::BOTH:  return "BOTH";
    }
    return "UNKNOWN";
}

inline const char* to_string(FixRisk r) {
    switch (r) {
        case FixRisk::NONE: return "NONE";
        case FixRisk::LOW:  return "LOW";
        case FixRisk::MED:  return "MED";
        case FixRisk::HIGH: return "HIGH";
    }
    return "UNKNOWN";
}

inline const char* to_string(RunMode m) {
    switch (m) {
        case RunMode::QUICK: return "quick";
        case RunMode::DEEP:  return "deep";
    }
    return "unknown";
}

} // namespace doctor
} // namespace dinero
