#pragma once

#include "consensus_params.h"
#include "subsidy_calculator.h"
#include "../framework/mining_trace.h"
#include "../framework/mining_types.h"
#include <vector>
#include <string>

// Ring 4 Phase 4c: Mining Correctness Oracle
// Purpose: Validate C1-C5 correctness properties against mining traces
// Rule: Oracle validates properties WITHOUT modifying simulator

namespace mining_test {

// ============================================================================
// Violation Structures
// ============================================================================

// MC1: Subsidy Correctness Violation
struct SubsidyViolation {
    uint64_t template_height;
    uint64_t claimed_subsidy;
    uint64_t expected_subsidy;
    std::string description;

    SubsidyViolation(uint64_t height, uint64_t claimed, uint64_t expected, const std::string& desc)
        : template_height(height), claimed_subsidy(claimed), expected_subsidy(expected), description(desc) {}
};

// MC2: Coinbase Structure Violation
struct CoinbaseViolation {
    uint64_t template_height;
    std::string violation_type;  // "missing_height", "missing_subsidy", etc.
    std::string description;

    CoinbaseViolation(uint64_t height, const std::string& type, const std::string& desc)
        : template_height(height), violation_type(type), description(desc) {}
};

// MC3: Template Validity Violation
struct TemplateViolation {
    uint64_t template_height;
    std::string invalidity_reason;
    std::string description;

    TemplateViolation(uint64_t height, const std::string& reason, const std::string& desc)
        : template_height(height), invalidity_reason(reason), description(desc) {}
};

// MC4: Transaction Context Violation
struct ContextViolation {
    uint64_t template_height;
    uint64_t tx_hash;  // Placeholder
    std::string context_error;  // "tx_count_exceeded", etc.
    std::string description;

    ContextViolation(uint64_t height, uint64_t hash, const std::string& error, const std::string& desc)
        : template_height(height), tx_hash(hash), context_error(error), description(desc) {}
};

// MC5: Consensus Bypass Violation
struct BypassViolation {
    uint64_t template_height;
    std::string bypassed_check;  // "validation_skipped", etc.
    std::string description;

    BypassViolation(uint64_t height, const std::string& check, const std::string& desc)
        : template_height(height), bypassed_check(check), description(desc) {}
};

// ============================================================================
// Correctness Report (Aggregated)
// ============================================================================

struct CorrectnessReport {
    std::vector<SubsidyViolation> subsidy_violations;
    std::vector<CoinbaseViolation> coinbase_violations;
    std::vector<TemplateViolation> template_violations;
    std::vector<ContextViolation> context_violations;
    std::vector<BypassViolation> bypass_violations;

    // Check if all properties satisfied
    bool allPropertiesSatisfied() const {
        return subsidy_violations.empty() &&
               coinbase_violations.empty() &&
               template_violations.empty() &&
               context_violations.empty() &&
               bypass_violations.empty();
    }

    // Get total violation count
    size_t totalViolations() const {
        return subsidy_violations.size() +
               coinbase_violations.size() +
               template_violations.size() +
               context_violations.size() +
               bypass_violations.size();
    }
};

// ============================================================================
// MiningCorrectnessOracle - Property Validator
// ============================================================================

class MiningCorrectnessOracle {
public:
    // Construct with consensus parameters
    explicit MiningCorrectnessOracle(const ConsensusParams& params);

    // MC1: Subsidy Correctness (FULL IMPLEMENTATION)
    // Check that all templates claim correct subsidy for their height
    std::vector<SubsidyViolation> checkSubsidyCorrectness(const MiningTrace& trace);

    // MC2: Coinbase Structure (PLACEHOLDER - Phase 4c)
    // Check that coinbase metadata is present and consistent
    // Full coinbase structure validation deferred to Phase 4h
    std::vector<CoinbaseViolation> checkCoinbaseStructure(const MiningTrace& trace);

    // MC3: Template Validity (PLACEHOLDER - Phase 4c)
    // Check that template metadata is internally consistent
    // Full block validation deferred to Phase 4h
    std::vector<TemplateViolation> checkTemplateValidity(const MiningTrace& trace);

    // MC4: Transaction Context (PLACEHOLDER - Phase 4c)
    // Check that transaction counts are reasonable
    // Full UTXO validation deferred to Phase 4h
    std::vector<ContextViolation> checkTransactionContext(const MiningTrace& trace);

    // MC5: No Consensus Bypass (PLACEHOLDER - Phase 4c)
    // Check that validation happens consistently
    // Full bypass detection deferred to Phase 4h
    std::vector<BypassViolation> checkNoConsensusBypass(const MiningTrace& trace);

    // Convenience: Check all properties at once
    CorrectnessReport checkAllProperties(const MiningTrace& trace);

    // Get consensus params
    const ConsensusParams& getParams() const { return params_; }

    // Get subsidy calculator
    const ConsensusSubsidyCalculator& getSubsidyCalculator() const { return subsidy_calc_; }

private:
    // Consensus parameters
    ConsensusParams params_;

    // Subsidy calculator (for MC1)
    ConsensusSubsidyCalculator subsidy_calc_;
};

}  // namespace mining_test
