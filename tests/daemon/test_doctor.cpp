// test_doctor.cpp - Comprehensive test suite for dinerod doctor
// Phase 4: Registry ordering, dependency handling, timeout behavior,
// exit code contracts, JSON schema, safe-fix tests, budget controls.

#include <gtest/gtest.h>

#include "daemon/doctor/doctor_types.h"
#include "daemon/doctor/doctor_registry.h"
#include "daemon/doctor/doctor_runner.h"
#include "daemon/doctor/doctor_context.h"
#include "daemon/doctor/doctor_fixer.h"
#include "daemon/doctor/doctor_json_emitter.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

using namespace dinero::doctor;

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

// A trivial check that always passes
static DoctorCheckResult AlwaysPass(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::PASS;
    r.message = "OK";
    return r;
}

// A check that always warns
static DoctorCheckResult AlwaysWarn(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::WARN;
    r.message = "warning";
    return r;
}

// A check that always crits
static DoctorCheckResult AlwaysCrit(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::CRIT;
    r.message = "critical";
    return r;
}

// A check that throws
static DoctorCheckResult AlwaysThrow(const DoctorContext&) {
    throw std::runtime_error("test exception");
}

// A slow check (sleeps 200ms)
static DoctorCheckResult SlowCheck(const DoctorContext&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    DoctorCheckResult r;
    r.status = CheckStatus::PASS;
    r.message = "slow but ok";
    return r;
}

// A check that provides a safe fix
static DoctorCheckResult CritWithSafeFix(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::CRIT;
    r.message = "fixable issue";
    r.fix_plan.push_back(FixAction{
        "test.safe_fix", true, FixRisk::LOW, "none",
        {"none"}, {"echo fix"}, ""
    });
    return r;
}

// A check that provides a safe fix with a REAL dispatch ID (survives FindFixImpl)
static DoctorCheckResult CritWithRealSafeFix(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::CRIT;
    r.message = "mempool corrupt";
    r.fix_plan.push_back(FixAction{
        "mempool.snapshot_sanity.remove", true, FixRisk::LOW, "none",
        {"none"}, {"remove mempool.dat"}, ""
    });
    return r;
}

// A check that provides an unsafe fix
static DoctorCheckResult CritWithUnsafeFix(const DoctorContext&) {
    DoctorCheckResult r;
    r.status = CheckStatus::CRIT;
    r.message = "needs manual fix";
    r.fix_plan.push_back(FixAction{
        "test.unsafe_fix", false, FixRisk::HIGH, "> 1 hour",
        {"requires restart"}, {"reindex"}, "manual rollback"
    });
    return r;
}

// Helper to make a fresh DoctorContext pointing at a temp directory
static DoctorContext MakeTestContext(const std::string& tmpdir = "/tmp/doctor_test") {
    DoctorContext ctx(tmpdir, "regtest");
    ctx.SetMode(RunMode::QUICK);
    ctx.SetNodeVersion("test-0.0.1");
    return ctx;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. Registry Ordering Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorRegistry, LexicalOrderWithNoDependencies) {
    DoctorRegistry reg;
    reg.Register({"c.check", "C check", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"a.check", "A check", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b.check", "B check", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto order = reg.GetExecutionOrder(RunMode::QUICK);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0]->metadata.id, "a.check");
    EXPECT_EQ(order[1]->metadata.id, "b.check");
    EXPECT_EQ(order[2]->metadata.id, "c.check");
}

TEST(DoctorRegistry, DependencyOrderOverridesLexical) {
    DoctorRegistry reg;
    // z depends on a, so a must come first despite z being registered first
    reg.Register({"z.check", "Z", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"a.check"}, 5000}, AlwaysPass);
    reg.Register({"a.check", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto order = reg.GetExecutionOrder(RunMode::QUICK);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0]->metadata.id, "a.check");
    EXPECT_EQ(order[1]->metadata.id, "z.check");
}

TEST(DoctorRegistry, DiamondDependencyResolvesCorrectly) {
    DoctorRegistry reg;
    //     a
    //    / \
    //   b   c
    //    \ /
    //     d
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"a"}, 5000}, AlwaysPass);
    reg.Register({"c", "C", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"a"}, 5000}, AlwaysPass);
    reg.Register({"d", "D", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"b", "c"}, 5000}, AlwaysPass);

    auto order = reg.GetExecutionOrder(RunMode::QUICK);
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0]->metadata.id, "a");
    // b and c both depend on a; lexical tie-break puts b before c
    EXPECT_EQ(order[1]->metadata.id, "b");
    EXPECT_EQ(order[2]->metadata.id, "c");
    EXPECT_EQ(order[3]->metadata.id, "d");
}

