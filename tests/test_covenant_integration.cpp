/**
 * Phase 29: Covenant Integration Test Suite
 *
 * Tests for covenant wallet, RPC, and mempool integration:
 * - CTV template creation and spending
 * - CSFS delegation and signing
 * - Contract state management
 * - Fee estimation
 * - Script analysis
 * - Mempool policy validation
 */

#include "wallet/covenant_wallet.h"
#include "mempool/covenant_policy.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "wallet/transaction.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

using namespace dinero;
using namespace dinero::wallet;
using namespace dinero::mempool;
using namespace dinero::consensus;

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

// Test database path
static std::string test_db_path = "/tmp/test_covenant_wallet.db";

// ============================================================================
// Helper Functions
// ============================================================================

void cleanup() {
    std::filesystem::remove(test_db_path);
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::string hex;
    hex.reserve(len * 2);
    const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex.push_back(digits[data[i] >> 4]);
        hex.push_back(digits[data[i] & 0xf]);
    }
    return hex;
}

std::string bytesToHex(const std::vector<uint8_t>& data) {
    return bytesToHex(data.data(), data.size());
}

std::string bytesToHex(const std::array<uint8_t, 32>& data) {
    return bytesToHex(data.data(), 32);
}

// ============================================================================
// Test 1: CovenantWallet Initialization
// ============================================================================

void test_wallet_initialization() {
    std::cout << "\n[Test 1] CovenantWallet Initialization\n";
    std::cout << "--------------------------------------------\n";

    cleanup();

    CovenantWallet wallet(test_db_path);

    assert(!wallet.isInitialized());
    tests_passed++;
    std::cout << "  [PASS] Wallet not initialized before init()\n";

    bool init_result = wallet.initialize();
    assert(init_result);
    tests_passed++;
    std::cout << "  [PASS] Wallet initialization succeeded\n";

    assert(wallet.isInitialized());
    tests_passed++;
    std::cout << "  [PASS] Wallet reports initialized state\n";

    // Verify database file exists
    assert(std::filesystem::exists(test_db_path));
    tests_passed++;
    std::cout << "  [PASS] Database file created\n";

    wallet.shutdown();
    assert(!wallet.isInitialized());
    tests_passed++;
    std::cout << "  [PASS] Wallet shutdown successful\n";
}

// ============================================================================
// Test 2: CTV Template Creation
// ============================================================================

void test_ctv_template_creation() {
    std::cout << "\n[Test 2] CTV Template Creation\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Create outputs for template
    std::vector<CTVTemplate::CommittedOutput> outputs;

    CTVTemplate::CommittedOutput out1;
    out1.value = 50000000;  // 0.5 DIN
    out1.script_pubkey = {0x00, 0x14};  // P2WPKH prefix
    for (int i = 0; i < 20; i++) out1.script_pubkey.push_back(0x42);
    out1.address = "din1qtest...";
    outputs.push_back(out1);

    CTVTemplate::CommittedOutput out2;
    out2.value = 49990000;  // Remaining minus fee
    out2.script_pubkey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) out2.script_pubkey.push_back(0x43);
    out2.address = "din1qchange...";
    outputs.push_back(out2);

    // Create template
    std::string template_id = wallet.createCTVTemplate(outputs, 0, "Test Vault Template");

    assert(!template_id.empty());
    tests_passed++;
    std::cout << "  [PASS] Template created with ID: " << template_id.substr(0, 8) << "...\n";

    // Retrieve template
    auto tmpl_opt = wallet.getCTVTemplate(template_id);
    assert(tmpl_opt.has_value());
    tests_passed++;
    std::cout << "  [PASS] Template retrieved successfully\n";

    auto& tmpl = *tmpl_opt;

    // Verify template fields
    assert(tmpl.template_id == template_id);
    assert(tmpl.outputs.size() == 2);
    assert(tmpl.locktime == 0);
    assert(!tmpl.is_spent);
    tests_passed++;
    std::cout << "  [PASS] Template fields correct\n";

    // Verify template hash is 32 bytes
    bool hash_nonzero = false;
    for (uint8_t b : tmpl.template_hash) {
        if (b != 0) { hash_nonzero = true; break; }
    }
    assert(hash_nonzero);
    tests_passed++;
    std::cout << "  [PASS] Template hash computed: " << bytesToHex(tmpl.template_hash).substr(0, 16) << "...\n";

    // Generate CTV script
    auto script = wallet.generateCTVScript(tmpl.template_hash);
    assert(script.size() == 34);  // OP_PUSH32 + 32 bytes + OP_CTV
    assert(script[0] == 0x20);  // OP_PUSH32
    assert(script[33] == static_cast<uint8_t>(OP_CHECKTEMPLATEVERIFY));
    tests_passed++;
    std::cout << "  [PASS] CTV script generated correctly\n";

    wallet.shutdown();
}

