/**
 * Ring Signature Fuzz Harness: CLSAG Signature Deserialization
 *
 * Feeds random/malformed bytes to CLSAGSignature::Deserialize to ensure:
 * - No crashes, no hangs, no OOB reads on any input
 * - If deserialization succeeds, the result is re-serializable
 * - Round-trip: Deserialize(Serialize(sig)) == sig
 * - Invalid ring sizes (0, 1, >64) are rejected
 * - Truncated data is rejected gracefully
 *
 * Attack surfaces:
 * 1. Ring size byte: controls allocation (must be bounded)
 * 2. Scalar fields (c0, s[]): 32-byte chunks, OOB if size wrong
 * 3. D point: 33-byte compressed point at the tail
 * 4. Size mismatches: data too short or too long for claimed ring size
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include -I../src fuzz_clsag_deserialize.cpp \
 *           -o fuzz_clsag_deserialize
 *
 * Run:
 *   ./fuzz_clsag_deserialize corpus/clsag_deser/ -max_len=4096
 */

#include "zk/clsag.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace dinero::zk;

namespace {

// Target 1: Direct deserialization of arbitrary bytes
void fuzz_deserialize(const uint8_t* data, size_t size) {
    std::vector<uint8_t> input(data, data + size);
    CLSAGSignature sig;
    bool ok = CLSAGSignature::Deserialize(input, sig);

    if (ok) {
        // If deserialization succeeded, the signature must be re-serializable.
        auto reserialized = sig.Serialize();

        // Re-serialized form must not be empty (valid sig has ring_size >= 2).
        if (reserialized.empty()) {
            __builtin_trap();  // BUG: valid deser produced empty serialization
        }

        // Round-trip: re-deserialize must succeed.
        CLSAGSignature sig2;
        if (!CLSAGSignature::Deserialize(reserialized, sig2)) {
            __builtin_trap();  // BUG: round-trip deserialization failed
        }

        // Check structural equality: c0, s count, D must match.
        if (sig.c0 != sig2.c0) __builtin_trap();
        if (sig.s.size() != sig2.s.size()) __builtin_trap();
        if (sig.D != sig2.D) __builtin_trap();
        for (size_t i = 0; i < sig.s.size(); ++i) {
            if (sig.s[i] != sig2.s[i]) __builtin_trap();
        }
    }
}

// Target 2: Fuzz with controlled ring size byte to stress allocation
void fuzz_controlled_ring_size(const uint8_t* data, size_t size) {
    if (size < 33) return;  // Need at least c0(32) + ring_size(1)

    // Try every possible ring size byte with the given payload
    for (uint16_t rs = 0; rs < 256; ++rs) {
        std::vector<uint8_t> modified(data, data + size);
        modified[32] = static_cast<uint8_t>(rs);  // Override ring size byte

        CLSAGSignature sig;
        bool ok = CLSAGSignature::Deserialize(modified, sig);

        if (ok) {
            // Ring size must be within bounds [2, 64]
            if (sig.s.size() < 2 || sig.s.size() > 64) {
                __builtin_trap();  // BUG: accepted out-of-bounds ring size
            }
        }
    }
}

// Target 3: Truncation testing - try every prefix length
void fuzz_truncation(const uint8_t* data, size_t size) {
    // Build a well-structured signature and test every truncation
    // Use the fuzz data to populate a signature-sized buffer
    size_t sig_size = CLSAGSignature::SerializedSize(CLSAG_RING_SIZE);
    if (size < sig_size) return;

    for (size_t len = 0; len <= sig_size; ++len) {
        std::vector<uint8_t> truncated(data, data + len);
        CLSAGSignature sig;
        bool ok = CLSAGSignature::Deserialize(truncated, sig);

        // Truncated data for a 16-member ring should fail for len < sig_size
        // (unless a smaller ring_size is encoded in the data)
        (void)ok;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    // Use first byte to select target
    uint8_t target = data[0] % 3;
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (target) {
        case 0:
            fuzz_deserialize(payload, payload_size);
            break;
        case 1:
            fuzz_controlled_ring_size(payload, payload_size);
            break;
        case 2:
            fuzz_truncation(payload, payload_size);
            break;
    }

    return 0;
}

// Standalone mode for testing without libFuzzer
#ifdef FUZZ_STANDALONE

#include <iostream>
#include <random>

int main() {
    std::cout << "CLSAG Deserialize Fuzzer - Standalone Mode\n";
    std::cout << "==========================================\n\n";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    const int NUM_ITERATIONS = 10000;
    std::vector<uint8_t> buffer(2048);

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t sz = (gen() % 1500) + 1;
        for (size_t j = 0; j < sz; ++j) {
            buffer[j] = dist(gen);
        }
        LLVMFuzzerTestOneInput(buffer.data(), sz);

        if ((i + 1) % 1000 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    std::cout << "\nAll " << NUM_ITERATIONS << " iterations completed without crashes.\n";
    return 0;
}

#endif  // FUZZ_STANDALONE
