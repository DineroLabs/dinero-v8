#include "review_checklist_manager.h"
#include <chrono>

namespace dinero {
namespace governance {
namespace test {

ReviewChecklist ReviewChecklistManager::createStandardChecklist(
    const ChangeProposal& proposal,
    const std::string& reviewer)
{
    ReviewChecklist checklist(proposal.id, reviewer);

    auto now = std::chrono::system_clock::now();
    checklist.review_date = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();

    // Get standard items based on impact level
    checklist.items = getStandardChecklistItems(proposal.impact);

    return checklist;
}

void ReviewChecklistManager::addReview(const ReviewChecklist& review) {
    reviews_[review.change_id].push_back(review);
}

std::vector<ReviewChecklist> ReviewChecklistManager::getReviews(const std::string& change_id) const {
    auto it = reviews_.find(change_id);
    if (it == reviews_.end()) {
        return {};
    }
    return it->second;
}

bool ReviewChecklistManager::isChangeApproved(const std::string& change_id) const {
    auto reviews = getReviews(change_id);

    for (const auto& review : reviews) {
        if (review.approved && review.allItemsSatisfied()) {
            return true;
        }
    }

    return false;
}

size_t ReviewChecklistManager::getApprovalCount(const std::string& change_id) const {
    auto reviews = getReviews(change_id);
    size_t count = 0;

    for (const auto& review : reviews) {
        if (review.approved) {
            count++;
        }
    }

    return count;
}

bool ReviewChecklistManager::allRequiredChecksSatisfied(const std::string& change_id) const {
    auto reviews = getReviews(change_id);

    // Need at least one review
    if (reviews.empty()) {
        return false;
    }

    // Check if at least one review has all items satisfied
    for (const auto& review : reviews) {
        if (review.allItemsSatisfied()) {
            return true;
        }
    }

    return false;
}

std::vector<ReviewChecklistItem> ReviewChecklistManager::getStandardChecklistItems(ImpactLevel impact) {
    std::vector<ReviewChecklistItem> items;

    // Common items for all changes
    items.push_back(ReviewChecklistItem("Change has clear title and description"));
    items.push_back(ReviewChecklistItem("Motivation is well-explained"));
    items.push_back(ReviewChecklistItem("Affected components are documented"));

    // Impact-specific items
    if (impact == ImpactLevel::CRITICAL || impact == ImpactLevel::HIGH) {
        items.push_back(ReviewChecklistItem("Rationale is comprehensive"));
        items.push_back(ReviewChecklistItem("Alternatives were considered"));
        items.push_back(ReviewChecklistItem("Decision rationale is provided"));
        items.push_back(ReviewChecklistItem("Ring impact analysis is complete"));
        items.push_back(ReviewChecklistItem("Backward compatibility is assessed"));
    }

    // Testing items
    items.push_back(ReviewChecklistItem("Change has test coverage"));
    items.push_back(ReviewChecklistItem("All existing tests pass"));

    // Critical changes require additional scrutiny
    if (impact == ImpactLevel::CRITICAL) {
        items.push_back(ReviewChecklistItem("Security implications analyzed"));
        items.push_back(ReviewChecklistItem("Consensus implications analyzed"));
        items.push_back(ReviewChecklistItem("Ring 7 immutability preserved (if applicable)"));
        items.push_back(ReviewChecklistItem("Ring 8a BC properties preserved"));
        items.push_back(ReviewChecklistItem("Ring 8b gating requirements met (if extension)"));
    }

    // Documentation items
    if (impact != ImpactLevel::LOW) {
        items.push_back(ReviewChecklistItem("Documentation updated"));
        items.push_back(ReviewChecklistItem("Release notes prepared"));
    }

    return items;
}

} // namespace test
} // namespace governance
} // namespace dinero