// ============================================================================
// Test 3: CTV Template List
// ============================================================================

void test_ctv_template_list() {
    std::cout << "\n[Test 3] CTV Template List\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Create multiple templates
    std::vector<CTVTemplate::CommittedOutput> outputs;
    CTVTemplate::CommittedOutput out;
    out.value = 10000000;
    out.script_pubkey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) out.script_pubkey.push_back(0x44);
    outputs.push_back(out);

    wallet.createCTVTemplate(outputs, 0, "Template 1");
    wallet.createCTVTemplate(outputs, 100, "Template 2");
    wallet.createCTVTemplate(outputs, 200, "Template 3");

    // List templates
    auto templates = wallet.listCTVTemplates(false);
    assert(templates.size() == 3);
    tests_passed++;
    std::cout << "  [PASS] Listed 3 templates\n";

    // Each should have different hash (due to different locktimes)
    assert(templates[0].template_hash != templates[1].template_hash);
    assert(templates[1].template_hash != templates[2].template_hash);
    tests_passed++;
    std::cout << "  [PASS] Each template has unique hash\n";

    wallet.shutdown();
}

// ============================================================================
// Test 4: CSFS Delegation
// ============================================================================

void test_csfs_delegation() {
    std::cout << "\n[Test 4] CSFS Delegation\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Create x-only pubkey (32 bytes)
    std::vector<uint8_t> pubkey(32, 0x42);

    // Create message to sign
    std::vector<uint8_t> message = {'H', 'e', 'l', 'l', 'o', ',', ' ', 'D', 'i', 'n', 'e', 'r', 'o', '!'};

    // Create delegation
    std::string delegation_id = wallet.createCSFSDelegation(
        pubkey, message, "oracle", 0);

    assert(!delegation_id.empty());
    tests_passed++;
    std::cout << "  [PASS] Delegation created: " << delegation_id.substr(0, 8) << "...\n";

    // Retrieve delegation
    auto del_opt = wallet.getCSFSDelegation(delegation_id);
    assert(del_opt.has_value());
    tests_passed++;
    std::cout << "  [PASS] Delegation retrieved\n";

    auto& del = *del_opt;
    assert(del.pubkey == pubkey);
    assert(del.message == message);
    assert(del.purpose == "oracle");
    assert(!del.is_signed);
    assert(!del.is_used);
    tests_passed++;
    std::cout << "  [PASS] Delegation fields correct\n";

    // Add signature (64 bytes)
    std::vector<uint8_t> signature(64, 0x55);
    bool sig_result = wallet.addCSFSSignature(delegation_id, signature);
    assert(sig_result);
    tests_passed++;
    std::cout << "  [PASS] Signature added to delegation\n";

    // Verify signature was stored
    del_opt = wallet.getCSFSDelegation(delegation_id);
    assert(del_opt->is_signed);
    assert(del_opt->signature == signature);
    tests_passed++;
    std::cout << "  [PASS] Delegation marked as signed\n";

    wallet.shutdown();
}

// ============================================================================
// Test 5: Contract Registration
// ============================================================================

void test_contract_registration() {
    std::cout << "\n[Test 5] Contract Registration\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Contract code (simplified)
    std::vector<uint8_t> code = {0xb3, 0xb4, 0xb5};  // Placeholder

    // Initial state data
    std::vector<uint8_t> initial_data = {0x01, 0x02, 0x03, 0x04};

    // Register contract
    std::string contract_id = wallet.registerContract(
        code, initial_data, "escrow", "Test Escrow Contract");

    assert(!contract_id.empty());
    tests_passed++;
    std::cout << "  [PASS] Contract registered: " << contract_id.substr(0, 8) << "...\n";

    wallet.shutdown();
}