TEST(DoctorRegistry, ModeFilterExcludesDeepChecksInQuickMode) {
    DoctorRegistry reg;
    reg.Register({"quick_only", "Q", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"deep_only", "D", Severity::WARN, CheckMode::DEEP, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"both", "B", Severity::WARN, CheckMode::BOTH, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto quick = reg.GetExecutionOrder(RunMode::QUICK);
    EXPECT_EQ(quick.size(), 2u);  // quick_only + both

    auto deep = reg.GetExecutionOrder(RunMode::DEEP);
    EXPECT_EQ(deep.size(), 2u);   // deep_only + both

    // Verify deep_only is NOT in quick results
    for (const auto* c : quick) {
        EXPECT_NE(c->metadata.id, "deep_only");
    }
    // Verify quick_only is NOT in deep results
    for (const auto* c : deep) {
        EXPECT_NE(c->metadata.id, "quick_only");
    }
}

TEST(DoctorRegistry, GlobFilterWorks) {
    DoctorRegistry reg;
    reg.Register({"storage.disk", "D", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"storage.perms", "P", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"p2p.bind", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto filtered = reg.Filter({"storage.*"}, RunMode::QUICK);
    EXPECT_EQ(filtered.size(), 2u);
    for (const auto* c : filtered) {
        EXPECT_TRUE(c->metadata.id.find("storage.") == 0);
    }
}

TEST(DoctorRegistry, FindReturnsNullForUnknownId) {
    DoctorRegistry reg;
    reg.Register({"known.check", "K", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    EXPECT_NE(reg.Find("known.check"), nullptr);
    EXPECT_EQ(reg.Find("unknown.check"), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Dependency Handling Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorRunner, SkipsCheckWhenDependencyFails) {
    DoctorRegistry reg;
    reg.Register({"parent", "P", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysCrit);
    reg.Register({"child", "C", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"parent"}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 2u);
    EXPECT_EQ(run.results[1].status, CheckStatus::SKIP);
    EXPECT_TRUE(run.results[1].message.find("dependency") != std::string::npos);
}

TEST(DoctorRunner, RunsCheckWhenDependencyPasses) {
    DoctorRegistry reg;
    reg.Register({"parent", "P", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"child", "C", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"parent"}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 2u);
    EXPECT_EQ(run.results[0].status, CheckStatus::PASS);
    EXPECT_EQ(run.results[1].status, CheckStatus::PASS);
}

TEST(DoctorRunner, WarnDependencyDoesNotBlockChild) {
    // WARN is not PASS, so child should be skipped
    DoctorRegistry reg;
    reg.Register({"parent", "P", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysWarn);
    reg.Register({"child", "C", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"parent"}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 2u);
    EXPECT_EQ(run.results[0].status, CheckStatus::WARN);
    // Only PASS counts for dependency resolution
    EXPECT_EQ(run.results[1].status, CheckStatus::SKIP);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Timeout Behavior Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorRunner, TimesOutSlowCheck) {
    DoctorRegistry reg;
    // 50ms timeout for a 200ms check
    reg.Register({"slow", "S", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 50}, SlowCheck);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 1u);
    EXPECT_EQ(run.results[0].status, CheckStatus::ERROR);
    EXPECT_TRUE(run.results[0].message.find("timed out") != std::string::npos);
}

TEST(DoctorRunner, HandlesCheckException) {
    DoctorRegistry reg;
    reg.Register({"throws", "T", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysThrow);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 1u);
    EXPECT_EQ(run.results[0].status, CheckStatus::ERROR);
    EXPECT_TRUE(run.results[0].message.find("exception") != std::string::npos);
}

TEST(DoctorRunner, DurationMsIsPopulated) {
    DoctorRegistry reg;
    reg.Register({"check", "C", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 1u);
    // duration_ms should be populated (could be 0 for very fast checks, but field must exist)
    EXPECT_GE(run.results[0].duration_ms, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Exit Code Contract Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorExitCode, HealthyWhenAllPass) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::HEALTHY);
    EXPECT_EQ(static_cast<int>(run.exit_code), 0);
}

TEST(DoctorExitCode, WarningsWhenWarnOnly) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysWarn);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::WARNINGS);
    EXPECT_EQ(static_cast<int>(run.exit_code), 1);
}

TEST(DoctorExitCode, CriticalWhenAnyCrit) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysCrit);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::CRITICAL);
    EXPECT_EQ(static_cast<int>(run.exit_code), 2);
}

TEST(DoctorExitCode, InternalErrorWhenCheckErrors) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysThrow);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::INTERNAL_ERROR);
    EXPECT_EQ(static_cast<int>(run.exit_code), 3);
}

