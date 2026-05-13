#include "mining_correctness_oracle.h"
#include <sstream>

// Ring 4 Phase 4c: Mining Correctness Oracle Implementation
// Rule: Oracle validates properties, simulator remains dumb

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MiningCorrectnessOracle::MiningCorrectnessOracle(const ConsensusParams& params)
    : params_(params), subsidy_calc_(params) {
}

// ============================================================================
// MC1: Subsidy Correctness (FULL IMPLEMENTATION)
// ============================================================================

std::vector<SubsidyViolation> MiningCorrectnessOracle::checkSubsidyCorrectness(const MiningTrace& trace) {
    std::vector<SubsidyViolation> violations;

    // Check all TEMPLATE_CREATED events
    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Extract template metadata
            if (!event.template_height.has_value()) {
                // Missing height - cannot validate subsidy
                continue;
            }

            if (!event.subsidy_claimed.has_value()) {
                // Missing subsidy claim - cannot validate
                continue;
            }

            uint32_t height = *event.template_height;
            uint64_t claimed = *event.subsidy_claimed;

            // Calculate expected subsidy using Ring 1 consensus logic
            uint64_t expected = subsidy_calc_.getBlockSubsidy(height);

            // Check if claimed matches expected
            if (claimed != expected) {
                std::ostringstream desc;
                desc << "Template at height " << height
                     << " claims subsidy " << claimed
                     << " but consensus requires " << expected;

                violations.emplace_back(height, claimed, expected, desc.str());
            }
        }
    }

    return violations;
}

// ============================================================================
// MC2: Coinbase Structure (PLACEHOLDER)
// ============================================================================

std::vector<CoinbaseViolation> MiningCorrectnessOracle::checkCoinbaseStructure(const MiningTrace& trace) {
    std::vector<CoinbaseViolation> violations;

    // Phase 4c: Placeholder validation (metadata checks only)
    // Full coinbase structure validation deferred to Phase 4h

    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Check that template has required metadata
            if (!event.template_height.has_value()) {
                violations.emplace_back(
                    0,  // Unknown height
                    "missing_height",
                    "Template created without height metadata"
                );
            }

            if (!event.subsidy_claimed.has_value()) {
                uint32_t height = event.template_height.value_or(0);
                violations.emplace_back(
                    height,
                    "missing_subsidy",
                    "Template created without subsidy metadata"
                );
            }
        }
    }

    return violations;
}

// ============================================================================
// MC3: Template Validity (PLACEHOLDER)
// ============================================================================

std::vector<TemplateViolation> MiningCorrectnessOracle::checkTemplateValidity(const MiningTrace& trace) {
    std::vector<TemplateViolation> violations;

    // Phase 4c: Placeholder validation (consistency checks only)
    // Full block validation deferred to Phase 4h

    // For Phase 4c, we do very basic checks
    // Most validation happens in Phase 4h with real BlockAssembler

    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Check subsidy is non-negative when it shouldn't be zero
            if (event.subsidy_claimed.has_value() && event.template_height.has_value()) {
                uint32_t template_height = *event.template_height;
                uint64_t subsidy = *event.subsidy_claimed;

                // Subsidy should only be zero after 64 halvings
                // This is a very conservative check
                if (subsidy == 0 && template_height < 64 * params_.halving_interval) {
                    violations.emplace_back(
                        template_height,
                        "zero_subsidy_early",
                        "Subsidy is zero before 64 halvings"
                    );
                }
            }
        }
    }

    return violations;
}

// ============================================================================
// MC4: Transaction Context (PLACEHOLDER)
// ============================================================================

std::vector<ContextViolation> MiningCorrectnessOracle::checkTransactionContext(const MiningTrace& trace) {
    std::vector<ContextViolation> violations;

    // Phase 4c: Placeholder validation (count checks only)
    // Full UTXO validation deferred to Phase 4h

    // Check state snapshots for transaction count
    for (const auto& state : trace.snapshots) {
        if (state.template_tx_count.has_value()) {
            uint32_t tx_count = *state.template_tx_count;
            uint32_t height = state.template_height.value_or(0);

            // Check transaction count doesn't exceed block limit
            if (tx_count > params_.max_block_txs) {
                std::ostringstream desc;
                desc << "Template has " << tx_count
                     << " transactions, exceeds limit " << params_.max_block_txs;

                violations.emplace_back(
                    height,
                    0,  // Unknown tx hash
                    "tx_count_exceeded",
                    desc.str()
                );
            }

            // Template should have at least 1 tx (coinbase)
            // But allow 0 if template not fully assembled yet
            // This is a very basic check; full validation in Phase 4h
        }
    }

    return violations;
}

// ============================================================================
// MC5: No Consensus Bypass (PLACEHOLDER)
// ============================================================================

std::vector<BypassViolation> MiningCorrectnessOracle::checkNoConsensusBypass(const MiningTrace& trace) {
    std::vector<BypassViolation> violations;

    // Phase 4c: Placeholder validation (implicit checks only)
    // Full bypass detection deferred to Phase 4h

    // Track if system has crashed and restarted
    bool has_crashed = false;
    bool created_template_after_crash_without_restart = false;

    for (const auto& event : trace.events) {
        // Detect crash
        if (event.type == MiningEventType::ERROR_OCCURRED) {
            // Check description to see if it's a crash
            if (event.description.find("crashed") != std::string::npos) {
                has_crashed = true;
            }
        }

        // Detect restart
        if (event.type == MiningEventType::ERROR_OCCURRED) {
            if (event.description.find("restarted") != std::string::npos) {
                has_crashed = false;
                created_template_after_crash_without_restart = false;
            }
        }

        // Check if template created while crashed (should not happen)
        if (event.type == MiningEventType::TEMPLATE_CREATED && has_crashed) {
            uint32_t height = event.template_height.value_or(0);
            violations.emplace_back(
                height,
                "template_while_crashed",
                "Template created while system is crashed (validation bypassed)"
            );
            created_template_after_crash_without_restart = true;
        }
    }

    return violations;
}

// ============================================================================
// Check All Properties
// ============================================================================

CorrectnessReport MiningCorrectnessOracle::checkAllProperties(const MiningTrace& trace) {
    CorrectnessReport report;

    report.subsidy_violations = checkSubsidyCorrectness(trace);
    report.coinbase_violations = checkCoinbaseStructure(trace);
    report.template_violations = checkTemplateValidity(trace);
    report.context_violations = checkTransactionContext(trace);
    report.bypass_violations = checkNoConsensusBypass(trace);

    return report;
}

}  // namespace mining_test
