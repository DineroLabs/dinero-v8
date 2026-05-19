// MiningEventBus unit test.
//
// dinero_core gap closure — src/daemon/mining_events.cpp had zero direct test
// coverage in the source-map audit. MiningEventBus is a thread-safe ring
// buffer of mining-side events (hashrate, state, block-found) that
// downstream subscribers (the RPC layer, the qt UI, metrics exporters)
// consume via snapshot polling.
//
// Five contract properties:
//   1. Fresh bus has empty snapshot
//   2. push* assigns monotonically increasing event ids starting at 1
//   3. snapshot(since) returns only events with id > since
//   4. snapshot honours max (default 200, explicit cap respected)
//   5. Event payloads carry the right shape ({type, ts, id, ...})

#include "daemon/mining_events.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_pass = 0;
int g_total = 0;

#define EXPECT(cond, msg) do { \
    ++g_total; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        std::abort(); \
    } \
    ++g_pass; \
} while (0)

void test_fresh_bus_is_empty() {
    std::cout << "Test 1: Fresh bus returns empty snapshot\n";
    MiningEventBus bus;
    Json::Value snap = bus.snapshot(/*since=*/0);
    EXPECT(snap.isArray(),
           "snapshot must return a JSON array");
    EXPECT(snap.size() == 0,
           "fresh bus must produce an empty snapshot");
    std::cout << "  PASSED\n";
}

void test_push_assigns_monotonic_ids() {
    std::cout << "Test 2: push* assigns monotonically increasing ids from 1\n";
    MiningEventBus bus;
    bus.pushHashrate(100);
    bus.pushState(true);
    bus.pushHashrate(200);
    Json::Value snap = bus.snapshot(0);
    EXPECT(snap.size() == 3,
           "snapshot must return all three pushed events");
    EXPECT(snap[0]["id"].asInt64() == 1,
           "first event must have id == 1");
    EXPECT(snap[1]["id"].asInt64() == 2,
           "second event must have id == 2");
    EXPECT(snap[2]["id"].asInt64() == 3,
           "third event must have id == 3");
    std::cout << "  PASSED\n";
}

void test_snapshot_since_filter() {
    std::cout << "Test 3: snapshot(since) returns only events with id > since\n";
    MiningEventBus bus;
    bus.pushHashrate(10);   // id=1
    bus.pushHashrate(20);   // id=2
    bus.pushHashrate(30);   // id=3

    Json::Value first = bus.snapshot(/*since=*/0);
    EXPECT(first.size() == 3, "snapshot(0) must return all 3 events");

    Json::Value after_first = bus.snapshot(/*since=*/1);
    EXPECT(after_first.size() == 2,
           "snapshot(1) must skip event id=1 and return 2 events");
    EXPECT(after_first[0]["id"].asInt64() == 2,
           "snapshot(1)[0] must be id=2");

    Json::Value after_all = bus.snapshot(/*since=*/3);
    EXPECT(after_all.size() == 0,
           "snapshot(latest_id) must return empty (catching up)");
    std::cout << "  PASSED\n";
}

void test_snapshot_max_cap() {
    std::cout << "Test 4: snapshot honours max cap\n";
    MiningEventBus bus;
    for (int i = 0; i < 50; ++i) {
        bus.pushHashrate(static_cast<uint64_t>(i));
    }
    Json::Value capped = bus.snapshot(/*since=*/0, /*max=*/10);
    EXPECT(capped.size() <= 10,
           "snapshot(0, max=10) must return at most 10 events");
    EXPECT(capped.size() == 10,
           "snapshot(0, max=10) with 50 pending events must return exactly 10");
    std::cout << "  PASSED\n";
}

void test_event_payload_shape() {
    std::cout << "Test 5: pushed events carry correct payload shape\n";
    MiningEventBus bus;
    bus.pushHashrate(123456);
    bus.pushState(true);
    bus.pushBlockFound(/*height=*/42, "deadbeef", /*bits=*/0x207fffff,
                       /*rewardAtoms=*/5000000000ULL, "din1ptest");

    Json::Value snap = bus.snapshot(0);
    EXPECT(snap.size() == 3, "snapshot must return all 3 events");

    // Hashrate event shape
    EXPECT(snap[0]["type"].asString() == "hashrate",
           "hashrate event must have type == 'hashrate'");
    EXPECT(snap[0]["hps"].asUInt64() == 123456ULL,
           "hashrate event must carry hps payload");
    EXPECT(snap[0].isMember("ts"),
           "every event must carry an ISO timestamp");

    // State event shape
    EXPECT(snap[1]["type"].asString() == "state",
           "state event must have type == 'state'");
    EXPECT(snap[1]["running"].asBool() == true,
           "state event must carry running flag");

    // Block-found event shape
    EXPECT(snap[2]["type"].asString() == "block_found",
           "block-found event must have type == 'block_found'");
    EXPECT(snap[2]["height"].asInt() == 42,
           "block-found event must carry height");
    EXPECT(snap[2]["hash"].asString() == "deadbeef",
           "block-found event must carry hash");
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "MiningEventBus unit test\n";
    std::cout << "========================\n";
    test_fresh_bus_is_empty();
    test_push_assigns_monotonic_ids();
    test_snapshot_since_filter();
    test_snapshot_max_cap();
    test_event_payload_shape();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