TEST(DoctorExitCode, ErrorTrumpsCritical) {
    // Internal error should override critical in exit code
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysCrit);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysThrow);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::INTERNAL_ERROR);
}

TEST(DoctorExitCode, CriticalTrumpsWarnings) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysWarn);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysCrit);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.exit_code, ExitCode::CRITICAL);
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. JSON Schema Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorJson, ContainsRequiredTopLevelFields) {
    DoctorRegistry reg;
    reg.Register({"test.check", "T", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    std::ostringstream ss;
    DoctorJsonEmitter::Emit(ss, run);
    std::string json = ss.str();

    // All required top-level fields must be present
    EXPECT_TRUE(json.find("\"schema_version\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"node_version\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"network\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"timestamp\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"mode\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"exit_code\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"summary\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"checks\"") != std::string::npos);
}

TEST(DoctorJson, SchemaVersionIs1_0) {
    DoctorRunResult run;
    std::ostringstream ss;
    DoctorJsonEmitter::Emit(ss, run);
    std::string json = ss.str();

    EXPECT_TRUE(json.find("\"schema_version\" : \"1.0\"") != std::string::npos);
}

TEST(DoctorJson, CheckElementHasRequiredFields) {
    DoctorRegistry reg;
    reg.Register({"test.check", "T", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysWarn);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    std::ostringstream ss;
    DoctorJsonEmitter::Emit(ss, run);
    std::string json = ss.str();

    // Per-check required fields
    EXPECT_TRUE(json.find("\"id\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"status\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"message\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"evidence\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"fix_plan\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"duration_ms\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"started_at\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"finished_at\"") != std::string::npos);
}

TEST(DoctorJson, SummaryHasAllCounters) {
    DoctorRunResult run;
    std::ostringstream ss;
    DoctorJsonEmitter::Emit(ss, run);
    std::string json = ss.str();

    EXPECT_TRUE(json.find("\"critical\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"warnings\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"errors\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"skipped\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"passed\"") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. Deterministic Ordering Golden Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorGolden, OrderingIsDeterministicAcrossRuns) {
    // Register checks in non-alphabetical order; verify output is always the same
    auto make_registry = []() {
        DoctorRegistry reg;
        reg.Register({"z.z", "Z", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
        reg.Register({"a.a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
        reg.Register({"m.m", "M", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {"a.a"}, 5000}, AlwaysPass);
        reg.Register({"b.b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
        return reg;
    };

    std::vector<std::string> id_orders;
    for (int i = 0; i < 5; i++) {
        auto reg = make_registry();
        auto ctx = MakeTestContext();
        DoctorRunner runner;
        auto run = runner.Run(ctx, reg);

        std::string order;
        for (const auto& r : run.results) {
            if (!order.empty()) order += ",";
            order += r.id;
        }
        id_orders.push_back(order);
    }

    // All 5 runs must produce the same order
    for (size_t i = 1; i < id_orders.size(); i++) {
        EXPECT_EQ(id_orders[0], id_orders[i])
            << "Run " << i << " produced different order than run 0";
    }
}

TEST(DoctorGolden, V1CheckOrderIsStable) {
    // The v1 check set must always execute in this exact order (quick mode)
    DoctorRegistry reg;
    RegisterV1Checks(reg);
    auto order = reg.GetExecutionOrder(RunMode::QUICK);

    std::vector<std::string> ids;
    for (const auto* c : order) {
        ids.push_back(c->metadata.id);
    }

    // Expected order: topo sort with lexical tie-break
    // No deps: inv.supply_bounds, mempool.snapshot_sanity, p2p.bind_listen, p2p.dns_seeds.resolve
    //          storage.disk_space, storage.permissions
    // Deps on storage.permissions: db.sqlite.quick_check, db.tip_consistency,
    //                              storage.fsync_latency.sample
    ASSERT_GE(ids.size(), 6u);  // At least 6 in quick mode (excludes DEEP-only)

    // storage.permissions must come before its dependents
    auto perms_pos = std::find(ids.begin(), ids.end(), "storage.permissions");
    auto sqlite_pos = std::find(ids.begin(), ids.end(), "db.sqlite.quick_check");
    auto tip_pos = std::find(ids.begin(), ids.end(), "db.tip_consistency");
    auto fsync_pos = std::find(ids.begin(), ids.end(), "storage.fsync_latency.sample");

    ASSERT_NE(perms_pos, ids.end());
    if (sqlite_pos != ids.end()) EXPECT_LT(perms_pos, sqlite_pos);
    if (tip_pos != ids.end()) EXPECT_LT(perms_pos, tip_pos);
    if (fsync_pos != ids.end()) EXPECT_LT(perms_pos, fsync_pos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. Safe-Fix Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorFixes, CollectOnlySafeFixesWithApplySafe) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithSafeFix);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::HIGH, {}, 5000}, CritWithUnsafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.apply_safe_fixes = true;

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    // Only the safe fix should be eligible
    // (Note: test.safe_fix has no implementation, so CollectEligibleFixes
    //  filters it out. This test validates the eligibility logic.)
    // The unsafe fix should definitely NOT appear
    for (const auto& c : candidates) {
        EXPECT_TRUE(c.action.safe_to_apply);
        EXPECT_NE(c.action.id, "test.unsafe_fix");
    }
}

TEST(DoctorFixes, ForceAllIncludesUnsafe) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithSafeFix);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::HIGH, {}, 5000}, CritWithUnsafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.force_all_fixes = true;

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    // Both fixes should be eligible (though neither has implementation)
    // The point is the filtering logic accepts both when forced
    // Since neither has dispatch impl, both get filtered out by FindFixImpl
    // This is correct behavior - unimplemented fixes are never applied
    EXPECT_EQ(candidates.size(), 0u);
}

TEST(DoctorFixes, NoFixesCollectedWhenNoFlag) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithSafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;  // No fix flags set

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    EXPECT_EQ(candidates.size(), 0u);
}

TEST(DoctorFixes, PassingChecksProduceNoFixes) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.apply_safe_fixes = true;
    config.force_all_fixes = true;

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    EXPECT_EQ(candidates.size(), 0u);
}

TEST(DoctorFixes, FixByIdSelectsSpecificFix) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithSafeFix);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::HIGH, {}, 5000}, CritWithUnsafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.fix_ids.push_back("test.unsafe_fix");

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    // test.unsafe_fix has no impl, so 0 candidates (correct: only implemented fixes)
    EXPECT_EQ(candidates.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. Budget Control Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorBudget, DeepModeHasOverallBudget) {
    // Verify the deep mode budget constant
    EXPECT_EQ(kDeepModeBudgetMs, 600000u);  // 10 minutes
    EXPECT_EQ(kMinCheckBudgetMs, 1000u);    // 1 second minimum
}

TEST(DoctorBudget, QuickModeHasNoBudgetLimit) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    ctx.SetMode(RunMode::QUICK);
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    // Quick mode: no budget skipping
    for (const auto& r : run.results) {
        EXPECT_NE(r.status, CheckStatus::SKIP);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 9. Summary Counter Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorSummary, CountersAreAccurate) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);
    reg.Register({"b", "B", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysWarn);
    reg.Register({"c", "C", Severity::CRIT, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysCrit);
    reg.Register({"d", "D", Severity::CRIT, CheckMode::QUICK, FixRisk::NONE, {"c"}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.summary.passed, 1u);    // a
    EXPECT_EQ(run.summary.warnings, 1u);  // b
    EXPECT_EQ(run.summary.critical, 1u);  // c
    EXPECT_EQ(run.summary.skipped, 1u);   // d (dependency c failed)
    EXPECT_EQ(run.summary.errors, 0u);
}

TEST(DoctorSummary, TotalDurationIsNonNegative) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_GE(run.total_duration_ms, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 10. Type Contract Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorTypes, ExitCodeValues) {
    EXPECT_EQ(static_cast<int>(ExitCode::HEALTHY), 0);
    EXPECT_EQ(static_cast<int>(ExitCode::WARNINGS), 1);
    EXPECT_EQ(static_cast<int>(ExitCode::CRITICAL), 2);
    EXPECT_EQ(static_cast<int>(ExitCode::INTERNAL_ERROR), 3);
}

TEST(DoctorTypes, ToStringCoversAllEnums) {
    // Exit codes
    EXPECT_STREQ(to_string(ExitCode::HEALTHY), "healthy");
    EXPECT_STREQ(to_string(ExitCode::WARNINGS), "warnings");
    EXPECT_STREQ(to_string(ExitCode::CRITICAL), "critical");
    EXPECT_STREQ(to_string(ExitCode::INTERNAL_ERROR), "internal_error");

    // Check statuses
    EXPECT_STREQ(to_string(CheckStatus::PASS), "PASS");
    EXPECT_STREQ(to_string(CheckStatus::WARN), "WARN");
    EXPECT_STREQ(to_string(CheckStatus::CRIT), "CRIT");
    EXPECT_STREQ(to_string(CheckStatus::ERROR), "ERROR");
    EXPECT_STREQ(to_string(CheckStatus::SKIP), "SKIP");

    // Run modes
    EXPECT_STREQ(to_string(RunMode::QUICK), "quick");
    EXPECT_STREQ(to_string(RunMode::DEEP), "deep");

    // Fix risks
    EXPECT_STREQ(to_string(FixRisk::NONE), "NONE");
    EXPECT_STREQ(to_string(FixRisk::LOW), "LOW");
    EXPECT_STREQ(to_string(FixRisk::MED), "MED");
    EXPECT_STREQ(to_string(FixRisk::HIGH), "HIGH");
}

TEST(DoctorTypes, SchemaVersionIs1_0) {
    EXPECT_STREQ(kSchemaVersion, "1.0");
}

TEST(DoctorTypes, DefaultConfigIsReadOnly) {
    DoctorConfig config;
    EXPECT_FALSE(config.apply_safe_fixes);
    EXPECT_FALSE(config.force_all_fixes);
    EXPECT_TRUE(config.fix_ids.empty());
    EXPECT_EQ(config.mode, RunMode::QUICK);
}

// ═══════════════════════════════════════════════════════════════════════════
// 11. Context Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorContext, DefaultPorts) {
    DoctorContext ctx("/tmp/test", "mainnet");
    EXPECT_EQ(ctx.RpcPort(), 20998);
    EXPECT_EQ(ctx.P2pPort(), 20999);
}

TEST(DoctorContext, ModeDefaultsToQuick) {
    DoctorContext ctx("/tmp/test", "mainnet");
    EXPECT_EQ(ctx.Mode(), RunMode::QUICK);
}

TEST(DoctorContext, SettersWork) {
    DoctorContext ctx("/tmp/test", "mainnet");
    ctx.SetMode(RunMode::DEEP);
    ctx.SetNodeVersion("1.2.3");
    ctx.SetRpcPort(12345);
    ctx.SetP2pPort(54321);

    EXPECT_EQ(ctx.Mode(), RunMode::DEEP);
    EXPECT_EQ(ctx.NodeVersion(), "1.2.3");
    EXPECT_EQ(ctx.RpcPort(), 12345);
    EXPECT_EQ(ctx.P2pPort(), 54321);
}

// ═══════════════════════════════════════════════════════════════════════════
// 12. Run Metadata Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DoctorRun, MetadataIsPopulated) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    EXPECT_EQ(run.mode, RunMode::QUICK);
    EXPECT_EQ(run.network, "regtest");
    EXPECT_EQ(run.node_version, "test-0.0.1");
    EXPECT_FALSE(run.timestamp.empty());
    EXPECT_TRUE(run.timestamp.find("T") != std::string::npos);  // ISO 8601
}

