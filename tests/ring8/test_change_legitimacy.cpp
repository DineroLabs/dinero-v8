/**
 * Ring 8 Phase 8c: Change Legitimacy & Audit Discipline Tests
 *
 * These tests verify that protocol changes are properly documented, justified, and auditable.
 *
 * Properties tested:
 * - CL1: Change Documentation - All changes must be documented
 * - CL2: Rationale Traceability - Changes have clear rationale
 * - CL3: Audit Trail - Complete audit trail of all changes
 *
 * CRITICAL: These tests ensure governance discipline and change accountability.
 */

#include <gtest/gtest.h>
#include "framework/change_types.h"
#include "framework/change_audit_log.h"
#include "framework/review_checklist_manager.h"
#include <memory>

using namespace dinero::governance::test;

class ChangeLegitimacyTest : public ::testing::Test {
protected:
    void SetUp() override {
        audit_log = std::make_unique<ChangeAuditLog>();
        review_mgr = std::make_unique<ReviewChecklistManager>();
    }

    std::unique_ptr<ChangeAuditLog> audit_log;
    std::unique_ptr<ReviewChecklistManager> review_mgr;
};

// ============================================================================
// CL1: Change Documentation
// ============================================================================

TEST_F(ChangeLegitimacyTest, CL1_AllChangesDocumented) {
    // All changes must have complete documentation

    ChangeProposal proposal("CIP-001", "Add covenant extensions");
    proposal.description = "Implement covenant extensions for Dinero";
    proposal.category = ChangeCategory::EXTENSION_ADDITION;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {"src/script/covenant.cpp", "include/script/covenant.h"};

    audit_log->recordProposal(proposal);

    // Check CL1: All changes documented
    auto violations = audit_log->checkAllChangesDocumented();

    EXPECT_TRUE(violations.empty())
        << "CL1: All changes must have complete documentation";
}

TEST_F(ChangeLegitimacyTest, CL1_MissingTitleViolation) {
    // Change without title violates CL1

    ChangeProposal proposal("CIP-002", "");  // Missing title
    proposal.description = "Some change";
    proposal.affected_components = {"src/consensus.cpp"};

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkAllChangesDocumented();

    EXPECT_FALSE(violations.empty())
        << "CL1 violation: Missing title should be detected";
    EXPECT_EQ(violations[0].property_name, "CL1");
}

TEST_F(ChangeLegitimacyTest, CL1_MissingDescriptionViolation) {
    // Change without description violates CL1

    ChangeProposal proposal("CIP-003", "Some change");
    proposal.description = "";  // Missing description
    proposal.affected_components = {"src/consensus.cpp"};

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkAllChangesDocumented();

    EXPECT_FALSE(violations.empty())
        << "CL1 violation: Missing description should be detected";
}

TEST_F(ChangeLegitimacyTest, CL1_MissingAffectedComponentsViolation) {
    // Change without affected components violates CL1

    ChangeProposal proposal("CIP-004", "Some change");
    proposal.description = "Description here";
    // Missing affected_components

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkAllChangesDocumented();

    EXPECT_FALSE(violations.empty())
        << "CL1 violation: Missing affected components should be detected";
}

TEST_F(ChangeLegitimacyTest, CL1_CompleteDocumentation) {
    // Well-documented change passes CL1

    ChangeProposal proposal("CIP-005", "Optimize block validation");
    proposal.description = "Improve block validation performance by 50%";
    proposal.category = ChangeCategory::PERFORMANCE;
    proposal.impact = ImpactLevel::MEDIUM;
    proposal.affected_components = {"src/validation.cpp", "tests/validation_tests.cpp"};
    proposal.motivation = "Current validation is too slow";
    proposal.rationale = "Use parallel validation for independent checks";

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkAllChangesDocumented();

    EXPECT_TRUE(violations.empty())
        << "Complete documentation should pass CL1";
}

// ============================================================================
// CL2: Rationale Traceability
// ============================================================================

TEST_F(ChangeLegitimacyTest, CL2_RationaleTraceability) {
    // Changes must have clear rationale

    ChangeProposal proposal("CIP-006", "Add taproot support");
    proposal.description = "Implement taproot for privacy";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {"src/script/taproot.cpp"};
    proposal.motivation = "Users need better privacy";
    proposal.rationale = "Taproot provides script privacy via MAST";
    proposal.alternatives = {"Schnorr only", "MimbleWimble"};
    proposal.decision_rationale = "Taproot is Bitcoin-compatible and well-tested";

    audit_log->recordProposal(proposal);

    // Check CL2: Rationale traceability
    auto violations = audit_log->checkRationaleTraceability();

    EXPECT_TRUE(violations.empty())
        << "CL2: Complete rationale should pass";
}

TEST_F(ChangeLegitimacyTest, CL2_MissingMotivationViolation) {
    // Change without motivation violates CL2

    ChangeProposal proposal("CIP-007", "Some consensus change");
    proposal.description = "Description here";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {"src/consensus.cpp"};
    // Missing motivation

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkRationaleTraceability();

    EXPECT_FALSE(violations.empty())
        << "CL2 violation: Missing motivation should be detected";
}

