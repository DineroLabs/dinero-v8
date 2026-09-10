// Copyright (c) 2026 Dinero Labs.
//
// Lifetime fee earnings, summed from the chain.
//
// A pool operator's fee is an output in every block their pool finds, so
// the chain is the only source that cannot be wrong — and the only one
// that survives a pool restart, a reinstall, or a pool that is currently
// down. But "what is at this address now" and "what has this address
// ever been paid" are different questions:
//
//   * `blockchain.getaddressbalance` sums the UTXO set, so it FALLS when
//     the operator sweeps their fee to cold storage. Right as a balance,
//     wrong as earnings.
//   * `blockchain.getaddresshistory` reports each receipt, so summing
//     the receipts gives a figure that only ever rises.
//
// The history walk can be incomplete in three ways, and each one turns
// the sum into a lower bound. Presenting a lower bound as the total
// would tell an operator their pool paid them less than it did, so
// incompleteness is detected and named rather than hidden.
//
// Header-only and free of widget dependencies so it can be tested
// without constructing a panel.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace poolearnings {

struct Received {
    /// Sum of every receipt the node could show, in una.
    qint64 total_una = 0;
    /// How many receipts that sum covers.
    int count = 0;
    /// False when the node could not show the whole history, which makes
    /// `total_una` a floor rather than the truth.
    bool complete = true;
    /// Why it is a floor. Empty if and only if `complete`.
    QString caveat;
};

/// `requested_count` is what the caller asked `getaddresshistory` for. A
/// reply holding exactly that many entries hit the cap and may have been
/// cut off, which is indistinguishable from an exact fit — so it is
/// reported as possibly truncated rather than assumed complete.
inline Received sumReceived(const QJsonObject& result, int requested_count) {
    Received out;
    const QJsonArray txs = result.value(QStringLiteral("transactions")).toArray();

    bool hidden_seen = false;
    bool malformed_seen = false;
    for (const QJsonValue& value : txs) {
        const QJsonObject entry = value.toObject();
        // Sends are the operator moving their own money out. Counting
        // them at all would either double-count the receipt that funded
        // them or turn this figure back into a balance.
        if (entry.value(QStringLiteral("type")).toString() != QLatin1String("receive")) {
            continue;
        }
        if (entry.value(QStringLiteral("amount_hidden")).toBool()) {
            hidden_seen = true;
            continue;
        }
        const QJsonValue amount = entry.value(QStringLiteral("amount"));
        if (!amount.isDouble()) {
            malformed_seen = true;
            continue;
        }
        out.total_una += static_cast<qint64>(amount.toDouble());
        ++out.count;
    }

    // Ordered by what the operator can do about it. A node that cannot
    // see its own history is fixable (reindex); a truncated page is a
    // property of the RPC; a confidential amount is unknowable here.
    const bool node_incomplete =
        result.contains(QStringLiteral("history_complete")) &&
        !result.value(QStringLiteral("history_complete")).toBool();
    if (node_incomplete) {
        out.complete = false;
        out.caveat = QStringLiteral(
            "At least this much — this node was bootstrapped from a snapshot and does not hold "
            "the block bodies before its snapshot base, so earlier fee payments cannot be counted.");
    } else if (requested_count > 0 && txs.size() >= requested_count) {
        out.complete = false;
        out.caveat = QStringLiteral(
            "At least this much — the node returned a full page of %1 entries, so older fee "
            "payments may not be included.")
                         .arg(requested_count);
    } else if (hidden_seen) {
        out.complete = false;
        out.caveat = QStringLiteral(
            "At least this much — one or more payments to this address are confidential and "
            "their amounts cannot be read.");
    } else if (malformed_seen) {
        out.complete = false;
        out.caveat = QStringLiteral(
            "At least this much — the node returned an entry whose amount could not be read.");
    }
    return out;
}

}  // namespace poolearnings
