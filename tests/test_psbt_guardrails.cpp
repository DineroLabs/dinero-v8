#include "wallet/psbt_taproot_validator.h"
#include "dinero/core/wallet/psbt.h"
#include <iostream>
#include <cassert>

using namespace dinero;
using namespace din;

int main() {
    std::cout << "=== PSBT Taproot Guardrails Test ===" << std::endl << std::endl;

    // Test 1: BIP84 wallet - no restrictions
    {
        std::cout << "Test 1: BIP84 wallet with Taproot input (should pass)" << std::endl;

        PsbtInput input;

        // Add witness UTXO with Taproot scriptPubKey
        // Format: <amount:8><scriptPubKey>
        // Taproot: OP_1 (0x51) + 0x20 (push 32 bytes) + 32-byte pubkey
        std::vector<uint8_t> witness_utxo;
        // Amount: 100000 una (little-endian)
        witness_utxo.push_back(0xa0); witness_utxo.push_back(0x86);
        witness_utxo.push_back(0x01); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        // Taproot scriptPubKey: OP_1 + 0x20 + 32-byte pubkey
        witness_utxo.push_back(0x51); // OP_1
        witness_utxo.push_back(0x20); // Push 32 bytes
        for (int i = 0; i < 32; i++) {
            witness_utxo.push_back(0xaa); // Dummy pubkey
        }

        PsbtMapKV witness_kv;
        witness_kv.key = {0x01}; // WITNESS_UTXO type
        witness_kv.value = witness_utxo;
        input.kv.push_back(witness_kv);

        // Add TAP_MERKLE_ROOT (indicates script tree - script-path spending)
        PsbtMapKV merkle_kv;
        merkle_kv.key = {0x18}; // TAP_MERKLE_ROOT type
        merkle_kv.value = std::vector<uint8_t>(32, 0x55); // Non-zero merkle root
        input.kv.push_back(merkle_kv);

        auto result = PSBTTaprootValidator::validateInput(input.kv, "bip84");

        std::cout << "  Valid: " << (result.valid ? "YES" : "NO") << std::endl;
        std::cout << "  Expected: YES (BIP84 has no Taproot restrictions)" << std::endl;
        assert(result.valid && "BIP84 wallet should allow script-path spending");
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }

    // Test 2: BIP86 wallet with key-path only (should pass)
    {
        std::cout << "Test 2: BIP86 wallet with key-path only (should pass)" << std::endl;

        PsbtInput input;

        // Add witness UTXO with Taproot scriptPubKey
        std::vector<uint8_t> witness_utxo;
        witness_utxo.push_back(0xa0); witness_utxo.push_back(0x86);
        witness_utxo.push_back(0x01); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x51); // OP_1
        witness_utxo.push_back(0x20); // Push 32 bytes
        for (int i = 0; i < 32; i++) {
            witness_utxo.push_back(0xaa);
        }

        PsbtMapKV witness_kv;
        witness_kv.key = {0x01};
        witness_kv.value = witness_utxo;
        input.kv.push_back(witness_kv);

        // Add TAP_KEY_SIG (key-path signature - allowed for BIP86)
        PsbtMapKV key_sig_kv;
        key_sig_kv.key = {0x13}; // TAP_KEY_SIG type
        key_sig_kv.value = std::vector<uint8_t>(64, 0xbb); // Dummy signature
        input.kv.push_back(key_sig_kv);

        auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

        std::cout << "  Valid: " << (result.valid ? "YES" : "NO") << std::endl;
        std::cout << "  Expected: YES (key-path spending is allowed)" << std::endl;
        assert(result.valid && "BIP86 wallet should allow key-path spending");
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }

    // Test 3: BIP86 wallet with TAP_SCRIPT_SIG (should fail)
    {
        std::cout << "Test 3: BIP86 wallet with TAP_SCRIPT_SIG (should reject)" << std::endl;

        PsbtInput input;

        // Add witness UTXO
        std::vector<uint8_t> witness_utxo;
        witness_utxo.push_back(0xa0); witness_utxo.push_back(0x86);
        witness_utxo.push_back(0x01); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x51); // OP_1
        witness_utxo.push_back(0x20); // Push 32 bytes
        for (int i = 0; i < 32; i++) {
            witness_utxo.push_back(0xaa);
        }

        PsbtMapKV witness_kv;
        witness_kv.key = {0x01};
        witness_kv.value = witness_utxo;
        input.kv.push_back(witness_kv);

        // Add TAP_SCRIPT_SIG (script-path signature - forbidden for BIP86)
        PsbtMapKV script_sig_kv;
        script_sig_kv.key = {0x14}; // TAP_SCRIPT_SIG type
        script_sig_kv.value = std::vector<uint8_t>(64, 0xcc);
        input.kv.push_back(script_sig_kv);

        auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

        std::cout << "  Valid: " << (result.valid ? "YES" : "NO") << std::endl;
        std::cout << "  Expected: NO (script-path spending forbidden for BIP86)" << std::endl;
        std::cout << "  Error: " << (result.error.empty() ? "(none)" : result.error.substr(0, 50) + "...") << std::endl;
        assert(!result.valid && "BIP86 wallet should reject TAP_SCRIPT_SIG");
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }

    // Test 4: BIP86 wallet with TAP_LEAF_SCRIPT (should fail)
    {
        std::cout << "Test 4: BIP86 wallet with TAP_LEAF_SCRIPT (should reject)" << std::endl;

        PsbtInput input;

        // Add witness UTXO
        std::vector<uint8_t> witness_utxo;
        witness_utxo.push_back(0xa0); witness_utxo.push_back(0x86);
        witness_utxo.push_back(0x01); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x51);
        witness_utxo.push_back(0x20);
        for (int i = 0; i < 32; i++) {
            witness_utxo.push_back(0xaa);
        }

        PsbtMapKV witness_kv;
        witness_kv.key = {0x01};
        witness_kv.value = witness_utxo;
        input.kv.push_back(witness_kv);

        // Add TAP_LEAF_SCRIPT (script tree - forbidden for BIP86)
        PsbtMapKV leaf_kv;
        leaf_kv.key = {0x15}; // TAP_LEAF_SCRIPT type
        leaf_kv.value = std::vector<uint8_t>(100, 0xdd);
        input.kv.push_back(leaf_kv);

        auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

        std::cout << "  Valid: " << (result.valid ? "YES" : "NO") << std::endl;
        std::cout << "  Expected: NO (script tree forbidden for BIP86)" << std::endl;
        assert(!result.valid && "BIP86 wallet should reject TAP_LEAF_SCRIPT");
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }

    // Test 5: BIP86 wallet with non-zero TAP_MERKLE_ROOT (should fail)
    {
        std::cout << "Test 5: BIP86 wallet with TAP_MERKLE_ROOT (should reject)" << std::endl;

        PsbtInput input;

        // Add witness UTXO
        std::vector<uint8_t> witness_utxo;
        witness_utxo.push_back(0xa0); witness_utxo.push_back(0x86);
        witness_utxo.push_back(0x01); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x00); witness_utxo.push_back(0x00);
        witness_utxo.push_back(0x51);
        witness_utxo.push_back(0x20);
        for (int i = 0; i < 32; i++) {
            witness_utxo.push_back(0xaa);
        }

        PsbtMapKV witness_kv;
        witness_kv.key = {0x01};
        witness_kv.value = witness_utxo;
        input.kv.push_back(witness_kv);

        // Add non-zero TAP_MERKLE_ROOT (indicates script tree)
        PsbtMapKV merkle_kv;
        merkle_kv.key = {0x18}; // TAP_MERKLE_ROOT type
        merkle_kv.value = std::vector<uint8_t>(32, 0xee); // Non-zero
        input.kv.push_back(merkle_kv);

        auto result = PSBTTaprootValidator::validateInput(input.kv, "bip86");

        std::cout << "  Valid: " << (result.valid ? "YES" : "NO") << std::endl;
        std::cout << "  Expected: NO (merkle root indicates script tree)" << std::endl;
        assert(!result.valid && "BIP86 wallet should reject non-zero TAP_MERKLE_ROOT");
        std::cout << "  ✅ PASS" << std::endl << std::endl;
    }

    std::cout << "==================================" << std::endl;
    std::cout << "✅ All PSBT guardrail tests passed!" << std::endl;
    std::cout << "==================================" << std::endl;

    return 0;
}
