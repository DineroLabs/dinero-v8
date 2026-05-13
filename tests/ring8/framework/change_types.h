#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace dinero {
namespace governance {
namespace test {

//=============================================================================
// Change Category (What type of change)
//=============================================================================

enum class ChangeCategory {
    CONSENSUS_RULE,         // Consensus rule modification
    EXTENSION_ADDITION,     // New extension (Ring 8b)
    SECURITY_FIX,          // Security vulnerability fix
    BUG_FIX,               // Non-security bug fix
    PERFORMANCE,           // Performance optimization
    REFACTORING,           // Code refactoring (no semantic change)
    DOCUMENTATION          // Documentation update
};

//=============================================================================
// Impact Level (How significant is the change)
//=============================================================================

enum class ImpactLevel {
    CRITICAL,              // Breaks consensus, requires hard fork
    HIGH,                  // Affects consensus, soft fork possible
    MEDIUM,                // Affects behavior, backward compatible
    LOW                    // No behavior change (refactoring, docs)
};

//=============================================================================
// Change Status (Lifecycle)
//=============================================================================

enum class ChangeStatus {
    PROPOSED,              // Change proposed, under review
    REVIEWED,              // Change reviewed, approved
    IMPLEMENTED,           // Change implemented in code
    TESTED,                // Change tested
    MERGED,                // Change merged to main branch
    ACTIVATED,             // Change activated on network
    REJECTED,              // Change rejected
    SUPERSEDED             // Change superseded by another
};

//=============================================================================
// Change Proposal (Ring 8c: CL1 - Change Documentation)
//=============================================================================

struct ChangeProposal {
    // Metadata
    std::string id;                             // Unique identifier (e.g., "CIP-001")
    std::string title;                          // Short title
    std::string description;                    // Detailed description

    // Classification
    ChangeCategory category;                    // What type of change
    ImpactLevel impact;                         // How significant
    ChangeStatus status;                        // Current status

    // Rationale (CL2: Rationale Traceability)
    std::string motivation;                     // Why is this change needed?
    std::string rationale;                      // How does this solve the problem?
    std::vector<std::string> alternatives;      // What alternatives were considered?
    std::string decision_rationale;             // Why this approach over alternatives?

    // Technical Details
    std::vector<std::string> affected_components;  // What code is affected?
    std::vector<std::string> ring_impact;          // Which rings are affected?
    std::optional<std::string> extension_id;       // Extension ID (if Ring 8b)

    // Review & Testing
    std::vector<std::string> reviewers;         // Who reviewed this?
    std::vector<std::string> test_coverage;     // What tests cover this?
    bool backward_compatible;                   // Is it backward compatible?

    // Timeline
    uint64_t proposed_at;                       // Timestamp
    uint64_t reviewed_at;                       // Timestamp
    uint64_t merged_at;                         // Timestamp
    std::optional<uint64_t> activation_height;  // Block height (if applicable)

    ChangeProposal(const std::string& change_id, const std::string& change_title)
        : id(change_id), title(change_title),
          category(ChangeCategory::CONSENSUS_RULE),
          impact(ImpactLevel::MEDIUM),
          status(ChangeStatus::PROPOSED),
          backward_compatible(true),
          proposed_at(0), reviewed_at(0), merged_at(0) {}
};

//=============================================================================
// Audit Entry (Ring 8c: CL3 - Audit Trail)
//=============================================================================

struct AuditEntry {
    uint64_t timestamp;                         // When did this happen?
    std::string change_id;                      // Which change?
    std::string action;                         // What action? (proposed, reviewed, merged, etc.)
    std::string actor;                          // Who did it?
    std::string details;                        // Additional details
    std::optional<std::string> previous_status; // Previous status (for transitions)
    std::optional<std::string> new_status;      // New status (for transitions)

    AuditEntry(uint64_t ts, const std::string& cid, const std::string& act)
        : timestamp(ts), change_id(cid), action(act) {}
};

//=============================================================================
// Review Checklist (Ring 8c: Quality assurance)
//=============================================================================

struct ReviewChecklistItem {
    std::string requirement;                    // What must be checked?
    bool satisfied;                             // Is it satisfied?
    std::optional<std::string> evidence;        // Evidence (test name, file path, etc.)
    std::optional<std::string> notes;           // Reviewer notes

    ReviewChecklistItem(const std::string& req)
        : requirement(req), satisfied(false) {}
};

struct ReviewChecklist {
    std::string change_id;                      // Which change?
    std::string reviewer;                       // Who is reviewing?
    uint64_t review_date;                       // When?

    // Checklist items
    std::vector<ReviewChecklistItem> items;

    // Overall assessment
    bool approved;                              // Is change approved?
    std::optional<std::string> concerns;        // Any concerns?
    std::optional<std::string> recommendations; // Recommendations

    ReviewChecklist(const std::string& cid, const std::string& rev)
        : change_id(cid), reviewer(rev), review_date(0), approved(false) {}

    void addItem(const ReviewChecklistItem& item) {
        items.push_back(item);
    }

    bool allItemsSatisfied() const {
        for (const auto& item : items) {
            if (!item.satisfied) {
                return false;
            }
        }
        return true;
    }
};

//=============================================================================
// Documentation Violation (For CL1-CL3 testing)
//=============================================================================

struct DocumentationViolation {
    std::string property_name;                  // CL1, CL2, or CL3
    std::string description;                    // What was violated
    std::string change_id;                      // Which change?
    std::string details;                        // Additional details

    DocumentationViolation(const std::string& prop, const std::string& desc, const std::string& cid)
        : property_name(prop), description(desc), change_id(cid) {}
};

} // namespace test
} // namespace governance
} // namespace dinero
