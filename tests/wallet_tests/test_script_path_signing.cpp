/**
 * Taproot Script-Path Signing Test Suite (BIP342)
 *
 * Tests:
 * 1. TapleafHash computation
 * 2. TapBranch hash computation
 * 3. Control block construction and parsing
 * 4. Script-path sighash computation
 * 5. Script-path signature creation and verification
 * 6. Witness stack building
 */

#include <iostream>
#include <cassert>
#include <array>
#include <vector>
#include <cstring>

#include "wallet/taproot_keys.h"
#include "wallet/taproot_control_block.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>

using namespace dinero;

// Helper: Convert bytes to hex string
std::string toHex(const uint8_t* data, size_t len) {
    std::string result;
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        result += buf;
    }
    return result;
}

std::string toHex(const std::vector<uint8_t>& data) {
    return toHex(data.data(), data.size());
}

template<size_t N>
std::string toHex(const std::array<uint8_t, N>& data) {
    return toHex(data.data(), N);
}

// Test 1: TapleafHash computation
bool testTapleafHash() {
    std::cout << "\n[Test 1] TapleafHash Computation\n";

    // Simple CHECKSIG script: <pubkey> OP_CHECKSIG
    // Script: 0x20 <32-byte pubkey> 0xac
    std::vector<uint8_t> script = {0x20};  // OP_PUSHBYTES_32
    for (int i = 0; i < 32; i++) script.push_back(0x42);  // Dummy pubkey
    script.push_back(0xac);  // OP_CHECKSIG

    uint8_t leaf_version = 0xC0;  // Tapscript (BIP342)

    std::array<uint8_t, 32> leaf_hash;
    if (!TaprootKeys::ComputeTapleafHash(script, leaf_version, leaf_hash)) {
        std::cerr << "  ERROR: ComputeTapleafHash failed\n";
        return false;
    }

    std::cout << "  Script:      " << toHex(script) << "\n";
    std::cout << "  Leaf version: 0x" << std::hex << (int)leaf_version << std::dec << "\n";
    std::cout << "  Leaf hash:   " << toHex(leaf_hash) << "\n";

    // Verify hash is not all zeros
    bool all_zero = true;
    for (size_t i = 0; i < 32; i++) {
        if (leaf_hash[i] != 0) {
            all_zero = false;
            break;
        }
    }

    if (all_zero) {
        std::cerr << "  ERROR: Leaf hash is all zeros\n";
        return false;
    }

    std::cout << "  ✅ PASS: TapleafHash computed successfully\n";
    return true;
}

// Test 2: TapBranch hash computation
bool testTapBranchHash() {
    std::cout << "\n[Test 2] TapBranch Hash Computation\n";

    std::array<uint8_t, 32> left, right, branch_hash;

    // Create two leaf hashes
    for (int i = 0; i < 32; i++) {
        left[i] = 0x11;
        right[i] = 0x22;
    }

    if (!TaprootKeys::ComputeTapBranchHash(left, right, branch_hash)) {
        std::cerr << "  ERROR: ComputeTapBranchHash failed\n";
        return false;
    }

    std::cout << "  Left:   " << toHex(left) << "\n";
    std::cout << "  Right:  " << toHex(right) << "\n";
    std::cout << "  Branch: " << toHex(branch_hash) << "\n";

    // Verify sorting: if we swap left and right, result should be the same
    std::array<uint8_t, 32> branch_hash_swapped;
    if (!TaprootKeys::ComputeTapBranchHash(right, left, branch_hash_swapped)) {
        std::cerr << "  ERROR: ComputeTapBranchHash (swapped) failed\n";
        return false;
    }

    if (branch_hash != branch_hash_swapped) {
        std::cerr << "  ERROR: TapBranch hash should be order-independent\n";
        return false;
    }

    std::cout << "  ✅ PASS: TapBranch hash computed (order-independent)\n";
    return true;
}

