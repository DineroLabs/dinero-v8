// doctor_fixer.h - Safe fix execution engine for dinerod doctor
// Dispatches fix implementations by ID, enforces preconditions,
// and provides preview/apply flow with per-fix reporting.
#pragma once

#include "daemon/doctor/doctor_types.h"
#include <ostream>
#include <string>
#include <vector>

namespace dinero {
namespace doctor {

// Forward declaration
class DoctorContext;

// Result of applying a single fix
struct FixResult {
    std::string fix_id;
    std::string check_id;          // Which check produced this fix
    bool applied = false;
    bool success = false;
    std::string message;           // Human-readable outcome
};

// Summary of a fix-apply session
struct FixSession {
    std::vector<FixResult> results;
    std::vector<std::string> affected_check_ids;  // Checks to re-run
    int applied = 0;
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
};

class DoctorFixer {
public:
    // Collect all eligible fixes from a run result, filtered by config.
    // Returns fix actions paired with their source check ID.
    struct FixCandidate {
        FixAction action;
        std::string check_id;
    };
    static std::vector<FixCandidate> CollectEligibleFixes(
        const DoctorRunResult& run,
        const DoctorConfig& config);

    // Preview fixes to the user (dry-run display)
    static void PreviewFixes(std::ostream& out,
                              const std::vector<FixCandidate>& candidates);

    // Apply fixes. Returns session with per-fix results.
    static FixSession ApplyFixes(const std::vector<FixCandidate>& candidates,
                                  const DoctorContext& ctx);

    // Render fix session results
    static void RenderFixResults(std::ostream& out, const FixSession& session);
};

} // namespace doctor
} // namespace dinero