TEST_F(ChangeLegitimacyTest, CL2_MissingAlternativesForHighImpact) {
    // High impact change without alternatives analysis violates CL2

    ChangeProposal proposal("CIP-008", "Critical consensus change");
    proposal.description = "Description here";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::CRITICAL;
    proposal.affected_components = {"src/consensus.cpp"};
    proposal.motivation = "Motivation here";
    proposal.rationale = "Rationale here";
    // Missing alternatives and decision_rationale for HIGH/CRITICAL

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkRationaleTraceability();

    EXPECT_FALSE(violations.empty())
        << "CL2 violation: High impact changes require alternatives analysis";
}

TEST_F(ChangeLegitimacyTest, CL2_LowImpactNoAlternativesRequired) {
    // Low impact changes don't require alternatives analysis

    ChangeProposal proposal("CIP-009", "Documentation fix");
    proposal.description = "Fix typo in docs";
    proposal.category = ChangeCategory::DOCUMENTATION;
    proposal.impact = ImpactLevel::LOW;
    proposal.affected_components = {"docs/README.md"};
    proposal.motivation = "Documentation has typo";
    proposal.rationale = "Fix the typo";
    // No alternatives required for LOW impact

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkRationaleTraceability();

    EXPECT_TRUE(violations.empty())
        << "CL2: Low impact changes don't require alternatives";
}

// ============================================================================
// CL3: Audit Trail
// ============================================================================

TEST_F(ChangeLegitimacyTest, CL3_CompleteAuditTrail) {
    // Changes must have complete audit trail

    ChangeProposal proposal("CIP-010", "Security fix");
    proposal.description = "Fix integer overflow vulnerability";
    proposal.category = ChangeCategory::SECURITY_FIX;
    proposal.impact = ImpactLevel::CRITICAL;
    proposal.affected_components = {"src/validation.cpp"};
    proposal.status = ChangeStatus::PROPOSED;

    audit_log->recordProposal(proposal);

    // Transition through states
    audit_log->updateProposalStatus("CIP-010", ChangeStatus::REVIEWED);
    audit_log->updateProposalStatus("CIP-010", ChangeStatus::IMPLEMENTED);
    audit_log->updateProposalStatus("CIP-010", ChangeStatus::TESTED);
    audit_log->updateProposalStatus("CIP-010", ChangeStatus::MERGED);

    // Get audit trail
    auto trail = audit_log->getAuditTrail("CIP-010");

    EXPECT_GE(trail.size(), 5)  // PROPOSED + 4 transitions
        << "CL3: Complete audit trail should have all status transitions";
}

TEST_F(ChangeLegitimacyTest, CL3_MissingAuditTrailViolation) {
    // Merged change without complete audit trail violates CL3

    ChangeProposal proposal("CIP-011", "Some change");
    proposal.description = "Description";
    proposal.category = ChangeCategory::BUG_FIX;
    proposal.impact = ImpactLevel::MEDIUM;
    proposal.affected_components = {"src/bug.cpp"};
    proposal.status = ChangeStatus::MERGED;  // Jumped to MERGED without transitions

    audit_log->recordProposal(proposal);

    auto violations = audit_log->checkAuditTrailComplete();

    EXPECT_FALSE(violations.empty())
        << "CL3 violation: Merged change must have complete audit trail";
}

TEST_F(ChangeLegitimacyTest, CL3_StatusTransitions) {
    // Audit trail captures all status transitions

    ChangeProposal proposal("CIP-012", "Feature addition");
    proposal.description = "Add new feature";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {"src/feature.cpp"};

    audit_log->recordProposal(proposal);

    // Valid transition: PROPOSED → REVIEWED
    bool success = audit_log->updateProposalStatus("CIP-012", ChangeStatus::REVIEWED);
    EXPECT_TRUE(success) << "Valid status transition should succeed";

    // Invalid transition: REVIEWED → MERGED (skipping IMPLEMENTED, TESTED)
    success = audit_log->updateProposalStatus("CIP-012", ChangeStatus::MERGED);
    EXPECT_FALSE(success) << "Invalid status transition should fail";
}

TEST_F(ChangeLegitimacyTest, CL3_AuditTrailImmutability) {
    // Audit trail should grow monotonically (no deletions)

    ChangeProposal proposal("CIP-013", "Some change");
    proposal.description = "Description";
    proposal.category = ChangeCategory::REFACTORING;
    proposal.impact = ImpactLevel::LOW;
    proposal.affected_components = {"src/refactor.cpp"};

    audit_log->recordProposal(proposal);

    size_t initial_size = audit_log->getAllAuditEntries().size();

    audit_log->updateProposalStatus("CIP-013", ChangeStatus::REVIEWED);

    size_t after_size = audit_log->getAllAuditEntries().size();

    EXPECT_GT(after_size, initial_size)
        << "CL3: Audit trail should grow with each event";
}