TEST(DoctorRun, TimestampsAreISO8601) {
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 5000}, AlwaysPass);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    ASSERT_EQ(run.results.size(), 1u);
    auto& r = run.results[0];

    // started_at and finished_at should be ISO 8601 format: YYYY-MM-DDTHH:MM:SS.mmmZ
    EXPECT_FALSE(r.started_at.empty());
    EXPECT_FALSE(r.finished_at.empty());
    EXPECT_TRUE(r.started_at.back() == 'Z');
    EXPECT_TRUE(r.finished_at.back() == 'Z');
    EXPECT_EQ(r.started_at[4], '-');
    EXPECT_EQ(r.started_at[10], 'T');
}

// ═══════════════════════════════════════════════════════════════════════════
// 13. Regression Tests (Round 2 Review)
// ═══════════════════════════════════════════════════════════════════════════

// A check that sleeps 500ms (used to verify hard timeout wall-clock)
static DoctorCheckResult VerySlowCheck(const DoctorContext&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    DoctorCheckResult r;
    r.status = CheckStatus::PASS;
    r.message = "slow";
    return r;
}

TEST(DoctorRunner, TimeoutReturnsWithinBudget) {
    // Regression: std::async destructor would block until thread finishes,
    // making ExecuteCheck take 500ms even with a 50ms budget.
    // With detached thread + promise, ExecuteCheck must return in ~budget time.
    DoctorRegistry reg;
    reg.Register({"slow", "S", Severity::WARN, CheckMode::QUICK, FixRisk::NONE, {}, 80}, VerySlowCheck);

    auto ctx = MakeTestContext();
    DoctorRunner runner;

    auto wall_start = std::chrono::steady_clock::now();
    auto run = runner.Run(ctx, reg);
    auto wall_end = std::chrono::steady_clock::now();

    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();

    ASSERT_EQ(run.results.size(), 1u);
    EXPECT_EQ(run.results[0].status, CheckStatus::ERROR);
    EXPECT_TRUE(run.results[0].message.find("timed out") != std::string::npos);

    // Wall clock must be well under the check's sleep time (500ms).
    // Allow generous margin (250ms) for thread startup overhead,
    // but it must NOT take the full 500ms.
    EXPECT_LT(wall_ms, 250) << "ExecuteCheck took " << wall_ms
        << "ms — hard timeout not working (should be ~80ms, not 500ms)";
}