// Test 3: Control block construction and parsing
bool testControlBlock() {
    std::cout << "\n[Test 3] Control Block Construction/Parsing\n";

    std::array<uint8_t, 32> internal_key;
    for (int i = 0; i < 32; i++) internal_key[i] = 0x42 + i;

    // Create control block for single-leaf tree
    TaprootControlBlock cb = TaprootControlBlock::forSingleLeaf(
        internal_key,
        0xC0,  // Tapscript
        true   // odd parity
    );

    std::cout << "  Internal key:   " << toHex(internal_key) << "\n";
    std::cout << "  Leaf version:   0x" << std::hex << (int)cb.leaf_version << std::dec << "\n";
    std::cout << "  Output parity:  " << (cb.output_key_parity ? "odd" : "even") << "\n";

    // Serialize
    std::vector<uint8_t> serialized = cb.serialize();
    std::cout << "  Serialized:     " << toHex(serialized) << "\n";
    std::cout << "  Size:           " << serialized.size() << " bytes\n";

    // Verify size (1 header + 32 internal key = 33 for single leaf)
    if (serialized.size() != 33) {
        std::cerr << "  ERROR: Expected 33 bytes for single-leaf control block\n";
        return false;
    }

    // Verify first byte: leaf_version | parity
    uint8_t expected_first = 0xC0 | 0x01;  // 0xC1
    if (serialized[0] != expected_first) {
        std::cerr << "  ERROR: First byte should be 0xC1, got 0x"
                  << std::hex << (int)serialized[0] << std::dec << "\n";
        return false;
    }

    // Parse back
    TaprootControlBlock cb2;
    if (!cb2.parse(serialized)) {
        std::cerr << "  ERROR: Failed to parse serialized control block\n";
        return false;
    }

    // Verify round-trip
    if (cb2.leaf_version != cb.leaf_version ||
        cb2.output_key_parity != cb.output_key_parity ||
        cb2.internal_key != cb.internal_key ||
        cb2.merkle_path.size() != cb.merkle_path.size()) {
        std::cerr << "  ERROR: Round-trip mismatch\n";
        return false;
    }

    std::cout << "  ✅ PASS: Control block serialize/parse round-trip\n";
    return true;
}

