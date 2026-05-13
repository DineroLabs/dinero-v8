#include "wallet/psbt_taproot_validator.h"
#include "wallet/psbt_signer.h"
#include "wallet/wallet_manager.h"
#include "dinero/core/wallet/psbt.h"
#include <iostream>
#include <cassert>

using namespace dinero;
using namespace din;

// Helper: Create a Taproot witness UTXO
std::vector<uint8_t> createTaprootWitnessUTXO(uint64_t amount) {
    std::vector<uint8_t> witness_utxo;

    // Amount (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        witness_utxo.push_back((amount >> (i * 8)) & 0xFF);
    }

    // Taproot scriptPubKey: OP_1 (0x51) + 0x20 (push 32 bytes) + 32-byte pubkey
    witness_utxo.push_back(0x51); // OP_1
    witness_utxo.push_back(0x20); // Push 32 bytes
    for (int i = 0; i < 32; i++) {
        witness_utxo.push_back(0xaa); // Dummy pubkey
    }

    return witness_utxo;
}

// Helper: Create PSBT input with witness UTXO
PsbtInput createBasicInput(uint64_t amount) {
    PsbtInput input;

    std::vector<uint8_t> witness_utxo = createTaprootWitnessUTXO(amount);

    PsbtMapKV witness_kv;
    witness_kv.key = {0x01}; // PSBT_IN_WITNESS_UTXO
    witness_kv.value = witness_utxo;
    input.kv.push_back(witness_kv);

    return input;
}

// Test 1: BIP86 wallet rejects script-path spending PSBT
void test_bip86_rejects_script_path() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 1: BIP86 Wallet Rejects Script-Path Spending" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add TAP_SCRIPT_SIG - indicates script-path spending
    PsbtMapKV script_sig_kv;
    script_sig_kv.key = {0x14}; // PSBT_IN_TAP_SCRIPT_SIG
    script_sig_kv.value = std::vector<uint8_t>(64, 0xcc); // Dummy signature
    input.kv.push_back(script_sig_kv);

    // Validate with BIP86 policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

    std::cout << "  Input contains: TAP_SCRIPT_SIG (script-path signature)" << std::endl;
    std::cout << "  Wallet policy: bip86" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ❌" : "NO ✅") << std::endl;
    std::cout << "  Error: " << result.error << std::endl;

    assert(!result.valid && "BIP86 should reject TAP_SCRIPT_SIG");
    assert(result.error.find("BIP86 Policy Violation") != std::string::npos);

    std::cout << "  ✅ PASS: BIP86 wallet correctly rejected script-path spending" << std::endl;
    std::cout << std::endl;
}

// Test 2: BIP86 wallet accepts key-path spending PSBT
void test_bip86_accepts_key_path() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 2: BIP86 Wallet Accepts Key-Path Spending" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add TAP_KEY_SIG - indicates key-path spending (allowed for BIP86)
    PsbtMapKV key_sig_kv;
    key_sig_kv.key = {0x13}; // PSBT_IN_TAP_KEY_SIG
    key_sig_kv.value = std::vector<uint8_t>(64, 0xbb); // Dummy Schnorr signature
    input.kv.push_back(key_sig_kv);

    // Validate with BIP86 policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

    std::cout << "  Input contains: TAP_KEY_SIG (key-path signature)" << std::endl;
    std::cout << "  Wallet policy: bip86" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ✅" : "NO ❌") << std::endl;

    assert(result.valid && "BIP86 should accept TAP_KEY_SIG");
    assert(result.error.empty());

    std::cout << "  ✅ PASS: BIP86 wallet correctly accepted key-path spending" << std::endl;
    std::cout << std::endl;
}

// Test 3: BIP86 wallet rejects TAP_LEAF_SCRIPT
void test_bip86_rejects_leaf_script() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 3: BIP86 Wallet Rejects TAP_LEAF_SCRIPT" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add TAP_LEAF_SCRIPT - indicates script tree (forbidden for BIP86)
    PsbtMapKV leaf_kv;
    leaf_kv.key = {0x15}; // PSBT_IN_TAP_LEAF_SCRIPT
    leaf_kv.value = std::vector<uint8_t>(100, 0xdd); // Dummy script
    input.kv.push_back(leaf_kv);

    // Validate with BIP86 policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

    std::cout << "  Input contains: TAP_LEAF_SCRIPT (script tree)" << std::endl;
    std::cout << "  Wallet policy: bip86" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ❌" : "NO ✅") << std::endl;
    std::cout << "  Error: " << result.error << std::endl;

    assert(!result.valid && "BIP86 should reject TAP_LEAF_SCRIPT");

    std::cout << "  ✅ PASS: BIP86 wallet correctly rejected script tree" << std::endl;
    std::cout << std::endl;
}