// ============================================================================
// Test 6: Covenant UTXO Tracking
// ============================================================================

void test_covenant_utxo_tracking() {
    std::cout << "\n[Test 6] Covenant UTXO Tracking\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Create a CTV covenant UTXO
    CovenantUTXO utxo;
    utxo.txid = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    utxo.vout = 0;
    utxo.value = 100000000;  // 1 DIN
    utxo.script_pubkey = {0x00, 0x20};  // P2WSH
    for (int i = 0; i < 32; i++) utxo.script_pubkey.push_back(0x42);
    utxo.height = 100;
    utxo.is_spent = false;
    utxo.covenant_type = CovenantType::CTV;
    utxo.covenant_id = "test-template-id";

    std::array<uint8_t, 32> ctv_hash;
    ctv_hash.fill(0x99);
    utxo.ctv_hash = ctv_hash;

    utxo.requires_template_match = true;
    utxo.estimated_witness_size = 33;
    utxo.spending_tx_outputs = 2;

    // Add UTXO
    bool add_result = wallet.addCovenantUTXO(utxo);
    assert(add_result);
    tests_passed++;
    std::cout << "  [PASS] Covenant UTXO added\n";

    // List UTXOs
    auto utxos = wallet.listCovenantUTXOs(CovenantType::CTV, false);
    assert(utxos.size() == 1);
    tests_passed++;
    std::cout << "  [PASS] Listed 1 CTV UTXO\n";

    // Verify fields
    assert(utxos[0].txid == utxo.txid);
    assert(utxos[0].vout == utxo.vout);
    assert(utxos[0].value == utxo.value);
    assert(utxos[0].covenant_type == CovenantType::CTV);
    assert(utxos[0].requires_template_match);
    tests_passed++;
    std::cout << "  [PASS] UTXO fields match\n";

    // Mark as spent
    bool spend_result = wallet.markCovenantSpent(utxo.txid, utxo.vout, "spend-txid");
    assert(spend_result);
    tests_passed++;
    std::cout << "  [PASS] UTXO marked as spent\n";

    // Should not appear in unspent list
    utxos = wallet.listCovenantUTXOs(CovenantType::CTV, false);
    assert(utxos.empty());
    tests_passed++;
    std::cout << "  [PASS] Spent UTXO not in unspent list\n";

    // Should appear when including spent
    utxos = wallet.listCovenantUTXOs(CovenantType::CTV, true);
    assert(utxos.size() == 1);
    assert(utxos[0].is_spent);
    tests_passed++;
    std::cout << "  [PASS] Spent UTXO in full list\n";

    wallet.shutdown();
}

// ============================================================================
// Test 7: Script Analysis
// ============================================================================

void test_script_analysis() {
    std::cout << "\n[Test 7] Script Analysis\n";
    std::cout << "--------------------------------------------\n";

    // Test CTV script
    std::vector<uint8_t> ctv_script;
    ctv_script.push_back(0x20);  // OP_PUSH32
    for (int i = 0; i < 32; i++) ctv_script.push_back(0x42);
    ctv_script.push_back(static_cast<uint8_t>(OP_CHECKTEMPLATEVERIFY));

    auto analysis = CovenantWallet::analyzeScript(ctv_script);
    assert(analysis.type == CovenantType::CTV);
    assert(analysis.has_ctv);
    assert(!analysis.has_csfs);
    assert(!analysis.has_txhash);
    assert(!analysis.has_ccv);
    tests_passed++;
    std::cout << "  [PASS] CTV script detected\n";

    // Verify CTV hash was extracted
    assert(analysis.ctv_hash.has_value());
    tests_passed++;
    std::cout << "  [PASS] CTV hash extracted from script\n";

    // Test CSFS script
    std::vector<uint8_t> csfs_script;
    csfs_script.push_back(0x20);  // pubkey
    for (int i = 0; i < 32; i++) csfs_script.push_back(0x43);
    csfs_script.push_back(static_cast<uint8_t>(OP_CHECKSIGFROMSTACK));

    analysis = CovenantWallet::analyzeScript(csfs_script);
    assert(analysis.type == CovenantType::CSFS);
    assert(analysis.has_csfs);
    tests_passed++;
    std::cout << "  [PASS] CSFS script detected\n";

    // Test standard script (no covenants)
    std::vector<uint8_t> standard_script = {0x00, 0x14};
    for (int i = 0; i < 20; i++) standard_script.push_back(0x44);

    analysis = CovenantWallet::analyzeScript(standard_script);
    assert(analysis.type == CovenantType::NONE);
    assert(!analysis.has_ctv);
    assert(!analysis.has_csfs);
    tests_passed++;
    std::cout << "  [PASS] Standard script has no covenants\n";
}

