#pragma once

#include "change_types.h"
#include <map>
#include <vector>
#include <memory>

namespace dinero {
namespace governance {
namespace test {

/**
 * Change Audit Log (Ring 8c: CL3 - Audit Trail)
 *
 * Maintains complete audit trail of all protocol changes.
 * Ensures every change is traceable from proposal to activation.
 *
 * Key invariants:
 * - Every change has complete audit trail
 * - No gaps in audit trail (all state transitions recorded)
 * - Immutable audit history (entries cannot be deleted/modified)
 */
class ChangeAuditLog {
public:
    ChangeAuditLog() = default;

    /**
     * Record a change proposal
     * Returns: true if recorded, false if duplicate
     */
    bool recordProposal(const ChangeProposal& proposal);

    /**
     * Record a change status transition
     */
    void recordStatusChange(
        const std::string& change_id,
        ChangeStatus old_status,
        ChangeStatus new_status,
        const std::string& actor
    );

    /**
     * Record a generic audit event
     */
    void recordEvent(const AuditEntry& entry);

    /**
     * Get all proposals
     */
    std::vector<ChangeProposal> getAllProposals() const;

    /**
     * Get proposal by ID
     */
    std::optional<ChangeProposal> getProposal(const std::string& change_id) const;

    /**
     * Get audit trail for specific change
     */
    std::vector<AuditEntry> getAuditTrail(const std::string& change_id) const;

    /**
     * Get all audit entries (chronological)
     */
    std::vector<AuditEntry> getAllAuditEntries() const;

    /**
     * CL1 Property Check: All changes documented
     * Returns: empty vector if all documented, violations if not
     */
    std::vector<DocumentationViolation> checkAllChangesDocumented() const;

    /**
     * CL2 Property Check: Rationale traceability
     * Returns: empty vector if traceable, violations if not
     */
    std::vector<DocumentationViolation> checkRationaleTraceability() const;

    /**
     * CL3 Property Check: Complete audit trail
     * Returns: empty vector if complete, violations if not
     */
    std::vector<DocumentationViolation> checkAuditTrailComplete() const;

    /**
     * Get changes by status
     */
    std::vector<ChangeProposal> getChangesByStatus(ChangeStatus status) const;

    /**
     * Get changes by category
     */
    std::vector<ChangeProposal> getChangesByCategory(ChangeCategory category) const;

    /**
     * Update proposal status
     */
    bool updateProposalStatus(const std::string& change_id, ChangeStatus new_status);

private:
    // Change ID -> Proposal
    std::map<std::string, ChangeProposal> proposals_;

    // Audit trail (chronological)
    std::vector<AuditEntry> audit_trail_;

    // Next audit entry ID
    uint64_t next_entry_id_;

    uint64_t getCurrentTimestamp() const;
    bool isValidStatusTransition(ChangeStatus from, ChangeStatus to) const;
};

} // namespace test
} // namespace governance
} // namespace dinero