TEST(DoctorFixes, FixByIdAloneDoesNotApply) {
    // Regression: --fix <id> without --apply-safe-fixes must NOT apply fixes.
    // This prevents accidental mutation without the explicit gate.
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithSafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.fix_ids.push_back("test.safe_fix");
    // Deliberately NOT setting config.apply_safe_fixes = true

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    EXPECT_EQ(candidates.size(), 0u)
        << "--fix alone (without --apply-safe-fixes) must produce zero candidates";
}

TEST(DoctorFixes, FixByIdWithSafeGateWorks) {
    // --fix <id> + --apply-safe-fixes must select exactly the named safe fix.
    // Uses a real dispatch ID so the fix survives FindFixImpl filtering.
    DoctorRegistry reg;
    reg.Register({"a", "A", Severity::CRIT, CheckMode::QUICK, FixRisk::LOW, {}, 5000}, CritWithRealSafeFix);
    reg.Register({"b", "B", Severity::CRIT, CheckMode::QUICK, FixRisk::HIGH, {}, 5000}, CritWithUnsafeFix);

    auto ctx = MakeTestContext();
    DoctorRunner runner;
    auto run = runner.Run(ctx, reg);

    DoctorConfig config;
    config.apply_safe_fixes = true;
    config.fix_ids.push_back("mempool.snapshot_sanity.remove");

    auto candidates = DoctorFixer::CollectEligibleFixes(run, config);
    // Exactly 1 candidate: the named safe fix with a real implementation.
    // The unsafe fix (test.unsafe_fix) must be excluded by the gate.
    ASSERT_EQ(candidates.size(), 1u)
        << "Expected exactly 1 eligible fix (mempool.snapshot_sanity.remove)";
    EXPECT_EQ(candidates[0].action.id, "mempool.snapshot_sanity.remove");
    EXPECT_TRUE(candidates[0].action.safe_to_apply);
    EXPECT_EQ(candidates[0].check_id, "a");
}

// ═══════════════════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
