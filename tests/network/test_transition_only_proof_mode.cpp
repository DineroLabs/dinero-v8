#include "daemon/utreexo_proof_mode.h"

#include <cassert>
#include <iostream>

using namespace dinero;

namespace {

int tests_passed = 0;
int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while (0)

void test_empty_batch_prefers_transition_proof() {
    consensus::BlockUtreexoData proof_data;
    consensus::UtreexoTransitionProof tp;

    TEST_ASSERT(
        daemon_helpers::ShouldUseTransitionProof(proof_data, tp),
        "Empty batch payload with transition proof must use TP validation"
    );
}

void test_legacy_zero_root_batch_prefers_transition_proof() {
    consensus::BlockUtreexoData proof_data;
    proof_data.accumulator_root_before.assign(32, 0x00);
    consensus::UtreexoTransitionProof tp;

    TEST_ASSERT(
        !proof_data.isEmpty(),
        "Legacy zero-root payload should not look empty to BlockUtreexoData"
    );
    TEST_ASSERT(
        daemon_helpers::IsLegacyTransitionOnlyBatchPayload(proof_data),
        "Legacy zero-root payload must be recognized as TP-only"
    );
    TEST_ASSERT(
        daemon_helpers::ShouldUseTransitionProof(proof_data, tp),
        "Legacy zero-root payload with transition proof must use TP validation"
    );
}

void test_nonzero_batch_root_stays_batch_mode() {
    consensus::BlockUtreexoData proof_data;
    proof_data.accumulator_root_before.assign(32, 0x11);
    consensus::UtreexoTransitionProof tp;

    TEST_ASSERT(
        !daemon_helpers::IsLegacyTransitionOnlyBatchPayload(proof_data),
        "Nonzero root_before payload must not be treated as legacy TP-only"
    );
    TEST_ASSERT(
        !daemon_helpers::ShouldUseTransitionProof(proof_data, tp),
        "Coinbase-only batch payload with real root_before must stay in batch mode"
    );
}

void test_missing_transition_proof_never_switches_modes() {
    consensus::BlockUtreexoData proof_data;
    proof_data.accumulator_root_before.assign(32, 0x00);

    TEST_ASSERT(
        !daemon_helpers::ShouldUseTransitionProof(proof_data, std::nullopt),
        "Without a transition proof we must never switch to TP validation"
    );
}

}  // namespace

int main() {
    std::cout << "Transition-only proof mode regression..." << std::endl;

    test_empty_batch_prefers_transition_proof();
    test_legacy_zero_root_batch_prefers_transition_proof();
    test_nonzero_batch_root_stays_batch_mode();
    test_missing_transition_proof_never_switches_modes();

    std::cout << "PASS: " << tests_passed << "/" << tests_total << " assertions" << std::endl;
    return 0;
}