// ============================================================================
// Test 8: Fee Estimation
// ============================================================================

void test_fee_estimation() {
    std::cout << "\n[Test 8] Fee Estimation\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Add a CTV UTXO for fee estimation
    CovenantUTXO utxo;
    utxo.txid = "feedbeef" + std::string(56, '0');
    utxo.vout = 0;
    utxo.value = 100000000;
    utxo.script_pubkey = {0x00, 0x20};
    for (int i = 0; i < 32; i++) utxo.script_pubkey.push_back(0x42);
    utxo.height = 100;
    utxo.covenant_type = CovenantType::CTV;
    utxo.spending_tx_outputs = 2;
    utxo.estimated_witness_size = 33;

    wallet.addCovenantUTXO(utxo);

    // Estimate fee at 1 una/vB
    auto fee_est = wallet.estimateCovenantFee(utxo.txid, utxo.vout, 1);

    assert(fee_est.fee_rate_una_vb == 1);
    assert(fee_est.input_count == 1);
    assert(fee_est.output_count == 2);
    assert(fee_est.tx_vsize > 0);
    assert(fee_est.total_fee > 0);
    tests_passed++;
    std::cout << "  [PASS] Fee estimated: " << fee_est.total_fee << " una for " << fee_est.tx_vsize << " vB\n";

    // Estimate at 10 sat/vB
    auto fee_est_10 = wallet.estimateCovenantFee(utxo.txid, utxo.vout, 10);
    assert(fee_est_10.total_fee > fee_est.total_fee);
    assert(fee_est_10.total_fee == fee_est.tx_vsize * 10);
    tests_passed++;
    std::cout << "  [PASS] Higher fee rate increases fee: " << fee_est_10.total_fee << " una\n";

    wallet.shutdown();
}

// ============================================================================
// Test 9: Covenant Statistics
// ============================================================================

void test_covenant_stats() {
    std::cout << "\n[Test 9] Covenant Statistics\n";
    std::cout << "--------------------------------------------\n";

    cleanup();
    CovenantWallet wallet(test_db_path);
    wallet.initialize();

    // Create some data - different templates must have different parameters
    std::vector<CTVTemplate::CommittedOutput> outputs1;
    CTVTemplate::CommittedOutput out1;
    out1.value = 10000000;
    out1.script_pubkey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) out1.script_pubkey.push_back(0x45);
    outputs1.push_back(out1);

    std::vector<CTVTemplate::CommittedOutput> outputs2;
    CTVTemplate::CommittedOutput out2;
    out2.value = 20000000;  // Different value
    out2.script_pubkey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) out2.script_pubkey.push_back(0x46);  // Different script
    outputs2.push_back(out2);

    wallet.createCTVTemplate(outputs1, 0, "Stats Test 1");
    wallet.createCTVTemplate(outputs2, 0, "Stats Test 2");

    std::vector<uint8_t> pubkey(32, 0x46);
    std::vector<uint8_t> message = {0x01, 0x02};
    wallet.createCSFSDelegation(pubkey, message, "test", 0);

    CovenantUTXO utxo;
    utxo.txid = std::string(64, 'a');
    utxo.vout = 0;
    utxo.value = 50000000;
    utxo.script_pubkey = {0x00};
    utxo.height = 1;
    utxo.covenant_type = CovenantType::CTV;
    wallet.addCovenantUTXO(utxo);

    // Get stats
    auto stats = wallet.getStats();

    assert(stats.total_ctv_templates == 2);
    tests_passed++;
    std::cout << "  [PASS] CTV templates: " << stats.total_ctv_templates << "\n";

    assert(stats.total_delegations == 1);
    tests_passed++;
    std::cout << "  [PASS] Delegations: " << stats.total_delegations << "\n";

    assert(stats.covenant_utxos == 1);
    tests_passed++;
    std::cout << "  [PASS] Covenant UTXOs: " << stats.covenant_utxos << "\n";

    assert(stats.covenant_value_locked == 50000000);
    tests_passed++;
    std::cout << "  [PASS] Value locked: " << stats.covenant_value_locked << " una\n";

    wallet.shutdown();
}

