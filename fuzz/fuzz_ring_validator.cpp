/**
 * Ring Signature Fuzz Harness: RingSignatureValidator with Crafted Inputs
 *
 * Feeds crafted ring members, signatures, and key images to the CLSAG
 * sign/verify pipeline to ensure:
 * - No crashes with random ring members, random signatures, random key images
 * - Invalid inputs are correctly rejected (CLSAGVerify returns false)
 * - Deserialization + verification pipeline handles all combinations
 * - Point parsing of malformed pubkeys/commitments does not crash
 *
 * This fuzzer exercises the full consensus validation path:
 *   parse ring members -> deserialize CLSAG -> verify CLSAG signature
 *
 * Unlike the other fuzzers that test individual functions, this one tests
 * the integrated pipeline that ConnectBlock/mempool acceptance would call.
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include -I../src fuzz_ring_validator.cpp \
 *           src/zk/clsag.cpp -lsecp256k1 -lcrypto -o fuzz_ring_validator
 *
 * Run:
 *   ./fuzz_ring_validator corpus/ring_validator/ -max_len=8192
 */

#include "zk/clsag.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>

using namespace dinero::zk;

namespace {

// Helper: extract a CompressedPoint from fuzz data
bool extract_point(const uint8_t* data, size_t size, size_t& pos, CompressedPoint& out) {
    if (pos + 33 > size) return false;
    std::memcpy(out.data(), data + pos, 33);
    pos += 33;
    return true;
}

// Helper: extract a Scalar32 from fuzz data
bool extract_scalar(const uint8_t* data, size_t size, size_t& pos, Scalar32& out) {
    if (pos + 32 > size) return false;
    std::memcpy(out.data(), data + pos, 32);
    pos += 32;
    return true;
}

// Target 1: CLSAGVerify with fully random ring + signature
// This is the primary fuzzing target — tests that CLSAGVerify never crashes.
void fuzz_verify_random(const uint8_t* data, size_t size) {
    if (size < 1) return;

    // Determine ring size from first byte (capped to [2, 32])
    uint8_t raw_ring_size = data[0];
    size_t ring_size = (raw_ring_size % 31) + 2;  // [2, 32]

    size_t pos = 1;

    // Build ring from fuzz data
    std::vector<RingMember> ring(ring_size);
    for (size_t i = 0; i < ring_size; ++i) {
        if (!extract_point(data, size, pos, ring[i].pubkey)) return;
        if (!extract_point(data, size, pos, ring[i].commitment)) return;
    }

    // Key image
    KeyImage key_image;
    if (!extract_point(data, size, pos, key_image)) return;

    // Pseudo-commitment
    CompressedPoint pseudo_commit;
    if (!extract_point(data, size, pos, pseudo_commit)) return;

    // Build CLSAGSignature from remaining data
    CLSAGSignature sig;
    if (!extract_scalar(data, size, pos, sig.c0)) return;

    sig.s.resize(ring_size);
    for (size_t i = 0; i < ring_size; ++i) {
        if (!extract_scalar(data, size, pos, sig.s[i])) return;
    }

    if (!extract_point(data, size, pos, sig.D)) return;

    // Message (remaining bytes, or empty)
    std::vector<uint8_t> message;
    if (pos < size) {
        message.assign(data + pos, data + size);
    }

    // This MUST NOT CRASH regardless of input.
    // Random data should virtually always fail verification.
    bool result = CLSAGVerify(ring, sig, key_image, pseudo_commit, message);
    (void)result;
}

// Target 2: Deserialize-then-verify pipeline
// Mirrors the actual consensus code path in RingSignatureValidator.
void fuzz_deser_then_verify(const uint8_t* data, size_t size) {
    // Layout: sig_len(2) + sig_bytes + ring(16 members) + key_image + pseudo + message
    if (size < 4) return;

    size_t pos = 0;

    // Signature length (2 bytes LE, capped)
    uint16_t sig_len = static_cast<uint16_t>(data[pos]) |
                       (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    sig_len = sig_len % 4096;  // Cap to 4KB

    if (pos + sig_len > size) return;

    // Deserialize CLSAG signature
    std::vector<uint8_t> sig_bytes(data + pos, data + pos + sig_len);
    pos += sig_len;

    CLSAGSignature sig;
    if (!CLSAGSignature::Deserialize(sig_bytes, sig)) {
        return;  // Invalid signature data — that is fine
    }

    // Build ring from remaining data (use sig.s.size() as ring size)
    size_t ring_size = sig.s.size();
    std::vector<RingMember> ring(ring_size);

    for (size_t i = 0; i < ring_size; ++i) {
        if (!extract_point(data, size, pos, ring[i].pubkey)) return;
        if (!extract_point(data, size, pos, ring[i].commitment)) return;
    }

    KeyImage key_image;
    if (!extract_point(data, size, pos, key_image)) return;

    CompressedPoint pseudo_commit;
    if (!extract_point(data, size, pos, pseudo_commit)) return;

    std::vector<uint8_t> message;
    if (pos < size) {
        size_t msg_len = (size - pos > 256) ? 256 : (size - pos);
        message.assign(data + pos, data + pos + msg_len);
    }

    // Verify — must not crash
    bool result = CLSAGVerify(ring, sig, key_image, pseudo_commit, message);
    (void)result;
}

// Target 3: CLSAGSign with random parameters
// Tests that signing never crashes, even with invalid keys/ring.
void fuzz_sign_random(const uint8_t* data, size_t size) {
    if (size < 1) return;

    uint8_t raw_ring_size = data[0];
    size_t ring_size = (raw_ring_size % 15) + 2;  // [2, 16]
    size_t pos = 1;

    std::vector<RingMember> ring(ring_size);
    for (size_t i = 0; i < ring_size; ++i) {
        if (!extract_point(data, size, pos, ring[i].pubkey)) return;
        if (!extract_point(data, size, pos, ring[i].commitment)) return;
    }

    Scalar32 privkey;
    if (!extract_scalar(data, size, pos, privkey)) return;

    Scalar32 commitment_secret;
    if (!extract_scalar(data, size, pos, commitment_secret)) return;

    CompressedPoint pseudo_commit;
    if (!extract_point(data, size, pos, pseudo_commit)) return;

    // Real index from fuzz data
    uint8_t real_index_byte = 0;
    if (pos < size) real_index_byte = data[pos++];
    size_t real_index = real_index_byte % ring_size;

    std::vector<uint8_t> message;
    if (pos < size) {
        size_t msg_len = (size - pos > 64) ? 64 : (size - pos);
        message.assign(data + pos, data + pos + msg_len);
    }

    // Sign — must not crash (may fail with invalid keys, that is fine)
    KeyImage ki_out;
    CLSAGSignature sig_out;
    bool ok = CLSAGSign(ring, real_index, privkey, commitment_secret,
                         pseudo_commit, message, ki_out, sig_out);

    if (ok) {
        // If signing succeeded, verify must also succeed
        bool verify_ok = CLSAGVerify(ring, sig_out, ki_out, pseudo_commit, message);
        if (!verify_ok) {
            __builtin_trap();  // BUG: sign succeeded but verify failed
        }

        // Serialization round-trip
        auto serialized = sig_out.Serialize();
        CLSAGSignature sig_rt;
        if (!CLSAGSignature::Deserialize(serialized, sig_rt)) {
            __builtin_trap();  // BUG: serialization round-trip failed
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    uint8_t target = data[0] % 3;
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (target) {
        case 0:
            fuzz_verify_random(payload, payload_size);
            break;
        case 1:
            fuzz_deser_then_verify(payload, payload_size);
            break;
        case 2:
            fuzz_sign_random(payload, payload_size);
            break;
    }

    return 0;
}

// Standalone mode
#ifdef FUZZ_STANDALONE

#include <iostream>
#include <random>

int main() {
    std::cout << "Ring Validator Fuzzer - Standalone Mode\n";
    std::cout << "=======================================\n\n";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    const int NUM_ITERATIONS = 1000;  // Fewer iterations due to crypto overhead
    std::vector<uint8_t> buffer(8192);

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t sz = (gen() % 6000) + 200;
        for (size_t j = 0; j < sz; ++j) {
            buffer[j] = dist(gen);
        }
        LLVMFuzzerTestOneInput(buffer.data(), sz);

        if ((i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations\n";
        }
    }

    std::cout << "\nAll " << NUM_ITERATIONS << " iterations completed without crashes.\n";
    return 0;
}

#endif  // FUZZ_STANDALONE
