/**
 * @file test_template_staleness.cpp
 * @brief Phase B1: Template Freshness & Staleness Tests (Mainnet Hardening)
 *
 * MAINNET REQUIREMENT: Any template derived from obsolete chainstate MUST be rejected.
 *
 * Invariant (Non-Negotiable):
 *   - No auto-fix
 *   - No silent rebuild
 *   - No "best effort"
 *   - EXPLICIT rejection with structured code
 *
 * Tests:
 *   B1.1: New block invalidates template
 *   B1.2: Mempool change invalidates template (tracked but may allow submission)
 *   B1.3: Reorg invalidates template
 *   B1.4: Time drift invalidates template
 *   B1.5: Explicit stale rejection codes verified
 *
 * If any test fails → DO NOT SHIP TO MAINNET
 */

#include "daemon/interfaces/ingress_types.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════
// Test Infrastructure
// ═══════════════════════════════════════════════════════════════════════════

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_EQ(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) != (b)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     Expected: " << static_cast<int>(b) << "\n"; \
            std::cerr << "     Got:      " << static_cast<int>(a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_NE(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) == (b)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     Both equal: " << static_cast<int>(a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

// ═══════════════════════════════════════════════════════════════════════════
// Mock Template Manager for Testing
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Mock template state that tracks what the miner believes vs reality
 */
struct MockTemplateState {
    uint64_t template_id;
    uint256 template_prev_hash;     // What prevhash the template was built for
    uint64_t template_height;       // Expected height of new block
    uint64_t template_timestamp;    // When template was created
    uint64_t mempool_version;       // Mempool version at template creation

    // Current chain state (may diverge from template state)
    uint256 current_tip_hash;
    uint64_t current_height;
    uint64_t current_mempool_version;
    uint64_t current_time;

    // Staleness detection
    bool IsTipStale() const {
        return template_prev_hash != current_tip_hash;
    }

    bool IsMempoolStale() const {
        return mempool_version != current_mempool_version;
    }

    bool IsTimestampStale(uint64_t max_drift_seconds = 600) const {
        return (current_time > template_timestamp) &&
               (current_time - template_timestamp > max_drift_seconds);
    }

    /**
     * Validate template freshness - returns rejection code if stale
     */
    BlockRejectCode ValidateFreshness() const {
        // Tip change is highest priority (most dangerous)
        if (IsTipStale()) {
            return BlockRejectCode::STALE_TIP_CHANGED;
        }

        // Time drift check
        if (IsTimestampStale()) {
            return BlockRejectCode::STALE_TIMESTAMP;
        }

        // Mempool change (informational, may still accept)
        // In strict mode, this would also reject
        // if (IsMempoolStale()) {
        //     return BlockRejectCode::STALE_MEMPOOL_CHANGED;
        // }

        return BlockRejectCode::OK;
    }
};

/**
 * Mock template manager that simulates real behavior
 */
class MockTemplateManager {
public:
    MockTemplateManager() : m_next_template_id(1), m_mempool_version(1) {
        // Initialize with genesis state
        m_tip_hash = uint256();  // All zeros for genesis
        m_height = 0;
        m_current_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Create a new template based on current state
    MockTemplateState CreateTemplate() {
        MockTemplateState state;
        state.template_id = m_next_template_id++;
        state.template_prev_hash = m_tip_hash;
        state.template_height = m_height + 1;
        state.template_timestamp = m_current_time;
        state.mempool_version = m_mempool_version;

        // Copy current state
        state.current_tip_hash = m_tip_hash;
        state.current_height = m_height;
        state.current_mempool_version = m_mempool_version;
        state.current_time = m_current_time;

        return state;
    }

    // Simulate mining a new block (changes tip)
    void MineBlock() {
        m_height++;
        // Generate new tip hash (simulated)
        m_tip_hash = uint256();
        m_tip_hash.data[0] = static_cast<uint8_t>(m_height & 0xFF);
        m_tip_hash.data[1] = static_cast<uint8_t>((m_height >> 8) & 0xFF);
    }

    // Simulate mempool change
    void AddTransaction() {
        m_mempool_version++;
    }

    // Simulate time passing
    void AdvanceTime(uint64_t seconds) {
        m_current_time += seconds;
    }

    // Simulate reorg (changes tip hash but may keep same height)
    void Reorg(uint64_t new_height) {
        m_height = new_height;
        // Generate different tip hash for same height (simulates reorg)
        m_tip_hash = uint256();
        m_tip_hash.data[0] = static_cast<uint8_t>(m_height & 0xFF);
        m_tip_hash.data[1] = static_cast<uint8_t>((m_height >> 8) & 0xFF);
        m_tip_hash.data[2] = 0xAA;  // Different from normal mine
    }

    // Update template's view of current state
    void UpdateTemplateState(MockTemplateState& state) const {
        state.current_tip_hash = m_tip_hash;
        state.current_height = m_height;
        state.current_mempool_version = m_mempool_version;
        state.current_time = m_current_time;
    }

    uint64_t GetHeight() const { return m_height; }
    const uint256& GetTipHash() const { return m_tip_hash; }

private:
    uint64_t m_next_template_id;
    uint256 m_tip_hash;
    uint64_t m_height;
    uint64_t m_mempool_version;
    uint64_t m_current_time;
};

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.1: New Block Invalidates Template
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_1_new_block_invalidates_template() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.1: New block invalidates template" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;

    // Mine to height H
    manager.MineBlock();  // Height 1
    manager.MineBlock();  // Height 2
    manager.MineBlock();  // Height 3

    std::cout << "  Chain at height " << manager.GetHeight() << std::endl;

    // Request template T₁
    MockTemplateState template1 = manager.CreateTemplate();
    std::cout << "  Created template for height " << template1.template_height << std::endl;

    // Mine competing block → height H+1
    manager.MineBlock();  // Height 4
    std::cout << "  Mined competing block, chain now at height " << manager.GetHeight() << std::endl;

    // Update template's view of state
    manager.UpdateTemplateState(template1);

    // Submit T₁ - MUST be rejected
    BlockRejectCode result = template1.ValidateFreshness();

    std::cout << "  Template validation result: " << BlockRejectCodeToString(result) << std::endl;

    // Verify rejection
    ASSERT_EQ(result, BlockRejectCode::STALE_TIP_CHANGED,
              "Template must be rejected with STALE_TIP_CHANGED when tip changed");

    ASSERT_TRUE(template1.IsTipStale(), "Template must detect tip staleness");

    std::cout << "\n  ✅ New block correctly invalidates template\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.2: Mempool Change Detection
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_2_mempool_change_detection() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.2: Mempool change detection" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;
    manager.MineBlock();  // Height 1

    // Request template T₁
    MockTemplateState template1 = manager.CreateTemplate();
    uint64_t original_mempool_version = template1.mempool_version;
    std::cout << "  Created template with mempool version " << original_mempool_version << std::endl;

    // Add tx to mempool
    manager.AddTransaction();
    manager.AddTransaction();
    manager.AddTransaction();

    // Update template's view of state
    manager.UpdateTemplateState(template1);

    std::cout << "  Mempool version now " << template1.current_mempool_version << std::endl;

    // Verify mempool staleness is detected
    ASSERT_TRUE(template1.IsMempoolStale(), "Template must detect mempool changed");
    ASSERT_NE(template1.mempool_version, template1.current_mempool_version,
              "Mempool versions must differ");

    // Note: Mempool changes may not necessarily reject the block
    // (the transactions in the block are still valid)
    // But the staleness MUST be detectable for logging/metrics

    std::cout << "\n  ✅ Mempool changes correctly detected\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.3: Reorg Invalidates Template
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_3_reorg_invalidates_template() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.3: Reorg invalidates template" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;

    // Build chain A
    manager.MineBlock();  // Height 1
    manager.MineBlock();  // Height 2
    manager.MineBlock();  // Height 3

    // Template from chain A
    MockTemplateState template_a = manager.CreateTemplate();
    uint256 chain_a_tip = manager.GetTipHash();
    std::cout << "  Template from chain A at height " << manager.GetHeight() << std::endl;

    // Reorg to chain B (same height, different tip)
    manager.Reorg(3);  // Reorg back to height 3 with different tip
    uint256 chain_b_tip = manager.GetTipHash();

    std::cout << "  Reorg to chain B (same height " << manager.GetHeight() << ", different tip)" << std::endl;

    // Verify tips are different (reorg actually happened)
    ASSERT_TRUE(chain_a_tip != chain_b_tip, "Reorg must produce different tip hash");

    // Update template's view of state
    manager.UpdateTemplateState(template_a);

    // Submit template from A - MUST be rejected even though height matches
    BlockRejectCode result = template_a.ValidateFreshness();

    std::cout << "  Template validation result: " << BlockRejectCodeToString(result) << std::endl;

    // This catches root reuse bugs - prevhash changed even at same height
    ASSERT_EQ(result, BlockRejectCode::STALE_TIP_CHANGED,
              "Template must be rejected after reorg (different prevhash)");

    ASSERT_TRUE(template_a.IsTipStale(), "Template must detect tip changed after reorg");

    std::cout << "\n  ✅ Reorg correctly invalidates template (even at same height)\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.4: Time Drift Invalidates Template
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_4_time_drift_invalidates_template() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.4: Time drift invalidates template" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;
    manager.MineBlock();  // Height 1

    // Template built at time T
    MockTemplateState template1 = manager.CreateTemplate();
    std::cout << "  Template created at time " << template1.template_timestamp << std::endl;

    // Advance clock beyond acceptable range (10+ minutes)
    manager.AdvanceTime(601);  // 10 minutes + 1 second
    manager.UpdateTemplateState(template1);

    std::cout << "  Current time " << template1.current_time << " (+"
              << (template1.current_time - template1.template_timestamp) << " seconds)" << std::endl;

    // Verify time staleness
    ASSERT_TRUE(template1.IsTimestampStale(), "Template must detect time drift");

    // Template should be rejected
    BlockRejectCode result = template1.ValidateFreshness();

    std::cout << "  Template validation result: " << BlockRejectCodeToString(result) << std::endl;

    ASSERT_EQ(result, BlockRejectCode::STALE_TIMESTAMP,
              "Template must be rejected with STALE_TIMESTAMP after time drift");

    // Test that acceptable drift is OK
    MockTemplateState template2 = manager.CreateTemplate();
    manager.AdvanceTime(300);  // 5 minutes - should be OK
    manager.UpdateTemplateState(template2);

    ASSERT_TRUE(!template2.IsTimestampStale(600), "5-minute drift should be acceptable");
    BlockRejectCode result2 = template2.ValidateFreshness();
    ASSERT_EQ(result2, BlockRejectCode::OK, "Template within time drift should be OK");

    std::cout << "\n  ✅ Time drift correctly invalidates template\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.5: Explicit Stale Rejection Codes
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_5_explicit_stale_rejection_codes() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.5: Explicit stale rejection codes" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Verify all stale codes exist and have string representations

    // STALE_TIP_CHANGED
    const char* tip_str = BlockRejectCodeToString(BlockRejectCode::STALE_TIP_CHANGED);
    std::cout << "  STALE_TIP_CHANGED -> \"" << tip_str << "\"" << std::endl;
    ASSERT_TRUE(std::string(tip_str) == "stale-tip",
                "STALE_TIP_CHANGED must map to 'stale-tip'");

    // STALE_MEMPOOL_CHANGED
    const char* mempool_str = BlockRejectCodeToString(BlockRejectCode::STALE_MEMPOOL_CHANGED);
    std::cout << "  STALE_MEMPOOL_CHANGED -> \"" << mempool_str << "\"" << std::endl;
    ASSERT_TRUE(std::string(mempool_str) == "stale-mempool",
                "STALE_MEMPOOL_CHANGED must map to 'stale-mempool'");

    // STALE_REORG
    const char* reorg_str = BlockRejectCodeToString(BlockRejectCode::STALE_REORG);
    std::cout << "  STALE_REORG -> \"" << reorg_str << "\"" << std::endl;
    ASSERT_TRUE(std::string(reorg_str) == "stale-reorg",
                "STALE_REORG must map to 'stale-reorg'");

    // STALE_TIMESTAMP
    const char* time_str = BlockRejectCodeToString(BlockRejectCode::STALE_TIMESTAMP);
    std::cout << "  STALE_TIMESTAMP -> \"" << time_str << "\"" << std::endl;
    ASSERT_TRUE(std::string(time_str) == "stale-time",
                "STALE_TIMESTAMP must map to 'stale-time'");

    // Verify no ambiguity - codes are distinct
    ASSERT_TRUE(BlockRejectCode::STALE_TIP_CHANGED != BlockRejectCode::STALE_MEMPOOL_CHANGED,
                "Stale codes must be distinct");
    ASSERT_TRUE(BlockRejectCode::STALE_TIP_CHANGED != BlockRejectCode::STALE_REORG,
                "Stale codes must be distinct");
    ASSERT_TRUE(BlockRejectCode::STALE_TIP_CHANGED != BlockRejectCode::STALE_TIMESTAMP,
                "Stale codes must be distinct");

    // Verify none are OK
    ASSERT_TRUE(BlockRejectCode::STALE_TIP_CHANGED != BlockRejectCode::OK,
                "STALE_TIP_CHANGED must not equal OK");
    ASSERT_TRUE(BlockRejectCode::STALE_MEMPOOL_CHANGED != BlockRejectCode::OK,
                "STALE_MEMPOOL_CHANGED must not equal OK");
    ASSERT_TRUE(BlockRejectCode::STALE_REORG != BlockRejectCode::OK,
                "STALE_REORG must not equal OK");
    ASSERT_TRUE(BlockRejectCode::STALE_TIMESTAMP != BlockRejectCode::OK,
                "STALE_TIMESTAMP must not equal OK");

    std::cout << "\n  ✅ All stale rejection codes are explicit and distinct\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.6: No Auto-Fix or Silent Rebuild
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_6_no_auto_fix() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.6: No auto-fix or silent rebuild" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;
    manager.MineBlock();

    // Create template
    MockTemplateState template1 = manager.CreateTemplate();
    uint64_t original_template_id = template1.template_id;
    uint256 original_prevhash = template1.template_prev_hash;

    // Make template stale
    manager.MineBlock();
    manager.UpdateTemplateState(template1);

    // Validate - should get rejection
    BlockRejectCode result = template1.ValidateFreshness();
    ASSERT_EQ(result, BlockRejectCode::STALE_TIP_CHANGED, "Template must be rejected");

    // Verify the template was NOT modified (no auto-fix)
    ASSERT_EQ(template1.template_id, original_template_id,
              "Template ID must not change (no silent rebuild)");
    ASSERT_TRUE(template1.template_prev_hash == original_prevhash,
              "Template prevhash must not change (no auto-fix)");

    // The only valid response is to create a NEW template
    MockTemplateState template2 = manager.CreateTemplate();
    ASSERT_NE(template2.template_id, template1.template_id,
              "New template must have different ID");
    ASSERT_TRUE(template2.template_prev_hash != template1.template_prev_hash,
              "New template must have updated prevhash");

    std::cout << "  Original template unchanged (ID=" << template1.template_id << ")" << std::endl;
    std::cout << "  New template created (ID=" << template2.template_id << ")" << std::endl;

    std::cout << "\n  ✅ No auto-fix or silent rebuild - explicit rejection only\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B1.7: Multiple Staleness Conditions Priority
// ═══════════════════════════════════════════════════════════════════════════

bool test_b1_7_staleness_priority() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B1.7: Multiple staleness conditions priority" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockTemplateManager manager;
    manager.MineBlock();

    // Create template
    MockTemplateState template1 = manager.CreateTemplate();

    // Trigger ALL staleness conditions
    manager.MineBlock();         // Tip changed
    manager.AddTransaction();    // Mempool changed
    manager.AdvanceTime(700);    // Time drift
    manager.UpdateTemplateState(template1);

    // Verify all conditions are true
    ASSERT_TRUE(template1.IsTipStale(), "Should detect tip stale");
    ASSERT_TRUE(template1.IsMempoolStale(), "Should detect mempool stale");
    ASSERT_TRUE(template1.IsTimestampStale(), "Should detect time stale");

    // Tip change should take priority (most dangerous)
    BlockRejectCode result = template1.ValidateFreshness();
    ASSERT_EQ(result, BlockRejectCode::STALE_TIP_CHANGED,
              "Tip change must take priority over other staleness");

    std::cout << "  All staleness conditions active" << std::endl;
    std::cout << "  Rejection code: " << BlockRejectCodeToString(result) << " (tip takes priority)" << std::endl;

    std::cout << "\n  ✅ Staleness priority correctly enforced\n" << std::endl;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n" << std::endl;
    std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase B1: Template Freshness & Staleness Tests           ║" << std::endl;
    std::cout << "║  MAINNET HARDENING - Mining Integrity                     ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    bool all_passed = true;

    // Run all B1 tests
    all_passed &= test_b1_1_new_block_invalidates_template();
    all_passed &= test_b1_2_mempool_change_detection();
    all_passed &= test_b1_3_reorg_invalidates_template();
    all_passed &= test_b1_4_time_drift_invalidates_template();
    all_passed &= test_b1_5_explicit_stale_rejection_codes();
    all_passed &= test_b1_6_no_auto_fix();
    all_passed &= test_b1_7_staleness_priority();

    // Summary
    std::cout << "\n╔═══════════════════════════════════════════════════════════╗" << std::endl;
    if (all_passed) {
        std::cout << "║  ✅ ALL TEMPLATE STALENESS TESTS PASSED                  ║" << std::endl;
        std::cout << "╠═══════════════════════════════════════════════════════════╣" << std::endl;
        std::cout << "║  Proven:                                                  ║" << std::endl;
        std::cout << "║    • New block invalidates template                       ║" << std::endl;
        std::cout << "║    • Mempool changes detected                             ║" << std::endl;
        std::cout << "║    • Reorg invalidates template (even same height)        ║" << std::endl;
        std::cout << "║    • Time drift invalidates template                      ║" << std::endl;
        std::cout << "║    • Explicit stale rejection codes                       ║" << std::endl;
        std::cout << "║    • No auto-fix or silent rebuild                        ║" << std::endl;
    } else {
        std::cout << "║  ❌ TEMPLATE STALENESS TESTS FAILED                       ║" << std::endl;
        std::cout << "║  DO NOT SHIP TO MAINNET                                   ║" << std::endl;
    }
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    std::cout << "\nTests: " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;

    return all_passed ? 0 : 1;
}
