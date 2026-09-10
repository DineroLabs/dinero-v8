// Copyright (c) 2026 Dinero Labs.
//
// Contributor share arithmetic for the Pool tab.
//
// `MinerStatus.bps` from the pool's ops endpoint is a share of the
// CONTRIBUTOR POT, not of the block. `split.rs` takes the operator fee
// off the reward first:
//
//     fee_una = reward * fee_bps / 10000
//     pot     = reward - fee_una
//     paid    = pot * contributor_bps / 10000
//
// so the two percentages differ by exactly the fee, and they coincide
// only when the fee is zero. Both are worth showing: the split share is
// what the pool reports and what an operator sees in a hand-run
// `curl /status`; the block share is what the contributor is actually
// paid.
//
// Header-only and free of widget dependencies so the arithmetic can be
// tested without constructing a panel.

#pragma once

#include <QString>

#include <algorithm>

namespace poolshare {

/// Basis points are a fraction of 10000 by definition. The pool clamps
/// its own fee the same way (`split.rs`: `p.fee_bps.min(10_000)`), but
/// the panel parses whatever the endpoint sends — including values a
/// correct pool would never produce — so clamp before dividing rather
/// than rendering a negative or >100% share.
inline qint64 clampBps(qint64 bps) {
    return std::clamp<qint64>(bps, 0, 10000);
}

/// Share of the contributor pot, in percent. This is the raw `bps`.
inline double splitSharePercent(qint64 bps) {
    return static_cast<double>(clampBps(bps)) / 100.0;
}

/// Share of the whole block reward, in percent: the split share reduced
/// by the operator fee the pool is charging RIGHT NOW. The fee is
/// operator-controlled and changeable at runtime, so it can never be
/// baked in as a constant.
inline double blockSharePercent(qint64 bps, qint64 fee_bps) {
    const qint64 pot_bps = 10000 - clampBps(fee_bps);
    return static_cast<double>(clampBps(bps)) * static_cast<double>(pot_bps) / 1000000.0;
}

/// Two decimals, matching the operator-fee readout on the same panel.
inline QString formatPercent(double percent) {
    return QStringLiteral("%1%").arg(percent, 0, 'f', 2);
}

inline QString splitShareText(qint64 bps) {
    return formatPercent(splitSharePercent(bps));
}

inline QString blockShareText(qint64 bps, qint64 fee_bps) {
    return formatPercent(blockSharePercent(bps, fee_bps));
}

}  // namespace poolshare
