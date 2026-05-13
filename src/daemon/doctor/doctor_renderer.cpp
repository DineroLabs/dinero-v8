// doctor_renderer.cpp - Human-readable terminal output
#include "daemon/doctor/doctor_renderer.h"

#include <algorithm>
#include <iomanip>
#include <vector>

namespace dinero {
namespace doctor {

static const char* StatusTag(CheckStatus s) {
    switch (s) {
        case CheckStatus::PASS:  return "[PASS]";
        case CheckStatus::WARN:  return "[WARN]";
        case CheckStatus::CRIT:  return "[CRIT]";
        case CheckStatus::ERROR: return "[ERR ]";
        case CheckStatus::SKIP:  return "[SKIP]";
    }
    return "[????]";
}

void DoctorRenderer::RenderResults(std::ostream& out, const DoctorRunResult& run) {
    out << "dinerod doctor (" << to_string(run.mode) << " mode)\n";
    out << "Network: " << run.network << "  |  Version: " << run.node_version << "\n";
    out << std::string(60, '-') << "\n";

    for (const auto& result : run.results) {
        out << "  " << StatusTag(result.status)
            << "  " << result.id;

        if (result.duration_ms > 0) {
            out << "  (" << result.duration_ms << "ms)";
        }
        out << "\n";

        if (!result.message.empty() && result.status != CheckStatus::PASS) {
            out << "         " << result.message << "\n";
        }

        // Show evidence for non-pass results
        if (result.status != CheckStatus::PASS && result.status != CheckStatus::SKIP) {
            // Sort evidence keys for deterministic output
            std::vector<std::string> keys;
            keys.reserve(result.evidence.size());
            for (const auto& [k, v] : result.evidence) {
                keys.push_back(k);
            }
            std::sort(keys.begin(), keys.end());

            for (const auto& k : keys) {
                out << "           " << k << ": " << result.evidence.at(k) << "\n";
            }
        }

        // Show fix plans for non-pass results
        for (const auto& fix : result.fix_plan) {
            out << "         Fix: " << fix.id;
            if (fix.safe_to_apply) {
                out << " [safe-apply]";
            }
            out << " (risk: " << to_string(fix.risk) << ")\n";
            for (const auto& step : fix.steps) {
                out << "           -> " << step << "\n";
            }
        }
    }

    out << std::string(60, '-') << "\n";
    out << "Summary: "
        << run.summary.passed << " passed, "
        << run.summary.warnings << " warnings, "
        << run.summary.critical << " critical, "
        << run.summary.errors << " errors, "
        << run.summary.skipped << " skipped"
        << "  (" << run.total_duration_ms << "ms)\n";
    out << "Exit: " << to_string(run.exit_code) << " (" << static_cast<int>(run.exit_code) << ")\n";
}

void DoctorRenderer::RenderCheckList(std::ostream& out, const DoctorRegistry& registry) {
    out << "Available checks:\n\n";

    // Sort by ID for deterministic output
    std::vector<std::pair<std::string, const RegisteredCheck*>> sorted;
    for (const auto& [id, check] : registry.GetAll()) {
        sorted.emplace_back(id, &check);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [id, check] : sorted) {
        out << "  " << std::left << std::setw(36) << id
            << "  [" << to_string(check->metadata.mode) << "]"
            << "  " << check->metadata.description << "\n";
    }

    out << "\n" << sorted.size() << " checks registered.\n";
}

void DoctorRenderer::RenderExplain(std::ostream& out, const RegisteredCheck& check) {
    const auto& m = check.metadata;

    out << "Check: " << m.id << "\n";
    out << std::string(40, '-') << "\n";
    out << "Description:     " << m.description << "\n";
    out << "Mode:            " << to_string(m.mode) << "\n";
    out << "Default severity:" << to_string(m.severity_default) << "\n";
    out << "Fix risk:        " << to_string(m.risk) << "\n";
    out << "Timeout budget:  " << m.timeout_budget_ms << "ms\n";

    if (!m.dependencies.empty()) {
        out << "Dependencies:    ";
        for (size_t i = 0; i < m.dependencies.size(); ++i) {
            if (i > 0) out << ", ";
            out << m.dependencies[i];
        }
        out << "\n";
    }
}

} // namespace doctor
} // namespace dinero
