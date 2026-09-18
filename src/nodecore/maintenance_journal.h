#pragma once

#include <json/json.h>
#include <string>

namespace dinero::nodecore {

// Serialized by NodeCoreState::mtx. The only registered plan is read-only,
// bounded native qualification; this class implements no file deletion.
class MaintenanceJournal {
public:
    static constexpr const char* kInspection = "inspect-unchanged-v1";
    static int StartupGate(const std::string& target, Json::Value* info = nullptr);
    static bool SameTarget(const std::string& a, const std::string& b);
    static std::string RandomId();

    int Prepare(const std::string& target);
    int Resume(const std::string& target, const std::string& operation);
    int CompleteUnchanged();
    int RetainUncertainty();
    std::string Operation() const { return record_["operation_id"].asString(); }

private:
    Json::Value record_;
};

} // namespace dinero::nodecore
