// Observable counters + log rate limiting for the block acceptance path.
//
// Two problems this exists for, both measured on a live deferred-AssumeUTXO
// node where one block height was re-delivered 83,738 times:
//
//  1. Every delivery re-wrote and fsynced a body already on disk. Proving the
//     fix needs a count of ACTUAL durable writes -- not a grep of log lines,
//     which measures logging, not I/O.
//  2. Every delivery logged several lines, producing 9.3 MB/min (~13 GB/day).
//     Duplicate-path logging is therefore rate limited and aggregated.
#pragma once

#include <atomic>
#include <cstdint>

namespace dinero {
namespace daemon {

// Bodies actually written to flatfile storage (each one an fsync).
extern std::atomic<uint64_t> g_durable_body_writes;
// Writes skipped because the body was already durable.
extern std::atomic<uint64_t> g_duplicate_body_writes_avoided;
// Duplicate/side-chain log lines suppressed by the rate limiter.
extern std::atomic<uint64_t> g_duplicate_logs_suppressed;

/**
 * True at most once per `interval_seconds` per call site.
 *
 * `state` must be a distinct static per call site; it holds the last-emitted
 * steady_clock tick. Callers that are refused should still bump a counter so
 * the aggregate is reported when a line is finally emitted.
 */
bool ShouldEmitRateLimited(std::atomic<uint64_t>& state,
                           uint32_t interval_seconds);

// Test seams.
void ResetBlockWriteMetricsForTest();

}  // namespace daemon
}  // namespace dinero
