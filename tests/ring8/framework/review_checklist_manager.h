#pragma once

#include "change_types.h"
#include <map>
#include <vector>

namespace dinero {
namespace governance {
namespace test {

/**
 * Review Checklist Manager (Ring 8c: Quality assurance)
 *
 * Manages review checklists for protocol changes.
 * Ensures changes meet quality standards before merge.
 *
 * Key invariants:
 * - Critical/High impact changes require complete checklist
 * - All checklist items must be satisfied for approval
 * - Review evidence is documented
 */
class ReviewChecklistManager {
public:
    ReviewChecklistManager() = default;

    /**
     * Create standard checklist for change
     * Returns: ReviewChecklist with standard items based on impact level
     */
    ReviewChecklist createStandardChecklist(
        const ChangeProposal& proposal,
        const std::string& reviewer
    );

    /**
     * Add review to registry
     */
    void addReview(const ReviewChecklist& review);

    /**
     * Get reviews for change
     */
    std::vector<ReviewChecklist> getReviews(const std::string& change_id) const;

    /**
     * Check if change has been approved
     * Returns: true if at least one reviewer approved with complete checklist
     */
    bool isChangeApproved(const std::string& change_id) const;

    /**
     * Get approval count for change
     */
    size_t getApprovalCount(const std::string& change_id) const;

    /**
     * Check if all required checks are satisfied
     */
    bool allRequiredChecksSatisfied(const std::string& change_id) const;

    /**
     * Get standard checklist items for impact level
     */
    static std::vector<ReviewChecklistItem> getStandardChecklistItems(ImpactLevel impact);

private:
    // Change ID -> Reviews
    std::map<std::string, std::vector<ReviewChecklist>> reviews_;
};

} // namespace test
} // namespace governance
} // namespace dinero
