// Copyright (c) 2026 Dinero Labs.
//
// See nullifier_accumulator.h for the layout and why canonicality is asserted
// here rather than inherited from the storage layer.

#include "consensus/shielded/nullifier_accumulator.h"

#include <algorithm>
#include <cstring>

#include "crypto/sha256.h"

namespace dinero {
namespace consensus {
namespace shielded {

bool NullifierEntryLess(const NullifierEntry& a, const NullifierEntry& b) {
    if (a.height != b.height) {
        return a.height < b.height;
    }
    // std::array<uint8_t,N> compares lexicographically over UNSIGNED octets.
    // Spelled out because a `char`-based comparison would sort 0x80..0xFF
    // below 0x00..0x7F on platforms where char is signed, and two nodes
    // ordering differently would produce different commitments.
    return a.nullifier < b.nullifier;
}

uint256 ComputeNullifierAccumulator(std::vector<NullifierEntry> entries) {
    // Sort and de-duplicate HERE. The caller's order is deliberately
    // irrelevant, and the underlying object is a set, so the same
    // (height, nullifier) twice is one fact.
    std::sort(entries.begin(), entries.end(), NullifierEntryLess);
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    std::vector<uint8_t> pre;
    pre.reserve(13 + entries.size() * 36);
    pre.insert(pre.end(), std::begin(NULLIFIER_ACC_TAG), std::end(NULLIFIER_ACC_TAG));
    pre.push_back(NULLIFIER_ACC_VERSION);

    const auto count = static_cast<uint64_t>(entries.size());
    for (int i = 0; i < 8; ++i) {
        pre.push_back(static_cast<uint8_t>((count >> (i * 8)) & 0xFF));
    }
    for (const auto& e : entries) {
        for (int i = 0; i < 4; ++i) {
            pre.push_back(static_cast<uint8_t>((e.height >> (i * 8)) & 0xFF));
        }
        pre.insert(pre.end(), e.nullifier.begin(), e.nullifier.end());
    }

    dinero::crypto::CSHA256 hasher;
    hasher.Write(pre.data(), pre.size());
    uint8_t digest[32];
    hasher.Finalize(digest);
    uint256 out;
    std::memcpy(out.data, digest, 32);
    return out;
}

std::optional<uint256> AccumulateNullifierSet(const NullifierSet& set) {
    std::vector<NullifierEntry> entries;
    bool malformed = false;

    const bool ok = set.ForEach([&](uint32_t height, const uint8_t* nullifier_32) {
        if (nullifier_32 == nullptr) {
            malformed = true;
            return false;  // stop; do NOT accumulate a partial set
        }
        NullifierEntry e;
        e.height = height;
        std::memcpy(e.nullifier.data(), nullifier_32, 32);
        entries.push_back(e);
        return true;
    });

    // Fail closed. An enumeration that did not complete tells us nothing about
    // the set's contents, and reporting the empty digest here would make a
    // local fault indistinguishable from every nullifier having been deleted.
    if (!ok || malformed) {
        return std::nullopt;
    }
    return ComputeNullifierAccumulator(std::move(entries));
}

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