// Test 4: BIP86 wallet rejects non-zero TAP_MERKLE_ROOT
void test_bip86_rejects_merkle_root() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 4: BIP86 Wallet Rejects Non-Zero TAP_MERKLE_ROOT" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add non-zero TAP_MERKLE_ROOT - indicates script tree
    PsbtMapKV merkle_kv;
    merkle_kv.key = {0x18}; // PSBT_IN_TAP_MERKLE_ROOT
    merkle_kv.value = std::vector<uint8_t>(32, 0xee); // Non-zero merkle root
    input.kv.push_back(merkle_kv);

    // Validate with BIP86 policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

    std::cout << "  Input contains: TAP_MERKLE_ROOT (non-zero)" << std::endl;
    std::cout << "  Wallet policy: bip86" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ❌" : "NO ✅") << std::endl;
    std::cout << "  Error: " << result.error << std::endl;

    assert(!result.valid && "BIP86 should reject non-zero TAP_MERKLE_ROOT");

    std::cout << "  ✅ PASS: BIP86 wallet correctly rejected merkle root" << std::endl;
    std::cout << std::endl;
}

// Test 5: BIP84 wallet has no Taproot restrictions
void test_bip84_allows_script_path() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 5: BIP84 Wallet Has No Taproot Restrictions" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add all script-path fields (should be allowed for BIP84)
    PsbtMapKV script_sig_kv;
    script_sig_kv.key = {0x14}; // TAP_SCRIPT_SIG
    script_sig_kv.value = std::vector<uint8_t>(64, 0xcc);
    input.kv.push_back(script_sig_kv);

    PsbtMapKV leaf_kv;
    leaf_kv.key = {0x15}; // TAP_LEAF_SCRIPT
    leaf_kv.value = std::vector<uint8_t>(100, 0xdd);
    input.kv.push_back(leaf_kv);

    PsbtMapKV merkle_kv;
    merkle_kv.key = {0x18}; // TAP_MERKLE_ROOT
    merkle_kv.value = std::vector<uint8_t>(32, 0xee);
    input.kv.push_back(merkle_kv);

    // Validate with BIP84 policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "bip84");

    std::cout << "  Input contains: TAP_SCRIPT_SIG, TAP_LEAF_SCRIPT, TAP_MERKLE_ROOT" << std::endl;
    std::cout << "  Wallet policy: bip84" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ✅" : "NO ❌") << std::endl;

    assert(result.valid && "BIP84 should allow all script-path fields");

    std::cout << "  ✅ PASS: BIP84 wallet has no Taproot restrictions" << std::endl;
    std::cout << std::endl;
}

// Test 6: Empty policy (unknown/legacy) has no restrictions
void test_empty_policy_no_restrictions() {
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Test 6: Empty Policy (Legacy Wallet) Has No Restrictions" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════════════" << std::endl;

    PsbtInput input = createBasicInput(100000);

    // Add script-path fields
    PsbtMapKV script_sig_kv;
    script_sig_kv.key = {0x14};
    script_sig_kv.value = std::vector<uint8_t>(64, 0xcc);
    input.kv.push_back(script_sig_kv);

    // Validate with empty policy
    auto result = PSBTTaprootValidator::validateInput(input.kv, "");

    std::cout << "  Input contains: TAP_SCRIPT_SIG" << std::endl;
    std::cout << "  Wallet policy: (empty/legacy)" << std::endl;
    std::cout << "  Valid: " << (result.valid ? "YES ✅" : "NO ❌") << std::endl;

    assert(result.valid && "Empty policy should allow script-path");

    std::cout << "  ✅ PASS: Legacy wallet has no restrictions" << std::endl;
    std::cout << std::endl;
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║        PSBT BIP86 Guardrails - Integration Test Suite               ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    try {
        // Run all tests
        test_bip86_rejects_script_path();
        test_bip86_accepts_key_path();
        test_bip86_rejects_leaf_script();
        test_bip86_rejects_merkle_root();
        test_bip84_allows_script_path();
        test_empty_policy_no_restrictions();

        std::cout << "╔══════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║              ✅ All PSBT BIP86 Guardrail Tests Passed!               ║" << std::endl;
        std::cout << "╚══════════════════════════════════════════════════════════════════════╝" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
