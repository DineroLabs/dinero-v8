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
#include <optional>

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

/// Whether a reply belongs to the address we asked about.
///
/// `RpcClient` broadcasts every reply to every connected widget, and
/// mainwindow's address explorer calls these same two RPCs on the same
/// client. Without this check, searching an address over there repaints
/// the Pool tab's earnings with that address's numbers.
///
/// A node that does not echo `address` back leaves nothing to compare,
/// so the reply is accepted and the caller's in-flight bookkeeping is
/// the only guard.
inline bool isForAddress(const QJsonObject& result, const QString& expected) {
    const QJsonValue echoed = result.value(QStringLiteral("address"));
    if (!echoed.isString()) {
        return true;
    }
    return echoed.toString() == expected;
}

/// `blockchain.getaddresshistory` caps each reply at 200 entries, so one
/// call is a PAGE, not the history. DineroSJ's fee address carries 8,082
/// receipts across heights 7,562-109,580: reading only the first page
/// reported 2,000 DIN against a true 33,845 DIN, which is worse than no
/// figure at all. Paging with `from_height` reads the lot — 41 calls and
/// 5.4s against that address on a synced node.
///
/// Fold one page into a running total.
inline void addPage(Received& acc, const QJsonObject& page) {
    const QJsonArray txs = page.value(QStringLiteral("transactions")).toArray();
    for (const QJsonValue& value : txs) {
        const QJsonObject entry = value.toObject();
        // Sends are the operator moving their own money out. Counting
        // them at all would either double-count the receipt that funded
        // them or turn this figure back into a balance.
        if (entry.value(QStringLiteral("type")).toString() != QLatin1String("receive")) {
            continue;
        }
        // A pool's fee is always a coinbase output. Nothing stops an
        // operator reusing the fee address as an ordinary wallet address,
        // and an ordinary payment landing there is not pool income — on
        // DineroSJ two such payments account for 1,500 DIN of a balance
        // that is otherwise all block rewards. A node that omits the flag
        // is not saying "not mined", so a missing flag still counts.
        const QJsonValue coinbase = entry.value(QStringLiteral("is_coinbase"));
        if (coinbase.isBool() && !coinbase.toBool()) {
            continue;
        }
        if (entry.value(QStringLiteral("amount_hidden")).toBool()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "At least this much — one or more payments to this address are confidential and "
                "their amounts cannot be read.");
            continue;
        }
        const QJsonValue amount = entry.value(QStringLiteral("amount"));
        if (!amount.isDouble()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "At least this much — the node returned an entry whose amount could not be read.");
            continue;
        }
        acc.total_una += static_cast<qint64>(amount.toDouble());
        ++acc.count;
    }

    // A node that cannot see its own history outranks the rest: it is the
    // one the operator can actually act on, and it is the common case,
    // since RUN-A-POOL.md has every new operator bootstrap from a
    // snapshot.
    if (page.contains(QStringLiteral("history_complete")) &&
        !page.value(QStringLiteral("history_complete")).toBool()) {
        acc.complete = false;
        acc.caveat = QStringLiteral(
            "At least this much — this node was bootstrapped from a snapshot and does not hold "
            "the block bodies before its snapshot base, so earlier payments cannot be counted. "
            "Reindexing the node from genesis is what makes this figure exact.");
    }
}

/// The height to request next, or nothing when this page ended the walk.
///
/// A page short of the cap means the node had no more to give. A full
/// page resumes one block below the oldest entry it returned; genesis is
/// the floor, because asking below it would either error or restart the
/// walk at the tip and never terminate.
inline std::optional<int> nextFromHeight(const QJsonObject& page, int requested_count) {
    const QJsonArray txs = page.value(QStringLiteral("transactions")).toArray();
    if (requested_count <= 0 || txs.size() < requested_count) {
        return std::nullopt;
    }
    int lowest = -1;
    for (const QJsonValue& value : txs) {
        const QJsonValue height = value.toObject().value(QStringLiteral("height"));
        if (!height.isDouble()) continue;
        const int h = static_cast<int>(height.toDouble());
        if (lowest < 0 || h < lowest) lowest = h;
    }
    if (lowest <= 0) {
        return std::nullopt;
    }
    return lowest - 1;
}

/// Paging gave up before the history ran out. The only case where the
/// total really is truncated.
inline void markPageCapReached(Received& acc) {
    acc.complete = false;
    acc.caveat = QStringLiteral(
        "At least this much — the address has more history than this check reads in one go.");
}

/// One page, for callers that do not page. Kept so a single reply can be
/// summed on its own.
inline Received sumReceived(const QJsonObject& page) {
    Received out;
    addPage(out, page);
    return out;
}

}  // namespace poolearnings
