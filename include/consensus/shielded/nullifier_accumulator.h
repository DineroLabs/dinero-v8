// Copyright (c) 2026 Dinero Labs.
//
// Canonical nullifier accumulator (tag 'NUL1').
//
// WHY THIS EXISTS
// ───────────────
// `shielded_root` previously hashed `NullifierSet::SerializeContent()` bytes
// directly. That inherited two properties which are fine for a diagnostic and
// wrong for a value a block header would commit:
//
//   1. CANONICALITY WAS DELEGATED TO SQLITE. The ordering came from
//      `ORDER BY block_height ASC, nullifier ASC` inside the storage layer, so
//      the consensus value depended on the database's BLOB collation and on
//      that query never being reworked. Ordering a consensus commitment
//      depends on must be stated by the commitment, not inherited from a
//      serializer that is free to change for performance reasons.
//
//   2. IT FAILED OPEN. `SerializeContent()` returns an EMPTY vector to signal
//      a read error ("signal error via empty"). Hashing that yields exactly
//      the digest of an empty nullifier set — i.e. a local database fault
//      produces the same commitment as an attacker who deleted every
//      nullifier. `Accumulate()` returns `std::nullopt` instead, and callers
//      MUST refuse to produce a root rather than substitute a value.
//
// LAYOUT (concatenated, then SHA-256'd)
// ─────────────────────────────────────
//   [tag 'NUL1']        4 B
//   [version = 1]       1 B
//   [entry count LE]    8 B
//   then, for each entry, in canonical order:
//   [block height LE]   4 B
//   [nullifier]        32 B
//
// Entries are sorted HERE, by (block_height ASC, then nullifier bytes ASC,
// compared lexicographically as unsigned octets). Duplicates — the same
// (height, nullifier) pair twice — collapse to one, because the underlying
// object is a set. The count commits to the number of entries, so an entry
// cannot be dropped and the remainder re-padded.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "consensus/shielded/nullifier_set.h"
#include "primitives/uint256.h"

namespace dinero {
namespace consensus {
namespace shielded {

inline constexpr char NULLIFIER_ACC_TAG[4] = {'N', 'U', 'L', '1'};
inline constexpr uint8_t NULLIFIER_ACC_VERSION = 1;

struct NullifierEntry {
    uint32_t height{0};
    std::array<uint8_t, 32> nullifier{};
    // Written out rather than `= default`: dinero_shielded compiles at C++17 on
    // the Linux/gcc build, where a defaulted operator== is a hard error. Caught
    // by building on the target platform, not on the developer's Mac.
    bool operator==(const NullifierEntry& other) const {
        return height == other.height && nullifier == other.nullifier;
    }
};

/// Canonical order: height ascending, then nullifier bytes ascending as
/// unsigned octets. Exposed so tests can assert the ordering rule directly
/// rather than inferring it from a digest.
bool NullifierEntryLess(const NullifierEntry& a, const NullifierEntry& b);

/// Digest over the entries. Sorts and de-duplicates internally, so the caller's
/// order is irrelevant — that is the point.
uint256 ComputeNullifierAccumulator(std::vector<NullifierEntry> entries);

/// Enumerate a live set and accumulate it.
///
/// Returns `std::nullopt` if enumeration fails. A caller must propagate that
/// failure, never substitute the empty-set digest: an unreadable set and an
/// empty set are different facts, and conflating them is the forgery this
/// accumulator exists to prevent.
std::optional<uint256> AccumulateNullifierSet(const NullifierSet& set);

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
