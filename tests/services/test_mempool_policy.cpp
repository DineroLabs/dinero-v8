// MempoolPolicy unit test.
//
// Coverage gap from docs/architecture/dinero-core-source-map.md —
// src/policy/mempool_policy.cpp is a load-bearing policy file inside
// dinero_core with currently NO direct test coverage. This is not a
// service-boundary test (mempool_policy isn't a service hub), but it
// reuses the same compiled-binary smoke shape and lives next to the
// service-boundary tests because both are dinero_core gap closures.
//
// Test scope deliberately avoids anything that needs a real Transaction
// (those tests are integration-shaped and need transaction.cpp,
// dinero_tx_primitives, signing infra). Properties exercised:
//   - Default MempoolConfig matches the documented protocol defaults
//   - getErrorMessage returns non-empty string for every ValidationResult
//     enum value (catches regression where new enums are added without
//     paired error messages — a real prod foot-gun)
//   - getConfig/updateConfig round-trip preserves all fields
//   - EvictionPolicy::selectForEviction on an empty candidate list returns
//     empty
//   - EvictionPolicy::selectForEviction picks lower fee-rate candidates
//     first (the core eviction-ordering invariant)

#include "policy/mempool_policy.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

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

void test_default_config_matches_documented_protocol_limits() {
    std::cout << "Test 1: Default MempoolConfig has documented protocol limits\n";
    dinero::policy::MempoolConfig cfg;
    // The mempool-policy.md doc + header comments lock these defaults.
    EXPECT(cfg.max_mempool_size == 300 * 1024 * 1024ULL,
           "default max_mempool_size must be 300 MiB");
    EXPECT(cfg.max_tx_size == 100 * 1024U,
           "default max_tx_size must be 100 KiB");
    EXPECT(cfg.max_tx_weight == 400 * 1024U,
           "default max_tx_weight must be 400K weight units");
    EXPECT(cfg.min_relay_fee_rate == 1000ULL,
           "default min_relay_fee_rate must be 1000 una per KB");
    EXPECT(cfg.dust_threshold == 546ULL,
           "default dust_threshold must be 546 una (Bitcoin convention)");
    EXPECT(cfg.max_sigops == 20000U,
           "default max_sigops must be 20000");
    EXPECT(cfg.max_ancestor_count == 25U,
           "default max_ancestor_count must be 25 (Bitcoin BIP125 convention)");
    EXPECT(cfg.max_descendant_count == 25U,
           "default max_descendant_count must be 25");
    std::cout << "  PASSED\n";
}

void test_get_error_message_covers_every_enum_value() {
    std::cout << "Test 2: getErrorMessage() returns non-empty for every enum\n";
    dinero::policy::MempoolPolicy pol;
    using R = dinero::policy::ValidationResult;
    const std::vector<R> all_results = {
        R::VALID, R::INVALID_SIZE, R::INVALID_WEIGHT, R::INVALID_SIGOPS,
        R::INVALID_FEE, R::INVALID_SCRIPT, R::INVALID_DUST, R::INVALID_RBF,
        R::ALREADY_IN_MEMPOOL, R::CONFLICTING_TX,
    };
    for (R r : all_results) {
        const std::string msg = pol.getErrorMessage(r);
        EXPECT(!msg.empty(),
               "getErrorMessage must return non-empty string for every ValidationResult");
    }
    std::cout << "  PASSED\n";
}

void test_config_roundtrip() {
    std::cout << "Test 3: getConfig()/updateConfig() round-trip\n";
    dinero::policy::MempoolPolicy pol;
    dinero::policy::MempoolConfig cfg = pol.getConfig();
    cfg.max_mempool_size = 42ULL;
    cfg.dust_threshold = 999ULL;
    cfg.max_sigops = 7U;
    pol.updateConfig(cfg);
    const auto& after = pol.getConfig();
    EXPECT(after.max_mempool_size == 42ULL,
           "updated max_mempool_size must round-trip");
    EXPECT(after.dust_threshold == 999ULL,
           "updated dust_threshold must round-trip");
    EXPECT(after.max_sigops == 7U,
           "updated max_sigops must round-trip");
    std::cout << "  PASSED\n";
}

void test_eviction_with_empty_candidates() {
    std::cout << "Test 4: EvictionPolicy::selectForEviction([], _) returns empty\n";
    dinero::policy::EvictionPolicy ev;
    auto picked = ev.selectForEviction({}, /*bytes_to_free=*/1024);
    EXPECT(picked.empty(),
           "selectForEviction over an empty candidate list must return empty");
    std::cout << "  PASSED\n";
}

void test_eviction_picks_lower_fee_rate_first() {
    std::cout << "Test 5: EvictionPolicy picks lower fee-rate candidates first\n";
    dinero::policy::EvictionPolicy ev;
    using C = dinero::policy::EvictionPolicy::EvictionCandidate;
    std::vector<C> candidates = {
        C{ "high_fee", /*fee_rate=*/10000, /*size=*/1024, /*time=*/100, /*desc=*/0, /*hasUnconfP=*/false },
        C{ "low_fee",  /*fee_rate=*/1,     /*size=*/1024, /*time=*/100, /*desc=*/0, /*hasUnconfP=*/false },
        C{ "mid_fee",  /*fee_rate=*/500,   /*size=*/1024, /*time=*/100, /*desc=*/0, /*hasUnconfP=*/false },
    };
    // Free enough bytes to require exactly one eviction.
    auto picked = ev.selectForEviction(candidates, /*bytes_to_free=*/512);
    EXPECT(!picked.empty(),
           "selectForEviction must pick at least one candidate when bytes_to_free > 0");
    // First evicted must be the lowest-fee-rate candidate.
    EXPECT(picked.front() == "low_fee",
           "first evicted candidate must be the lowest-fee-rate ('low_fee')");
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "MempoolPolicy unit test\n";
    std::cout << "=======================\n";
    test_default_config_matches_documented_protocol_limits();
    test_get_error_message_covers_every_enum_value();
    test_config_roundtrip();
    test_eviction_with_empty_candidates();
    test_eviction_picks_lower_fee_rate_first();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