// ============================================================================
// Test 10: Mempool Policy Configuration
// ============================================================================

void test_mempool_policy() {
    std::cout << "\n[Test 10] Mempool Policy Configuration\n";
    std::cout << "--------------------------------------------\n";

    CovenantPolicyConfig config;

    // Verify defaults
    assert(config.max_ctv_outputs == 100);
    assert(config.max_csfs_message_size == 520);
    assert(config.max_covenant_depth == 10);
    assert(config.min_covenant_fee_rate == 1);
    tests_passed++;
    std::cout << "  [PASS] Default policy config values\n";

    // Create validator
    CovenantPolicyValidator validator(config);

    // Test reject code to string
    std::string err_str = CovenantPolicyValidator::rejectCodeToString(
        CovenantRejectCode::CTV_HASH_MISMATCH);
    assert(!err_str.empty());
    tests_passed++;
    std::cout << "  [PASS] Reject code converts to string\n";
}

// ============================================================================
// Test 11: CovenantTxBuilder
// ============================================================================

void test_covenant_tx_builder() {
    std::cout << "\n[Test 11] CovenantTxBuilder\n";
    std::cout << "--------------------------------------------\n";

    CovenantTxBuilder builder;

    builder.setVersion(2)
           .setLocktime(100)
           .addInput("abcd" + std::string(60, '0'), 0, 0xfffffffe)
           .addOutput(50000000, std::vector<uint8_t>{0x00, 0x14, 0x42, 0x42, 0x42, 0x42, 0x42,
                                 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
                                 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42});

    Transaction tx = builder.build();

    assert(tx.version == 2);
    assert(tx.lockTime == 100);
    assert(tx.vin.size() == 1);
    assert(tx.vout.size() == 1);
    tests_passed++;
    std::cout << "  [PASS] Transaction built with correct fields\n";

    // Validate builder
    bool valid = builder.validate();
    assert(valid);
    tests_passed++;
    std::cout << "  [PASS] Builder validation passed\n";

    // Empty builder should fail validation
    CovenantTxBuilder empty_builder;
    valid = empty_builder.validate();
    assert(!valid);
    assert(!empty_builder.getErrors().empty());
    tests_passed++;
    std::cout << "  [PASS] Empty builder fails validation\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "Phase 29: Covenant Integration Tests\n";
    std::cout << "============================================\n";

    try {
        test_wallet_initialization();
        test_ctv_template_creation();
        test_ctv_template_list();
        test_csfs_delegation();
        test_contract_registration();
        test_covenant_utxo_tracking();
        test_script_analysis();
        test_fee_estimation();
        test_covenant_stats();
        test_mempool_policy();
        test_covenant_tx_builder();
    } catch (const std::exception& e) {
        std::cout << "\n[FATAL] Test threw exception: " << e.what() << "\n";
        tests_failed++;
    }

    cleanup();

    std::cout << "\n============================================\n";
    std::cout << "Test Summary\n";
    std::cout << "============================================\n";
    std::cout << "  Passed: " << tests_passed << "\n";
    std::cout << "  Failed: " << tests_failed << "\n";

    if (tests_failed > 0) {
        std::cout << "\n[FAILED] Some tests failed!\n";
        return 1;
    }

    std::cout << "\n[SUCCESS] All covenant integration tests passed!\n";
    return 0;
}
