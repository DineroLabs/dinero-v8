// Copyright (c) 2026 Dinero Labs.
//
// Whether this panel can read a given pool's ops status.
//
// `OpsStatus.schema_version` says what the pool IS. It does not say what
// the pool is still readable BY, and only the pool knows that:
//
//   * Adding fields is backward compatible. Existing names, types and
//     meanings are untouched, an older client ignores the additions, and
//     a version bump is optional (dinero-sv2 #41 adds `bans` and stays
//     at 2 for exactly this reason).
//   * Removing or renaming a field is not, though a client that
//     validates the fields it reads will at least notice.
//   * REINTERPRETING a field is not, and nothing structural catches it.
//     A field can keep its name and its type while changing meaning —
//     `bps` becoming a share of the block rather than of the contributor
//     split would parse perfectly and render a confidently wrong number.
//
// So a consumer cannot decide this from the version alone. A bare
// `>= 2` floor assumes every future schema stays additive, which is a
// promise no client is in a position to make on the producer's behalf.
//
// The pool therefore declares it:
//
//     schema_version         3   // what I am
//     schema_min_compatible  2   // clients written for >= this can read me
//
// An additive bump leaves `schema_min_compatible` at 2 and every
// deployed wallet keeps working. A breaking change raises it, and older
// wallets refuse with a message that says to upgrade — which is the
// outcome you want, arrived at deliberately rather than by luck.
//
// A pool NEWER than this panel that declares nothing is refused. Silence
// is not a promise, and assuming compatibility is precisely the mistake
// that renders a reinterpreted field as though its meaning had not
// changed. Pools shipping today are all schema 2, so nothing in the
// field is refused by this rule; it only governs what happens the first
// time someone bumps.

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <optional>

namespace poolcontract {

/// The schema this panel was written against: the version whose field
/// names, types and meanings its readers assume. A pool reporting less
/// than this is handled on the panel's legacy path, which hides the
/// readings it cannot get rather than inventing them.
constexpr qint64 kPanelSchema = 2;

/// Why a status payload cannot be read. Separate cases because the fix
/// differs: one means upgrade the pool, the others mean upgrade the
/// wallet.
enum class SchemaVerdict {
    Supported,
    /// Older than the fields this panel reads.
    TooOld,
    /// Newer, and has declared that clients this old can no longer read
    /// it.
    DeclaredIncompatible,
    /// Newer, and has not said whether this panel can read it.
    UndeclaredNewer,
};

inline SchemaVerdict classifySchema(qint64 schema_version,
                                    std::optional<qint64> min_compatible) {
    if (schema_version < kPanelSchema) {
        return SchemaVerdict::TooOld;
    }
    if (schema_version == kPanelSchema) {
        // A pool cannot coherently claim to need a client newer than
        // itself; treat that as a broken declaration rather than a
        // promise.
        if (min_compatible && *min_compatible > kPanelSchema) {
            return SchemaVerdict::DeclaredIncompatible;
        }
        return SchemaVerdict::Supported;
    }
    // Newer than us. Readable only on the pool's own word.
    if (!min_compatible) {
        return SchemaVerdict::UndeclaredNewer;
    }
    return *min_compatible <= kPanelSchema ? SchemaVerdict::Supported
                                           : SchemaVerdict::DeclaredIncompatible;
}

inline bool isSupportedSchema(qint64 schema_version,
                              std::optional<qint64> min_compatible) {
    return classifySchema(schema_version, min_compatible) == SchemaVerdict::Supported;
}

/// Phrased so the operator knows which half of the stack to change.
/// "Unsupported schema_version 3" on its own sends someone to whichever
/// component they happen to think of first.
inline QString unsupportedSchemaReason(qint64 schema_version,
                                       std::optional<qint64> min_compatible) {
    switch (classifySchema(schema_version, min_compatible)) {
        case SchemaVerdict::Supported:
            return QString();
        case SchemaVerdict::TooOld:
            return QStringLiteral(
                       "this pool reports ops schema %1, older than the %2 this panel reads — "
                       "upgrade the pool binary on your pool host")
                .arg(schema_version)
                .arg(kPanelSchema);
        case SchemaVerdict::DeclaredIncompatible:
            return QStringLiteral(
                       "this pool reports ops schema %1 and requires a client written for %2 or "
                       "newer; this wallet reads %3 — upgrade the wallet")
                .arg(schema_version)
                .arg(min_compatible.value_or(kPanelSchema))
                .arg(kPanelSchema);
        case SchemaVerdict::UndeclaredNewer:
            return QStringLiteral(
                       "this pool reports ops schema %1, newer than the %2 this wallet reads, and "
                       "does not state whether it stays compatible — upgrade the wallet")
                .arg(schema_version)
                .arg(kPanelSchema);
    }
    return QString();
}

/// Read the two fields straight off a status payload and decide.
///
/// This, not `classifySchema`, is what the panel calls — because the
/// decision is only as good as what reaches it, and absence has to
/// survive parsing AS absence. Defaulting a missing declaration to
/// `kPanelSchema` would report "compatible with clients like me" on
/// behalf of a pool that never said so, which is precisely the hole the
/// declaration exists to close.
///
/// A payload with no `schema_version` at all is the legacy path, which
/// the panel handles by hiding the readings it cannot get rather than
/// refusing the pool outright, so it is not this function's to reject.
inline SchemaVerdict classifyStatus(const QJsonObject& status) {
    const QJsonValue version = status.value(QStringLiteral("schema_version"));
    if (!version.isDouble()) {
        return SchemaVerdict::Supported;
    }
    // A declaration that is not a whole number is not a declaration.
    std::optional<qint64> min_compatible;
    const QJsonValue declared = status.value(QStringLiteral("schema_min_compatible"));
    if (declared.isDouble()) {
        const double raw = declared.toDouble();
        const auto whole = static_cast<qint64>(raw);
        if (static_cast<double>(whole) == raw) {
            min_compatible = whole;
        }
    }
    return classifySchema(static_cast<qint64>(version.toDouble()), min_compatible);
}

inline bool isSupportedStatus(const QJsonObject& status) {
    return classifyStatus(status) == SchemaVerdict::Supported;
}

inline QString unsupportedStatusReason(const QJsonObject& status) {
    const QJsonValue version = status.value(QStringLiteral("schema_version"));
    if (!version.isDouble()) {
        return QString();
    }
    std::optional<qint64> min_compatible;
    const QJsonValue declared = status.value(QStringLiteral("schema_min_compatible"));
    if (declared.isDouble()) {
        const double raw = declared.toDouble();
        const auto whole = static_cast<qint64>(raw);
        if (static_cast<double>(whole) == raw) {
            min_compatible = whole;
        }
    }
    return unsupportedSchemaReason(static_cast<qint64>(version.toDouble()), min_compatible);
}

}  // namespace poolcontract