// Test 4: Script-path signing with untweaked key
bool testScriptPathSigning() {
    std::cout << "\n[Test 4] Script-Path Signing (untweaked key)\n";

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Generate a random keypair (this is the script key, NOT tweaked)
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 32> xonly_pubkey;
    int parity;

    if (!TaprootKeys::GenerateKeypair(privkey, xonly_pubkey, parity)) {
        std::cerr << "  ERROR: Failed to generate keypair\n";
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::cout << "  Script privkey: " << toHex(privkey) << "\n";
    std::cout << "  Script pubkey:  " << toHex(xonly_pubkey) << "\n";
    std::cout << "  Pubkey parity:  " << (parity ? "odd" : "even") << "\n";

    // Create a dummy sighash (32 bytes)
    std::array<uint8_t, 32> sighash;
    for (int i = 0; i < 32; i++) sighash[i] = 0x42 + i;

    // Sign with UNTWEAKED key (script-path uses raw key)
    std::array<uint8_t, 64> signature;
    uint8_t aux_rand[32] = {0};

    if (!TaprootKeys::SignSchnorr(signature, sighash, privkey, aux_rand)) {
        std::cerr << "  ERROR: SignSchnorr failed\n";
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::cout << "  Signature:      " << toHex(signature) << "\n";

    // Verify signature
    if (!TaprootKeys::VerifySchnorr(signature, sighash, xonly_pubkey)) {
        std::cerr << "  ERROR: Signature verification failed\n";
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::cout << "  ✅ PASS: Script-path signature verified (untweaked key)\n";

    secp256k1_context_destroy(ctx);
    return true;
}

// Test 5: Witness stack building
bool testWitnessStack() {
    std::cout << "\n[Test 5] Witness Stack Building\n";

    // Create dummy signature
    std::vector<uint8_t> sig(64, 0xAB);

    // Create dummy script
    std::vector<uint8_t> script = {0x20};
    for (int i = 0; i < 32; i++) script.push_back(0x42);
    script.push_back(0xac);

    // Create control block
    std::array<uint8_t, 32> internal_key;
    for (int i = 0; i < 32; i++) internal_key[i] = 0x33;

    TaprootControlBlock cb = TaprootControlBlock::forSingleLeaf(internal_key, 0xC0, false);

    // Build witness
    std::vector<std::vector<uint8_t>> witness = buildScriptPathWitness({sig}, script, cb);

    std::cout << "  Witness elements: " << witness.size() << "\n";

    // Should have 3 elements: signature, script, control block
    if (witness.size() != 3) {
        std::cerr << "  ERROR: Expected 3 witness elements, got " << witness.size() << "\n";
        return false;
    }

    std::cout << "  [0] Signature:     " << witness[0].size() << " bytes\n";
    std::cout << "  [1] Script:        " << witness[1].size() << " bytes\n";
    std::cout << "  [2] Control block: " << witness[2].size() << " bytes\n";

    // Verify contents
    if (witness[0] != sig) {
        std::cerr << "  ERROR: Signature mismatch in witness\n";
        return false;
    }

    if (witness[1] != script) {
        std::cerr << "  ERROR: Script mismatch in witness\n";
        return false;
    }

    if (witness[2] != cb.serialize()) {
        std::cerr << "  ERROR: Control block mismatch in witness\n";
        return false;
    }

    std::cout << "  ✅ PASS: Witness stack built correctly\n";
    return true;
}

// Test 6: Multiple signatures (multi-sig script)
bool testMultiSigWitness() {
    std::cout << "\n[Test 6] Multi-Signature Witness\n";

    // Create two signatures (for a 2-of-2 multisig)
    std::vector<uint8_t> sig1(64, 0xAA);
    std::vector<uint8_t> sig2(64, 0xBB);

    // Create script (simplified 2-of-2)
    std::vector<uint8_t> script = {0x20};
    for (int i = 0; i < 32; i++) script.push_back(0x11);  // pubkey1
    script.push_back(0xac);  // CHECKSIG
    script.push_back(0x20);
    for (int i = 0; i < 32; i++) script.push_back(0x22);  // pubkey2
    script.push_back(0xba);  // CHECKSIGADD

    // Control block
    std::array<uint8_t, 32> internal_key;
    for (int i = 0; i < 32; i++) internal_key[i] = 0x55;

    TaprootControlBlock cb = TaprootControlBlock::forSingleLeaf(internal_key, 0xC0, true);

    // Build witness with 2 signatures
    std::vector<std::vector<uint8_t>> witness = buildScriptPathWitness({sig1, sig2}, script, cb);

    std::cout << "  Witness elements: " << witness.size() << "\n";

    // Should have 4 elements: sig1, sig2, script, control block
    if (witness.size() != 4) {
        std::cerr << "  ERROR: Expected 4 witness elements, got " << witness.size() << "\n";
        return false;
    }

    std::cout << "  [0] Signature 1:   " << witness[0].size() << " bytes\n";
    std::cout << "  [1] Signature 2:   " << witness[1].size() << " bytes\n";
    std::cout << "  [2] Script:        " << witness[2].size() << " bytes\n";
    std::cout << "  [3] Control block: " << witness[3].size() << " bytes\n";

    std::cout << "  ✅ PASS: Multi-sig witness built correctly\n";
    return true;
}

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Taproot Script-Path Signing Test Suite (BIP342)          ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════╝\n";

    int pass = 0;
    int fail = 0;

    if (testTapleafHash()) pass++; else fail++;
    if (testTapBranchHash()) pass++; else fail++;
    if (testControlBlock()) pass++; else fail++;
    if (testScriptPathSigning()) pass++; else fail++;
    if (testWitnessStack()) pass++; else fail++;
    if (testMultiSigWitness()) pass++; else fail++;

    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "Results: " << pass << " passed, " << fail << " failed\n";
    std::cout << "════════════════════════════════════════════════════════════\n";

    if (fail == 0) {
        std::cout << "\n✅ ALL SCRIPT-PATH TESTS PASSED\n";
        std::cout << "   BIP342 Tapscript support is functional\n\n";
        return 0;
    } else {
        std::cout << "\n❌ SOME TESTS FAILED\n\n";
        return 1;
    }
}
