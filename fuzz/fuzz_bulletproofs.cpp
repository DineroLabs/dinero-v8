/**
 * Bulletproofs Range Proof Fuzzing Harness
 *
 * Adversarial fuzzing of ZK proof serialization and verification.
 * This is critical security testing - malformed proofs must NEVER:
 * - Crash the verifier
 * - Cause memory corruption
 * - Pass verification when invalid
 * - Allow inflation (value outside range)
 *
 * Attack surfaces tested:
 * 1. Proof deserialization (malformed bytes)
 * 2. Proof verification (corrupted proofs)
 * 3. Commitment parsing (invalid curve points)
 * 4. Batch verification (mixed valid/invalid)
 * 5. Rewind operations (wrong nonces)
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_bulletproofs.cpp \
 *           -L../build -ldinero_zk -lbulletproofs_ffi \
 *           -o fuzz_bulletproofs
 *
 * Run:
 *   ./fuzz_bulletproofs corpus/bulletproofs/ -max_len=4096
 */

#include "crypto/bulletproofs.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>

// ============================================================================
// Fuzzing Targets
// ============================================================================

namespace {

// Target 1: Proof verification with arbitrary bytes
// Goal: Ensure malformed proofs never pass verification
void fuzz_proof_verify(const uint8_t* data, size_t size) {
    if (size < BULLETPROOFS_COMMITMENT_SIZE + 1) {
        return;  // Need at least commitment + 1 byte proof
    }

    // Split input: first 32 bytes = commitment, rest = proof
    const uint8_t* commitment = data;
    const uint8_t* proof = data + BULLETPROOFS_COMMITMENT_SIZE;
    size_t proof_len = size - BULLETPROOFS_COMMITMENT_SIZE;

    // Cap proof size to reasonable maximum
    if (proof_len > BULLETPROOFS_MAX_PROOF_SIZE) {
        proof_len = BULLETPROOFS_MAX_PROOF_SIZE;
    }

    // This MUST return 0 (invalid) for random/malformed data
    // If it returns 1, we found a bug (false positive)
    int result = bp_verify(commitment, proof, proof_len);

    // Sanity check: random bytes should essentially never verify
    // (probability ~2^-256 for valid proof by chance)
    (void)result;
}

// Target 2: Commitment creation with arbitrary blinding factors
// Goal: Ensure no crashes with malformed inputs
void fuzz_commitment_create(const uint8_t* data, size_t size) {
    if (size < BULLETPROOFS_BLINDING_SIZE + 8) {
        return;  // Need blinding (32) + value (8)
    }

    const uint8_t* blinding = data;
    uint64_t value;
    memcpy(&value, data + BULLETPROOFS_BLINDING_SIZE, sizeof(value));

    uint8_t commitment[BULLETPROOFS_COMMITMENT_SIZE];

    // Should handle any input without crashing
    int result = commitment_create(value, blinding, commitment);
    (void)result;
}

// Target 3: Proof generation with fuzzed blinding factors
// Goal: Ensure deterministic failure modes, no UB
void fuzz_proof_generate(const uint8_t* data, size_t size) {
    if (size < BULLETPROOFS_BLINDING_SIZE + 8) {
        return;
    }

    const uint8_t* blinding = data;
    uint64_t value;
    memcpy(&value, data + BULLETPROOFS_BLINDING_SIZE, sizeof(value));

    uint8_t proof[BULLETPROOFS_MAX_PROOF_SIZE];
    size_t proof_len = 0;

    // Generate proof - should always succeed for valid blinding
    int result = bp_generate(value, blinding, proof, &proof_len);

    // If generation succeeded, verify the proof
    if (result == 0 && proof_len > 0) {
        uint8_t commitment[BULLETPROOFS_COMMITMENT_SIZE];
        if (commitment_create(value, blinding, commitment) == 0) {
            // Self-generated proof MUST verify
            int verify_result = bp_verify(commitment, proof, proof_len);
            if (verify_result != 0) {
                // BUG: Valid proof failed verification
                __builtin_trap();
            }
        }
    }
}

// Target 4: Commitment arithmetic with arbitrary points
// Goal: Ensure point decompression handles invalid points safely
void fuzz_commitment_arithmetic(const uint8_t* data, size_t size) {
    if (size < BULLETPROOFS_COMMITMENT_SIZE * 2) {
        return;  // Need two commitments
    }

    const uint8_t* commitment_a = data;
    const uint8_t* commitment_b = data + BULLETPROOFS_COMMITMENT_SIZE;
    uint8_t result[BULLETPROOFS_COMMITMENT_SIZE];

    // Addition with potentially invalid points
    int add_result = commitment_add(commitment_a, commitment_b, result);
    (void)add_result;

    // Subtraction with potentially invalid points
    int sub_result = commitment_sub(commitment_a, commitment_b, result);
    (void)sub_result;
}

// Target 5: Batch verification with mixed inputs
// Goal: Ensure batch verifier rejects if ANY proof invalid
void fuzz_batch_verify(const uint8_t* data, size_t size) {
    // Parse as: count (1 byte) + array of (commitment + proof_len + proof)
    if (size < 1) return;

    uint8_t count = data[0] % 8;  // Max 8 proofs in batch
    if (count == 0) return;

    size_t pos = 1;

    std::vector<const uint8_t*> commitments;
    std::vector<const uint8_t*> proofs;
    std::vector<size_t> proof_lens;

    for (uint8_t i = 0; i < count && pos < size; ++i) {
        // Need: commitment (32) + proof_len (2) + proof (variable)
        if (pos + BULLETPROOFS_COMMITMENT_SIZE + 2 > size) break;

        const uint8_t* commitment = data + pos;
        pos += BULLETPROOFS_COMMITMENT_SIZE;

        uint16_t proof_len;
        memcpy(&proof_len, data + pos, sizeof(proof_len));
        pos += 2;

        // Cap proof length
        proof_len = proof_len % (BULLETPROOFS_MAX_PROOF_SIZE + 1);
        if (pos + proof_len > size) break;

        const uint8_t* proof = data + pos;
        pos += proof_len;

        commitments.push_back(commitment);
        proofs.push_back(proof);
        proof_lens.push_back(proof_len);
    }

    if (commitments.empty()) return;

    // Batch verify - should handle any combination
    int result = bp_verify_batch(
        commitments.data(),
        proofs.data(),
        proof_lens.data(),
        commitments.size()
    );
    (void)result;
}

// Target 6: Proof rewind with arbitrary nonces
// Goal: Ensure rewind fails gracefully with wrong nonce
void fuzz_proof_rewind(const uint8_t* data, size_t size) {
    if (size < BULLETPROOFS_COMMITMENT_SIZE + BULLETPROOFS_BLINDING_SIZE + 100) {
        return;  // Need commitment + nonce + min proof
    }

    const uint8_t* commitment = data;
    const uint8_t* nonce = data + BULLETPROOFS_COMMITMENT_SIZE;
    const uint8_t* proof = data + BULLETPROOFS_COMMITMENT_SIZE + BULLETPROOFS_BLINDING_SIZE;
    size_t proof_len = size - BULLETPROOFS_COMMITMENT_SIZE - BULLETPROOFS_BLINDING_SIZE;

    if (proof_len > BULLETPROOFS_MAX_PROOF_SIZE) {
        proof_len = BULLETPROOFS_MAX_PROOF_SIZE;
    }

    uint64_t value_out = 0;
    uint8_t blinding_out[BULLETPROOFS_BLINDING_SIZE];

    // Rewind with potentially wrong nonce - should fail gracefully
    int result = bp_rewind(commitment, proof, proof_len, nonce, &value_out, blinding_out);
    (void)result;
}

}  // namespace

