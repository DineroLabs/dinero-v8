// src/consensus/shielded/shielded_consistency.cpp
#include "consensus/shielded/shielded_consistency.h"
#include <sstream>

namespace dinero::consensus::shielded {

static std::string hex16(const dinero::uint256& h){ return h.GetHex().substr(0,16); }

ShieldedConsistencyReport ClassifyShieldedConsistency(const ShieldedConsistencyInputs& in) {
    ShieldedConsistencyReport rep; rep.in = in;
    auto set = [&](ShieldedConsistency s, const std::string& why){
        rep.status = s;
        std::ostringstream os;
        os << "[Shielded] " << why
           << " observed{tree_size=" << in.observed.tree_size
           << ",nf=" << in.observed.nullifier_count
           << ",root=" << hex16(in.observed.root) << "}"
           << " marker{height=" << in.marker_height
           << ",tree_size=" << in.marker.tree_size
           << ",nf=" << in.marker.nullifier_count
           << ",root=" << hex16(in.marker.root) << "}"
           << " active_height=" << in.active_height
           << ". Transparent funds unaffected and scannable (scanutxos);"
              " repair: 'reconcileshielded' or resync.";
        rep.detail = os.str();
    };

    // 1. Marker-presence checks (before tip-height: a missing marker can't be compared to tip).
    if (!in.marker_present) {
        if (in.activity_below_tip) {
            set(ShieldedConsistency::MarkerMissingButActivityExists,
                "tip marker missing but shielded activity exists at/below tip.");
            return rep;
        }
        rep.status = ShieldedConsistency::MarkerMissingNoActivity;
        rep.detail = "[Shielded] no marker, no activity <= tip (benign; will persist).";
        return rep;
    }

    // 2. Tip-height/hash mismatch (a lagging marker is classified before triple checks).
    if (in.marker_height != static_cast<int32_t>(in.active_height) ||
        in.marker_hash != in.active_hash) {
        set(ShieldedConsistency::TipHeightMismatch,
            "tip marker height/hash disagrees with active tip.");
        return rep;
    }

    // 3. Triple mismatches: tree_size first (structural count), then root, then nullifier_count.
    if (in.observed.tree_size != in.marker.tree_size) {
        set(ShieldedConsistency::SizeMismatch, "commitment-tree tree_size mismatch.");
        return rep;
    }
    if (in.observed.root != in.marker.root) {
        set(ShieldedConsistency::RootMismatch, "commitment-tree root mismatch.");
        return rep;
    }
    if (in.observed.nullifier_count != in.marker.nullifier_count) {
        set(ShieldedConsistency::NullifierCountMismatch, "nullifier-count mismatch.");
        return rep;
    }

    rep.status = ShieldedConsistency::Aligned;
    rep.detail = "[Shielded] aligned.";
    return rep;
}

}  // namespace dinero::consensus::shielded
