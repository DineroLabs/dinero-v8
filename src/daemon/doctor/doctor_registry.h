// doctor_registry.h - Check registry with dependency-aware deterministic ordering
#pragma once

#include "daemon/doctor/doctor_types.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero {
namespace doctor {

// Forward declaration
class DoctorContext;

// Check function signature: receives read-only context, returns result
using CheckFn = std::function<DoctorCheckResult(const DoctorContext&)>;

struct RegisteredCheck {
    DoctorCheckMetadata metadata;
    CheckFn fn;
};

class DoctorRegistry {
public:
    // Register a check with its metadata and implementation
    void Register(DoctorCheckMetadata metadata, CheckFn fn);

    // Get all checks in deterministic execution order (topological + lexical)
    // Filters by mode. Returns ordered list.
    std::vector<const RegisteredCheck*> GetExecutionOrder(RunMode mode) const;

    // Get all registered checks (unordered, for --list-checks)
    const std::unordered_map<std::string, RegisteredCheck>& GetAll() const { return checks_; }

    // Lookup single check by ID (for --explain)
    const RegisteredCheck* Find(const std::string& id) const;

    // Filter checks by glob patterns (e.g. "storage.*", "db.tip*")
    std::vector<const RegisteredCheck*> Filter(
        const std::vector<std::string>& patterns, RunMode mode) const;

private:
    std::unordered_map<std::string, RegisteredCheck> checks_;

    // Topological sort with lexical tie-break for determinism
    std::vector<std::string> TopologicalSort() const;
};

// Global registry accessor (populated at startup)
DoctorRegistry& GetDoctorRegistry();

// Register all v1 checks into the registry (called once at startup)
void RegisterV1Checks(DoctorRegistry& registry);

} // namespace doctor
} // namespace dinero