// ============================================================================
// Review Checklist Tests
// ============================================================================

TEST_F(ChangeLegitimacyTest, ReviewChecklist_StandardItems) {
    // Review checklists should have standard items based on impact level

    ChangeProposal proposal("CIP-014", "Critical change");
    proposal.description = "Critical consensus change";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::CRITICAL;
    proposal.affected_components = {"src/consensus.cpp"};

    auto checklist = review_mgr->createStandardChecklist(proposal, "reviewer1");

    EXPECT_GT(checklist.items.size(), 0)
        << "Review checklist should have items";

    // Critical changes should have more items
    EXPECT_GT(checklist.items.size(), 10)
        << "Critical changes should have comprehensive checklist";
}

TEST_F(ChangeLegitimacyTest, ReviewChecklist_Approval) {
    // Change approval requires satisfied checklist

    ChangeProposal proposal("CIP-015", "Feature change");
    proposal.description = "New feature";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::MEDIUM;
    proposal.affected_components = {"src/feature.cpp"};

    audit_log->recordProposal(proposal);

    auto checklist = review_mgr->createStandardChecklist(proposal, "reviewer1");

    // Satisfy all items
    for (auto& item : checklist.items) {
        item.satisfied = true;
        item.evidence = "Test coverage verified";
    }

    checklist.approved = true;
    review_mgr->addReview(checklist);

    EXPECT_TRUE(review_mgr->isChangeApproved("CIP-015"))
        << "Change with satisfied checklist should be approved";
}

TEST_F(ChangeLegitimacyTest, ReviewChecklist_IncompleteDeniesApproval) {
    // Incomplete checklist should prevent approval

    ChangeProposal proposal("CIP-016", "Feature change");
    proposal.description = "New feature";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {"src/feature.cpp"};

    audit_log->recordProposal(proposal);

    auto checklist = review_mgr->createStandardChecklist(proposal, "reviewer1");

    // Leave some items unsatisfied
    checklist.items[0].satisfied = true;
    // Other items remain unsatisfied

    checklist.approved = false;  // Can't approve with incomplete checklist
    review_mgr->addReview(checklist);

    EXPECT_FALSE(review_mgr->isChangeApproved("CIP-016"))
        << "Change with incomplete checklist should not be approved";
}

TEST_F(ChangeLegitimacyTest, IntegrationTest_CompleteChangeLifecycle) {
    // Integration test: Complete change lifecycle with documentation, rationale, audit trail

    // Step 1: Propose change with complete documentation
    ChangeProposal proposal("CIP-100", "Add schnorr signatures");
    proposal.description = "Implement schnorr signature support for batch verification";
    proposal.category = ChangeCategory::CONSENSUS_RULE;
    proposal.impact = ImpactLevel::HIGH;
    proposal.affected_components = {
        "src/crypto/schnorr.cpp",
        "src/script/interpreter.cpp",
        "tests/schnorr_tests.cpp"
    };
    proposal.ring_impact = {"Ring 7", "Ring 8a"};
    proposal.motivation = "Enable batch signature verification for scalability";
    proposal.rationale = "Schnorr enables linear signature aggregation";
    proposal.alternatives = {"ECDSA only", "BLS signatures"};
    proposal.decision_rationale = "Schnorr is Bitcoin-compatible and well-tested";
    proposal.backward_compatible = false;  // Requires soft fork

    audit_log->recordProposal(proposal);

    // Step 2: Review with checklist
    auto checklist = review_mgr->createStandardChecklist(proposal, "maintainer1");
    for (auto& item : checklist.items) {
        item.satisfied = true;
        item.evidence = "Verified";
    }
    checklist.approved = true;
    review_mgr->addReview(checklist);

    // Step 3: Progress through lifecycle
    audit_log->updateProposalStatus("CIP-100", ChangeStatus::REVIEWED);
    audit_log->updateProposalStatus("CIP-100", ChangeStatus::IMPLEMENTED);
    audit_log->updateProposalStatus("CIP-100", ChangeStatus::TESTED);
    audit_log->updateProposalStatus("CIP-100", ChangeStatus::MERGED);

    // Verify CL1: Change is documented
    auto cl1_violations = audit_log->checkAllChangesDocumented();
    EXPECT_TRUE(cl1_violations.empty()) << "CL1: Change should be documented";

    // Verify CL2: Rationale is traceable
    auto cl2_violations = audit_log->checkRationaleTraceability();
    EXPECT_TRUE(cl2_violations.empty()) << "CL2: Rationale should be traceable";

    // Verify CL3: Audit trail is complete
    auto cl3_violations = audit_log->checkAuditTrailComplete();
    EXPECT_TRUE(cl3_violations.empty()) << "CL3: Audit trail should be complete";

    // Verify review approval
    EXPECT_TRUE(review_mgr->isChangeApproved("CIP-100"))
        << "Change should be approved";

    // Verify audit trail
    auto trail = audit_log->getAuditTrail("CIP-100");
    EXPECT_GE(trail.size(), 5) << "Should have complete audit trail";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