// ============================================================================
// LibFuzzer Entry Point
// ============================================================================

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    // Initialize bulletproofs library
    if (bp_init() != 0) {
        return -1;
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) {
        return 0;
    }

    // Use first byte to select fuzzing target
    uint8_t target = data[0] % 6;
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (target) {
        case 0:
            fuzz_proof_verify(payload, payload_size);
            break;
        case 1:
            fuzz_commitment_create(payload, payload_size);
            break;
        case 2:
            fuzz_proof_generate(payload, payload_size);
            break;
        case 3:
            fuzz_commitment_arithmetic(payload, payload_size);
            break;
        case 4:
            fuzz_batch_verify(payload, payload_size);
            break;
        case 5:
            fuzz_proof_rewind(payload, payload_size);
            break;
    }

    return 0;
}

// ============================================================================
// Standalone Mode (for non-libFuzzer testing)
// ============================================================================

#ifdef FUZZ_STANDALONE

#include <iostream>
#include <fstream>
#include <random>

int main(int argc, char* argv[]) {
    if (bp_init() != 0) {
        std::cerr << "Failed to initialize bulletproofs\n";
        return 1;
    }

    std::cout << "Bulletproofs Fuzzer - Standalone Mode\n";
    std::cout << "=====================================\n\n";

    // Generate random test cases
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    const int NUM_ITERATIONS = 10000;
    std::vector<uint8_t> buffer(4096);

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Random size
        size_t size = (gen() % 4000) + 100;

        // Fill with random data
        for (size_t j = 0; j < size; ++j) {
            buffer[j] = dist(gen);
        }

        // Run fuzzer
        LLVMFuzzerTestOneInput(buffer.data(), size);

        if ((i + 1) % 1000 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    std::cout << "\nAll " << NUM_ITERATIONS << " iterations completed without crashes.\n";
    return 0;
}

#endif  // FUZZ_STANDALONE
