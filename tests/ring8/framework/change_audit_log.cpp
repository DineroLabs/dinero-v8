#include "change_audit_log.h"
#include <algorithm>
#include <chrono>

namespace dinero {
namespace governance {
namespace test {

bool ChangeAuditLog::recordProposal(const ChangeProposal& proposal) {
    // Check if already exists
    if (proposals_.find(proposal.id) != proposals_.end()) {
        return false;  // Duplicate proposal ID
    }

    // Record the proposal
    proposals_.insert({proposal.id, proposal});

    // Record audit entry
    AuditEntry entry(getCurrentTimestamp(), proposal.id, "PROPOSED");
    entry.actor = "proposer";
    entry.details = proposal.title;
    entry.new_status = "PROPOSED";
    audit_trail_.push_back(entry);

    return true;
}

void ChangeAuditLog::recordStatusChange(
    const std::string& change_id,
    ChangeStatus old_status,
    ChangeStatus new_status,
    const std::string& actor)
{
    // Create audit entry
    AuditEntry entry(getCurrentTimestamp(), change_id, "STATUS_CHANGE");
    entry.actor = actor;

    // Convert statuses to strings for audit log
    auto statusToString = [](ChangeStatus s) -> std::string {
        switch (s) {
            case ChangeStatus::PROPOSED: return "PROPOSED";
            case ChangeStatus::REVIEWED: return "REVIEWED";
            case ChangeStatus::IMPLEMENTED: return "IMPLEMENTED";
            case ChangeStatus::TESTED: return "TESTED";
            case ChangeStatus::MERGED: return "MERGED";
            case ChangeStatus::ACTIVATED: return "ACTIVATED";
            case ChangeStatus::REJECTED: return "REJECTED";
            case ChangeStatus::SUPERSEDED: return "SUPERSEDED";
        }
        return "UNKNOWN";
    };

    entry.previous_status = statusToString(old_status);
    entry.new_status = statusToString(new_status);
    entry.details = "Transitioned from " + *entry.previous_status + " to " + *entry.new_status;

    audit_trail_.push_back(entry);
}

void ChangeAuditLog::recordEvent(const AuditEntry& entry) {
    audit_trail_.push_back(entry);
}

std::vector<ChangeProposal> ChangeAuditLog::getAllProposals() const {
    std::vector<ChangeProposal> result;
    for (const auto& [id, proposal] : proposals_) {
        result.push_back(proposal);
    }
    return result;
}

std::optional<ChangeProposal> ChangeAuditLog::getProposal(const std::string& change_id) const {
    auto it = proposals_.find(change_id);
    if (it == proposals_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<AuditEntry> ChangeAuditLog::getAuditTrail(const std::string& change_id) const {
    std::vector<AuditEntry> result;
    for (const auto& entry : audit_trail_) {
        if (entry.change_id == change_id) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<AuditEntry> ChangeAuditLog::getAllAuditEntries() const {
    return audit_trail_;
}

std::vector<DocumentationViolation> ChangeAuditLog::checkAllChangesDocumented() const {
    std::vector<DocumentationViolation> violations;

    // CL1: Every change must have complete documentation
    for (const auto& [id, proposal] : proposals_) {
        // Check required fields
        if (proposal.title.empty()) {
            violations.push_back(DocumentationViolation(
                "CL1", "Missing title", id
            ));
        }

        if (proposal.description.empty()) {
            violations.push_back(DocumentationViolation(
                "CL1", "Missing description", id
            ));
        }

        if (proposal.affected_components.empty()) {
            violations.push_back(DocumentationViolation(
                "CL1", "Missing affected components", id
            ));
        }
    }

    return violations;
}

std::vector<DocumentationViolation> ChangeAuditLog::checkRationaleTraceability() const {
    std::vector<DocumentationViolation> violations;

    // CL2: Every change must have clear rationale
    for (const auto& [id, proposal] : proposals_) {
        // Check rationale fields
        if (proposal.motivation.empty()) {
            violations.push_back(DocumentationViolation(
                "CL2", "Missing motivation (why is this change needed?)", id
            ));
        }

        if (proposal.rationale.empty()) {
            violations.push_back(DocumentationViolation(
                "CL2", "Missing rationale (how does this solve the problem?)", id
            ));
        }

        // For non-trivial changes, require alternatives analysis
        if (proposal.impact != ImpactLevel::LOW) {
            if (proposal.alternatives.empty()) {
                violations.push_back(DocumentationViolation(
                    "CL2", "Missing alternatives analysis for non-trivial change", id
                ));
            }

            if (proposal.decision_rationale.empty()) {
                violations.push_back(DocumentationViolation(
                    "CL2", "Missing decision rationale (why this approach over alternatives?)", id
                ));
            }
        }
    }

    return violations;
}

std::vector<DocumentationViolation> ChangeAuditLog::checkAuditTrailComplete() const {
    std::vector<DocumentationViolation> violations;

    // CL3: Every change must have complete audit trail
    for (const auto& [id, proposal] : proposals_) {
        auto trail = getAuditTrail(id);

        // Must have at least one entry (proposal)
        if (trail.empty()) {
            violations.push_back(DocumentationViolation(
                "CL3", "No audit trail entries", id
            ));
            continue;
        }

        // Check for status transitions based on current status
        if (proposal.status == ChangeStatus::MERGED ||
            proposal.status == ChangeStatus::ACTIVATED) {
            // Must have PROPOSED → REVIEWED → IMPLEMENTED → TESTED → MERGED
            bool has_reviewed = false;
            bool has_implemented = false;
            bool has_tested = false;

            for (const auto& entry : trail) {
                if (entry.new_status == "REVIEWED") has_reviewed = true;
                if (entry.new_status == "IMPLEMENTED") has_implemented = true;
                if (entry.new_status == "TESTED") has_tested = true;
            }

            if (!has_reviewed) {
                violations.push_back(DocumentationViolation(
                    "CL3", "Missing REVIEWED status in audit trail", id
                ));
            }
            if (!has_implemented) {
                violations.push_back(DocumentationViolation(
                    "CL3", "Missing IMPLEMENTED status in audit trail", id
                ));
            }
            if (!has_tested) {
                violations.push_back(DocumentationViolation(
                    "CL3", "Missing TESTED status in audit trail", id
                ));
            }
        }
    }

    return violations;
}

std::vector<ChangeProposal> ChangeAuditLog::getChangesByStatus(ChangeStatus status) const {
    std::vector<ChangeProposal> result;
    for (const auto& [id, proposal] : proposals_) {
        if (proposal.status == status) {
            result.push_back(proposal);
        }
    }
    return result;
}

std::vector<ChangeProposal> ChangeAuditLog::getChangesByCategory(ChangeCategory category) const {
    std::vector<ChangeProposal> result;
    for (const auto& [id, proposal] : proposals_) {
        if (proposal.category == category) {
            result.push_back(proposal);
        }
    }
    return result;
}

bool ChangeAuditLog::updateProposalStatus(const std::string& change_id, ChangeStatus new_status) {
    auto it = proposals_.find(change_id);
    if (it == proposals_.end()) {
        return false;
    }

    ChangeStatus old_status = it->second.status;

    // Validate transition
    if (!isValidStatusTransition(old_status, new_status)) {
        return false;
    }

    // Update status
    it->second.status = new_status;

    // Record status change
    recordStatusChange(change_id, old_status, new_status, "system");

    return true;
}

uint64_t ChangeAuditLog::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

bool ChangeAuditLog::isValidStatusTransition(ChangeStatus from, ChangeStatus to) const {
    // Define valid transitions
    // PROPOSED → REVIEWED, REJECTED
    // REVIEWED → IMPLEMENTED, REJECTED
    // IMPLEMENTED → TESTED, REJECTED
    // TESTED → MERGED, REJECTED
    // MERGED → ACTIVATED, SUPERSEDED
    // (Terminal states: REJECTED, SUPERSEDED, ACTIVATED)

    using CS = ChangeStatus;

    if (from == CS::PROPOSED) {
        return to == CS::REVIEWED || to == CS::REJECTED;
    }
    if (from == CS::REVIEWED) {
        return to == CS::IMPLEMENTED || to == CS::REJECTED;
    }
    if (from == CS::IMPLEMENTED) {
        return to == CS::TESTED || to == CS::REJECTED;
    }
    if (from == CS::TESTED) {
        return to == CS::MERGED || to == CS::REJECTED;
    }
    if (from == CS::MERGED) {
        return to == CS::ACTIVATED || to == CS::SUPERSEDED;
    }

    // Terminal states: no transitions allowed
    return false;
}

} // namespace test
} // namespace governance
} // namespace dinero
