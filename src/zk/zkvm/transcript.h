// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Fiat-Shamir Transcript for Non-Interactive Proofs
 *
 * Converts interactive proof protocols into non-interactive ones by
 * replacing verifier challenges with hash-derived values. Uses SHA256
 * with domain separation for each proof step.
 *
 * The transcript accumulates all prover messages and produces
 * deterministic challenges. Both prover and verifier maintain identical
 * transcripts, ensuring soundness.
 */

#include "zk/zkvm/scalar.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <openssl/sha.h>

namespace dinero {
namespace zk {
namespace zkvm {

class Transcript {
public:
    explicit Transcript(const char* label) {
        // Initialize with domain separator
        append_bytes("dom-sep", reinterpret_cast<const uint8_t*>(label), std::strlen(label));
    }

    // Append a labeled scalar to the transcript
    void append_scalar(const char* label, const Scalar& s) {
        append_bytes(label, s.data(), Scalar::SIZE);
    }

    // Append a labeled point to the transcript
    void append_point(const char* label, const Point& p, secp256k1_context* ctx) {
        Point::Compressed compressed;
        if (p.serialize(compressed, ctx)) {
            append_bytes(label, compressed.data(), Point::COMPRESSED_SIZE);
        }
    }

    // Append a labeled u64 to the transcript
    void append_u64(const char* label, uint64_t v) {
        uint8_t bytes[8];
        for (int i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(v >> (56 - 8*i));
        append_bytes(label, bytes, 8);
    }

    // Derive a challenge scalar from the current transcript state
    Scalar challenge_scalar(const char* label, secp256k1_context* ctx) {
        // Hash: state || "chal" || label || label_len
        SHA256_CTX h;
        SHA256_Init(&h);
        SHA256_Update(&h, state_.data(), state_.size());
        SHA256_Update(&h, "chal", 4);
        size_t label_len = std::strlen(label);
        SHA256_Update(&h, label, label_len);
        uint8_t len_byte = static_cast<uint8_t>(label_len & 0xff);
        SHA256_Update(&h, &len_byte, 1);

        uint8_t hash[32];
        SHA256_Final(hash, &h);

        // Update state with the challenge (for chaining)
        state_.insert(state_.end(), hash, hash + 32);

        return Scalar::from_hash(hash, ctx);
    }

private:
    std::vector<uint8_t> state_;

    void append_bytes(const char* label, const uint8_t* data, size_t len) {
        size_t label_len = std::strlen(label);
        // Format: label_len(1) || label || data_len(4) || data
        state_.push_back(static_cast<uint8_t>(label_len & 0xff));
        state_.insert(state_.end(), label, label + label_len);
        for (int i = 0; i < 4; ++i) {
            state_.push_back(static_cast<uint8_t>((len >> (24 - 8*i)) & 0xff));
        }
        state_.insert(state_.end(), data, data + len);
    }
};

} // namespace zkvm
} // namespace zk
} // namespace dinero
