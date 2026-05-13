// doctor_runner.cpp - Deterministic check execution with timeout and dependencies
#include "daemon/doctor/doctor_runner.h"

#include <chrono>
#include <ctime>
#include <future>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>

namespace dinero {
namespace doctor {

// ISO 8601 timestamp
static std::string NowISO8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

bool DoctorRunner::DependenciesMet(const DoctorCheckMetadata& meta) const {
    for (const auto& dep : meta.dependencies) {
        if (passed_checks_.find(dep) == passed_checks_.end()) {
            return false;
        }
    }
    return true;
}

uint32_t DoctorRunner::EffectiveBudget(uint32_t check_budget_ms) const {
    if (overall_budget_ms_ == 0) {
        return check_budget_ms;  // No overall budget constraint
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - run_start_).count());

    if (elapsed_ms >= overall_budget_ms_) {
        return 0;  // Budget exhausted
    }

    uint32_t remaining = overall_budget_ms_ - elapsed_ms;
    return std::min(check_budget_ms, remaining);
}

DoctorCheckResult DoctorRunner::ExecuteCheck(const RegisteredCheck& check,
                                              const DoctorContext& ctx,
                                              uint32_t effective_budget_ms) {
    DoctorCheckResult result;
    result.id = check.metadata.id;
    result.started_at = NowISO8601();

    auto start = std::chrono::steady_clock::now();

    // Check dependencies
    if (!DependenciesMet(check.metadata)) {
        result.status = CheckStatus::SKIP;
        result.message = "Skipped: dependency not met";
        result.finished_at = NowISO8601();
        result.duration_ms = 0;
        return result;
    }

    // Check overall run budget (only applies when an overall budget is active)
    bool budget_constrained = (effective_budget_ms < check.metadata.timeout_budget_ms);
    if (budget_constrained && effective_budget_ms < kMinCheckBudgetMs) {
        result.status = CheckStatus::SKIP;
        result.message = "Skipped: insufficient run budget remaining";
        result.evidence["budget_remaining_ms"] = std::to_string(effective_budget_ms);
        result.finished_at = NowISO8601();
        result.duration_ms = 0;
        return result;
    }

    // Execute with hard timeout.
    // We use a promise/future with a detached thread so that the timeout
    // is truly hard — if the check hangs, we abandon it rather than
    // blocking on std::async's destructor.
    uint32_t budget_ms = effective_budget_ms;

    try {
        auto promise = std::make_shared<std::promise<DoctorCheckResult>>();
        auto future = promise->get_future();

        // Capture EVERYTHING by value — the detached thread can outlive the caller
        // on timeout, so &ctx would be a use-after-free. DoctorContext is small (strings + ints).
        auto fn = check.fn;
        auto ctx_copy = ctx;
        std::thread([promise, fn, ctx_copy]() {
            try {
                promise->set_value(fn(ctx_copy));
            } catch (const std::exception& e) {
                DoctorCheckResult err;
                err.status = CheckStatus::ERROR;
                err.message = std::string("Check threw exception: ") + e.what();
                promise->set_value(std::move(err));
            } catch (...) {
                DoctorCheckResult err;
                err.status = CheckStatus::ERROR;
                err.message = "Check threw unknown exception";
                promise->set_value(std::move(err));
            }
        }).detach();

        auto timeout = std::chrono::milliseconds(budget_ms);
        auto status = future.wait_for(timeout);

        if (status == std::future_status::timeout) {
            result.status = CheckStatus::ERROR;
            result.message = "Check timed out after " + std::to_string(budget_ms) + "ms";
            result.evidence["timeout_budget_ms"] = std::to_string(budget_ms);
        } else {
            auto started = result.started_at;
            result = future.get();
            // Preserve fields the runner owns
            result.id = check.metadata.id;
            result.started_at = started;
        }
    } catch (const std::exception& e) {
        result.status = CheckStatus::ERROR;
        result.message = std::string("Runner error: ") + e.what();
    } catch (...) {
        result.status = CheckStatus::ERROR;
        result.message = "Runner unknown error";
    }

    auto end = std::chrono::steady_clock::now();
    result.duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    result.finished_at = NowISO8601();

    return result;
}

DoctorRunResult DoctorRunner::Run(const DoctorContext& ctx,
                                   const DoctorRegistry& registry,
                                   const std::vector<std::string>& check_filter) {
    DoctorRunResult run;
    run.mode = ctx.Mode();
    run.network = ctx.Network();
    run.node_version = ctx.NodeVersion();
    run.timestamp = NowISO8601();

    passed_checks_.clear();

    auto start = std::chrono::steady_clock::now();
    run_start_ = start;

    // Set overall budget for deep mode
    overall_budget_ms_ = (ctx.Mode() == RunMode::DEEP) ? kDeepModeBudgetMs : 0;

    // Get filtered, ordered checks
    auto checks = registry.Filter(check_filter, ctx.Mode());

    for (const auto* check : checks) {
        uint32_t budget = EffectiveBudget(check->metadata.timeout_budget_ms);
        auto result = ExecuteCheck(*check, ctx, budget);

        // Track passes for dependency resolution
        if (result.status == CheckStatus::PASS) {
            passed_checks_.insert(result.id);
        }

        // Update summary
        switch (result.status) {
            case CheckStatus::PASS:  run.summary.passed++;   break;
            case CheckStatus::WARN:  run.summary.warnings++; break;
            case CheckStatus::CRIT:  run.summary.critical++; break;
            case CheckStatus::ERROR: run.summary.errors++;   break;
            case CheckStatus::SKIP:  run.summary.skipped++;  break;
        }

        run.results.push_back(std::move(result));
    }

    auto end = std::chrono::steady_clock::now();
    run.total_duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    // Determine exit code
    if (run.summary.errors > 0) {
        run.exit_code = ExitCode::INTERNAL_ERROR;
    } else if (run.summary.critical > 0) {
        run.exit_code = ExitCode::CRITICAL;
    } else if (run.summary.warnings > 0) {
        run.exit_code = ExitCode::WARNINGS;
    } else {
        run.exit_code = ExitCode::HEALTHY;
    }

    return run;
}

} // namespace doctor
} // namespace dinero
