// Copyright (c) 2026 Dinero Labs.
//
// Finding 15: the block-write counters bypassed MetricsRegistry entirely.
//
// They were bare globals, so ExportMetrics and ExportMetricsJSON never saw
// them: the only way to read a durability counter was to attach a debugger or
// catch a rate-limited log line. The acceptance path that maintains them
// already calls into this registry, so surfacing them cost the same few lines
// it took to keep them hidden.
//
// These tests fail if the wiring is removed -- that is their entire purpose. A
// counter that is incremented but never exported is indistinguishable from one
// that is not incremented at all, from anywhere outside the process.

#include <gtest/gtest.h>

#include <string>

#include "daemon/block_write_metrics.h"
#include "metrics/metrics_registry.h"

using dinero::metrics::MetricsRegistry;

TEST(BlockWriteMetricsExport, RegistryReportsTheDurableWriteCounter) {
    dinero::daemon::ResetBlockWriteMetricsForTest();
    EXPECT_EQ(MetricsRegistry::GetDurableBodyWrites(), 0u);

    ++dinero::daemon::g_durable_body_writes;
    ++dinero::daemon::g_durable_body_writes;

    EXPECT_EQ(MetricsRegistry::GetDurableBodyWrites(), 2u)
        << "the registry accessor is not reading the live counter — remove the "
           "wiring and this is exactly what regresses";
}

TEST(BlockWriteMetricsExport, RegistryReportsSuppressedAcceptances) {
    dinero::daemon::ResetBlockWriteMetricsForTest();
    ++dinero::daemon::g_concurrent_acceptances_suppressed;
    EXPECT_EQ(MetricsRegistry::GetConcurrentAcceptancesSuppressed(), 1u);
}

// Reading through the registry must not consume the value. Two collectors --
// a Prometheus scrape and a JSON read, say -- must not race each other for the
// truth.
TEST(BlockWriteMetricsExport, RegistryReadIsNonDestructive) {
    dinero::daemon::ResetBlockWriteMetricsForTest();
    ++dinero::daemon::g_durable_body_writes;
    ++dinero::daemon::g_durable_body_writes;
    ++dinero::daemon::g_durable_body_writes;

    const uint64_t a = MetricsRegistry::GetDurableBodyWrites();
    const uint64_t b = MetricsRegistry::GetDurableBodyWrites();
    EXPECT_EQ(a, 3u);
    EXPECT_EQ(b, a) << "collection consumed the counter; a second scrape would "
                       "report a value that never happened";
    EXPECT_EQ(dinero::daemon::g_durable_body_writes.load(), 3u)
        << "the underlying counter was mutated by a read";
}

// The exported text must actually NAME the metrics. An accessor that exists but
// is never emitted leaves them just as invisible to an operator as before.
TEST(BlockWriteMetricsExport, ExportedTextCarriesBothCounters) {
    dinero::daemon::ResetBlockWriteMetricsForTest();
    ++dinero::daemon::g_durable_body_writes;
    ++dinero::daemon::g_concurrent_acceptances_suppressed;

    const std::string out = MetricsRegistry::ExportMetrics();

    // Assert the SAMPLE line (name + value), not merely that the name appears
    // somewhere. Prometheus output repeats the name in its "# HELP" and
    // "# TYPE" comments, so a substring search is satisfied by the comments
    // alone -- a mutation that renamed only the value line left this test
    // green. The sample line is the part a scraper actually consumes.
    EXPECT_NE(out.find("din_durable_body_writes_total 1"), std::string::npos)
        << "no durable-write SAMPLE line in the exported metrics (a HELP/TYPE "
           "comment mentioning the name is not an exported value):\n" << out;
    EXPECT_NE(out.find("din_concurrent_acceptances_suppressed_total 1"),
              std::string::npos)
        << "no suppressed-acceptance SAMPLE line in the exported metrics:\n" << out;
}
