// doctor_json_emitter.cpp - Schema-versioned JSON output
// Uses jsoncpp (already linked in dinerod) for structured, stable output.
#include "daemon/doctor/doctor_json_emitter.h"

#include <algorithm>
#include <fstream>
#include <json/json.h>

namespace dinero {
namespace doctor {

static Json::Value FixActionToJson(const FixAction& fix) {
    Json::Value j;
    j["id"] = fix.id;
    j["safe_to_apply"] = fix.safe_to_apply;
    j["risk"] = to_string(fix.risk);
    j["expected_downtime"] = fix.expected_downtime;

    Json::Value preconditions(Json::arrayValue);
    for (const auto& p : fix.preconditions) {
        preconditions.append(p);
    }
    j["preconditions"] = preconditions;

    Json::Value steps(Json::arrayValue);
    for (const auto& s : fix.steps) {
        steps.append(s);
    }
    j["steps"] = steps;

    if (!fix.rollback_notes.empty()) {
        j["rollback_notes"] = fix.rollback_notes;
    }

    return j;
}

static Json::Value CheckResultToJson(const DoctorCheckResult& result) {
    Json::Value j;
    j["id"] = result.id;
    j["status"] = to_string(result.status);
    j["message"] = result.message;
    j["duration_ms"] = result.duration_ms;
    j["started_at"] = result.started_at;
    j["finished_at"] = result.finished_at;

    // Evidence: sorted keys for deterministic output
    Json::Value evidence(Json::objectValue);
    std::vector<std::string> keys;
    keys.reserve(result.evidence.size());
    for (const auto& [k, v] : result.evidence) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& k : keys) {
        evidence[k] = result.evidence.at(k);
    }
    j["evidence"] = evidence;

    // Fix plans
    Json::Value fix_plan(Json::arrayValue);
    for (const auto& fix : result.fix_plan) {
        fix_plan.append(FixActionToJson(fix));
    }
    j["fix_plan"] = fix_plan;

    return j;
}

void DoctorJsonEmitter::Emit(std::ostream& out, const DoctorRunResult& run) {
    Json::Value root;
    root["schema_version"] = kSchemaVersion;
    root["node_version"] = run.node_version;
    root["network"] = run.network;
    root["timestamp"] = run.timestamp;
    root["mode"] = to_string(run.mode);
    root["exit_code"] = static_cast<int>(run.exit_code);
    root["total_duration_ms"] = run.total_duration_ms;

    Json::Value summary;
    summary["critical"] = run.summary.critical;
    summary["warnings"] = run.summary.warnings;
    summary["errors"] = run.summary.errors;
    summary["passed"] = run.summary.passed;
    summary["skipped"] = run.summary.skipped;
    root["summary"] = summary;

    Json::Value checks(Json::arrayValue);
    for (const auto& result : run.results) {
        checks.append(CheckResultToJson(result));
    }
    root["checks"] = checks;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    builder["emitUTF8"] = true;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &out);
    out << "\n";
}

bool DoctorJsonEmitter::EmitToFile(const std::string& path, const DoctorRunResult& run) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    Emit(file, run);
    return file.good();
}

} // namespace doctor
} // namespace dinero
