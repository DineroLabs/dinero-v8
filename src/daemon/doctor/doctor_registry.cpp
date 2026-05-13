// doctor_registry.cpp - Check registry with deterministic ordering
#include "daemon/doctor/doctor_registry.h"

#include <algorithm>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace dinero {
namespace doctor {

static DoctorRegistry g_doctor_registry;

DoctorRegistry& GetDoctorRegistry() {
    return g_doctor_registry;
}

void DoctorRegistry::Register(DoctorCheckMetadata metadata, CheckFn fn) {
    std::string id = metadata.id;
    checks_.emplace(id, RegisteredCheck{std::move(metadata), std::move(fn)});
}

const RegisteredCheck* DoctorRegistry::Find(const std::string& id) const {
    auto it = checks_.find(id);
    if (it == checks_.end()) return nullptr;
    return &it->second;
}

std::vector<std::string> DoctorRegistry::TopologicalSort() const {
    // Kahn's algorithm with lexical tie-break via priority_queue (min-heap)
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;

    // Initialize in-degree for all checks
    for (const auto& [id, _] : checks_) {
        if (in_degree.find(id) == in_degree.end()) {
            in_degree[id] = 0;
        }
    }

    // Build dependency graph
    for (const auto& [id, check] : checks_) {
        for (const auto& dep : check.metadata.dependencies) {
            // Only count dependencies that are registered
            if (checks_.find(dep) != checks_.end()) {
                in_degree[id]++;
                dependents[dep].push_back(id);
            }
        }
    }

    // Min-heap for lexical tie-break (smallest ID first)
    std::priority_queue<std::string, std::vector<std::string>, std::greater<std::string>> ready;

    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) {
            ready.push(id);
        }
    }

    std::vector<std::string> order;
    order.reserve(checks_.size());

    while (!ready.empty()) {
        std::string current = ready.top();
        ready.pop();
        order.push_back(current);

        for (const auto& dependent : dependents[current]) {
            in_degree[dependent]--;
            if (in_degree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    // If order.size() < checks_.size(), there's a cycle.
    // In that case, append remaining checks in lexical order (they'll get ERROR status).
    if (order.size() < checks_.size()) {
        std::set<std::string> ordered_set(order.begin(), order.end());
        for (const auto& [id, _] : checks_) {
            if (ordered_set.find(id) == ordered_set.end()) {
                order.push_back(id);
            }
        }
    }

    return order;
}

std::vector<const RegisteredCheck*> DoctorRegistry::GetExecutionOrder(RunMode mode) const {
    auto sorted_ids = TopologicalSort();
    std::vector<const RegisteredCheck*> result;
    result.reserve(sorted_ids.size());

    for (const auto& id : sorted_ids) {
        auto it = checks_.find(id);
        if (it == checks_.end()) continue;

        const auto& check = it->second;
        // Filter by mode
        if (check.metadata.mode == CheckMode::BOTH) {
            result.push_back(&check);
        } else if (mode == RunMode::QUICK && check.metadata.mode == CheckMode::QUICK) {
            result.push_back(&check);
        } else if (mode == RunMode::DEEP && check.metadata.mode == CheckMode::DEEP) {
            result.push_back(&check);
        }
    }

    return result;
}

// Simple glob matching: supports '*' wildcard at end or in middle
static bool GlobMatch(const std::string& pattern, const std::string& text) {
    size_t star = pattern.find('*');
    if (star == std::string::npos) {
        return pattern == text;
    }
    // "storage.*" matches "storage.disk_space"
    std::string prefix = pattern.substr(0, star);
    std::string suffix = pattern.substr(star + 1);
    if (text.size() < prefix.size() + suffix.size()) return false;
    return text.compare(0, prefix.size(), prefix) == 0 &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<const RegisteredCheck*> DoctorRegistry::Filter(
    const std::vector<std::string>& patterns, RunMode mode) const {

    if (patterns.empty()) {
        return GetExecutionOrder(mode);
    }

    auto all = GetExecutionOrder(mode);
    std::vector<const RegisteredCheck*> filtered;

    for (const auto* check : all) {
        for (const auto& pat : patterns) {
            if (GlobMatch(pat, check->metadata.id)) {
                filtered.push_back(check);
                break;
            }
        }
    }

    return filtered;
}

// RegisterV1Checks is defined in doctor_checks_v1.cpp

} // namespace doctor
} // namespace dinero
