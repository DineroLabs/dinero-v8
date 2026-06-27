#pragma once
// Shielded tip-consistency invariant (node-local; NOT a consensus rule).
// Pure classifier comparing the in-memory shielded triple against the
// persisted ShieldedTipMarker. See docs/superpowers/specs/2026-06-27-
// shielded-tip-consistency-invariant-design.md.
#include <cstdint>
#include <string>
#include "primitives/uint256.h"   // dinero::uint256

namespace dinero::consensus::shielded {

enum class ShieldedConsistency {
    Aligned,
    MarkerMissingNoActivity,         // benign: no marker, no shielded activity <= tip
    MarkerMissingButActivityExists,  // desync: no marker but activity exists <= tip
    TipHeightMismatch,               // marker height/hash != active tip
    RootMismatch,
    SizeMismatch,
    NullifierCountMismatch,
};

struct ShieldedTriple {
    dinero::uint256 root;
    uint64_t tree_size{0};
    uint64_t nullifier_count{0};
};

struct ShieldedConsistencyInputs {
    ShieldedTriple observed;
    bool            marker_present{false};
    ShieldedTriple  marker;
    int32_t         marker_height{0};
    dinero::uint256 marker_hash;
    uint32_t        active_height{0};
    dinero::uint256 active_hash;
    bool            activity_below_tip{false};
};

struct ShieldedConsistencyReport {
    ShieldedConsistency        status{ShieldedConsistency::Aligned};
    std::string                detail;
    ShieldedConsistencyInputs  in;
    // Classes a bounded forward-replay can repair.
    bool healable_class() const {
        switch (status) {
            case ShieldedConsistency::MarkerMissingNoActivity:        // persist-only
            case ShieldedConsistency::MarkerMissingButActivityExists:
            case ShieldedConsistency::TipHeightMismatch:
            case ShieldedConsistency::RootMismatch:
            case ShieldedConsistency::SizeMismatch:
            case ShieldedConsistency::NullifierCountMismatch:
                return true;
            case ShieldedConsistency::Aligned:
                return false;
        }
        return false;
    }
};

// Pure: no I/O, no side effects. Order of checks: marker-presence first (a
// missing marker can't be compared to the tip), then tip-height (a lagging
// marker is TipHeightMismatch even if triples would differ), then tree_size,
// root, nullifier_count.
ShieldedConsistencyReport ClassifyShieldedConsistency(const ShieldedConsistencyInputs& in);

}  // namespace dinero::consensus::shielded
