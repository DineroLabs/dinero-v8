// doctor_runner.h - Deterministic check execution engine
// Handles timeout enforcement, dependency tracking, and result collection.
#pragma once

#include "daemon/doctor/doctor_types.h"
#include "daemon/doctor/doctor_registry.h"
#include "daemon/doctor/doctor_context.h"
#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

namespace dinero {
namespace doctor {

// Overall run budget for deep mode (10 minutes)
inline constexpr uint32_t kDeepModeBudgetMs = 600000;
// Minimum per-check budget before skipping (1 second)
inline constexpr uint32_t kMinCheckBudgetMs = 1000;

class DoctorRunner {
public:
    // Run all applicable checks against the given context.
    // check_filter: glob patterns (empty = all). mode from context.
    DoctorRunResult Run(const DoctorContext& ctx,
                        const DoctorRegistry& registry,
                        const std::vector<std::string>& check_filter = {});

private:
    // Execute a single check with timeout enforcement
    DoctorCheckResult ExecuteCheck(const RegisteredCheck& check,
                                   const DoctorContext& ctx,
                                   uint32_t effective_budget_ms);

    // Track which checks have passed (for dependency resolution)
    std::unordered_set<std::string> passed_checks_;

    // Check if all dependencies of a check have passed
    bool DependenciesMet(const DoctorCheckMetadata& meta) const;

    // Run start time for budget tracking
    std::chrono::steady_clock::time_point run_start_;
    uint32_t overall_budget_ms_ = 0;  // 0 = no overall budget

    // Compute effective per-check budget (may be reduced by overall budget)
    uint32_t EffectiveBudget(uint32_t check_budget_ms) const;
};

} // namespace doctor
} // namespace dinero
