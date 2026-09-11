// Copyright (c) 2026 Dinero Labs.
//
// The pool's ops contract version.
//
// `OpsStatus.schema_version` is described in dinero-sv2 as "a monotonic
// contract version for strict consumers", with earlier fields kept
// present so older clients keep working. The contract therefore only
// ever ADDS fields, which makes a newer schema readable by an older
// panel — the version tells a consumer what it can COUNT on, not what it
// must match.
//
// Treating it as an equality inverts that. The pool and the wallet ship
// from separate repositories on separate schedules, and an operator
// upgrades their pool by hand, so an equality check means whoever
// upgrades their pool first takes their own Pool tab offline with no
// indication that the wallet is the half that needs replacing.
//
// So: a floor, not an equality.

#pragma once

#include <QString>

namespace poolcontract {

/// The oldest schema carrying the fields this panel reads — daemon and
/// Stratum health, last share, last block, rejection reasons. A pool
/// reporting less than this is handled on the panel's legacy path
/// instead, which hides those readings rather than inventing them.
constexpr qint64 kMinSupportedSchema = 2;

inline bool isSupportedSchema(qint64 schema_version) {
    return schema_version >= kMinSupportedSchema;
}

/// Why a version was refused, phrased so the operator knows which half
/// of the stack to change. "Unsupported schema_version 1" on its own
/// sends someone looking at their wallet when the pool is what needs
/// upgrading.
inline QString unsupportedSchemaReason(qint64 schema_version) {
    return QStringLiteral(
               "this pool reports ops schema %1, older than the %2 this panel reads — "
               "upgrade the pool binary on your pool host")
        .arg(schema_version)
        .arg(kMinSupportedSchema);
}

}  // namespace poolcontract
