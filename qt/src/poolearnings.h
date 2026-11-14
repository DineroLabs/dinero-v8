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
//     complete, anchored history gives payments on the selected chain.
//
// An unanchored or incomplete history is an observation, not a lifetime
// total. A reorg or malformed reply invalidates even that observation.
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
    /// False unless the node explicitly certified complete coverage.
    bool complete = true;
    /// True when no amount from this walk may be displayed (reorg/error).
    bool unusable = false;
    /// Why the result is incomplete or unusable. Empty only when complete.
    QString caveat;
    struct Anchor {
        int height;
        QString hash;

        QJsonObject toJson() const {
            return {{QStringLiteral("height"), height}, {QStringLiteral("hash"), hash}};
        }
    };
    /// Page one's canonical scan-start block. Later requests carry this.
    std::optional<Anchor> anchor;
    /// The bound sent with the current page. A daemon that ignores the
    /// bound must not make a repeated page look like a complete walk.
    std::optional<int> requested_from_height;
    int pages = 0;
};

inline std::optional<Received::Anchor> pageAnchor(const QJsonObject& coverage) {
    const QJsonValue height = coverage.value(QStringLiteral("scan_from_height"));
    const QJsonValue hash_value = coverage.value(QStringLiteral("scan_from_hash"));
    if (!height.isDouble() || !hash_value.isString()) return std::nullopt;
    const int h = height.toInt(-1);
    if (h < 0 || height.toDouble() != static_cast<double>(h)) return std::nullopt;
    const QString hash = hash_value.toString();
    if (hash.size() != 64) return std::nullopt;
    for (const QChar c : hash) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) {
            return std::nullopt;
        }
    }
    return Received::Anchor{h, hash};
}

inline QJsonArray historyRequest(const QString& address, int count) {
    return QJsonArray{address, count};
}

inline QJsonArray historyRequest(const QString& address, int count,
                                 int from_height, Received& acc) {
    acc.requested_from_height = from_height;
    QJsonArray params{address, count, from_height};
    if (acc.anchor) params.append(acc.anchor->toJson());
    return params;
}

inline void invalidate(Received& acc, const QString& reason) {
    acc.total_una = 0;
    acc.count = 0;
    acc.complete = false;
    acc.unusable = true;
    acc.caveat = reason;
}

/// Whether a reply belongs to the address we asked about.
///
/// The per-request reply route keeps the explorer's same-address call out
/// of this panel; an echoed address is an additional response check.
///
/// Older nodes may not echo `address`, so the reply route remains the
/// primary guard in that case.
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
    if (acc.unusable) return;
    const QJsonValue nested_error = page.value(QStringLiteral("error"));
    if (!nested_error.isUndefined() && !nested_error.isNull()) {
        invalidate(acc, QStringLiteral("The node could not verify this address's history. Please retry."));
        return;
    }
    const QJsonValue coverage_value = page.value(QStringLiteral("history_coverage"));
    const QJsonObject coverage = coverage_value.toObject();
    if (coverage.value(QStringLiteral("chain_changed")).toBool() ||
        page.value(QStringLiteral("chain_changed")).toBool()) {
        invalidate(acc, QStringLiteral("The chain changed while reading this address. Please retry."));
        return;
    }
    if (!page.value(QStringLiteral("transactions")).isArray()) {
        invalidate(acc, QStringLiteral("The node returned malformed address history. Please retry."));
        return;
    }
    const std::optional<Received::Anchor> current_anchor = pageAnchor(coverage);
    if (acc.pages > 0 && acc.anchor &&
        (!coverage_value.isObject() || !current_anchor ||
         !acc.requested_from_height ||
         current_anchor->height != *acc.requested_from_height ||
         !coverage.value(QStringLiteral("chain_changed")).isBool() ||
         !page.value(QStringLiteral("history_complete")).isBool())) {
        invalidate(acc, QStringLiteral(
            "The node could not confirm the same chain across history pages. Please retry."));
        return;
    }
    if (acc.pages == 0) {
        acc.anchor = current_anchor;
        if (!acc.anchor || !coverage.value(QStringLiteral("chain_changed")).isBool()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "Observed payments only. This node cannot anchor history pages to one chain; "
                "the lifetime total is unavailable.");
        }
    }
    ++acc.pages;
    const QJsonArray txs = page.value(QStringLiteral("transactions")).toArray();
    for (const QJsonValue& value : txs) {
        if (!value.isObject()) {
            invalidate(acc, QStringLiteral("The node returned a malformed history entry. Please retry."));
            return;
        }
        const QJsonObject entry = value.toObject();
        const QJsonValue height_value = entry.value(QStringLiteral("height"));
        const int height = height_value.toInt(-1);
        const QJsonValue type_value = entry.value(QStringLiteral("type"));
        if (!height_value.isDouble() || height < 0 ||
            height_value.toDouble() != static_cast<double>(height) ||
            (acc.pages == 1 && acc.anchor && height > acc.anchor->height) ||
            (acc.requested_from_height && height > *acc.requested_from_height) ||
            !type_value.isString() ||
            (type_value.toString() != QLatin1String("receive") &&
             type_value.toString() != QLatin1String("send"))) {
            invalidate(acc, QStringLiteral("The node returned a malformed history entry. Please retry."));
            return;
        }
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
        // is not saying "not mined", so it remains an observed receipt,
        // but cannot support an exact pool-reward claim.
        const QJsonValue coinbase = entry.value(QStringLiteral("is_coinbase"));
        if (coinbase.isBool() && !coinbase.toBool()) {
            continue;
        }
        if (!coinbase.isBool()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "Observed payments only — the node did not identify every receipt as a block reward.");
        }
        if (entry.value(QStringLiteral("amount_hidden")).toBool()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "Observed payments only — one or more payments to this address are confidential and "
                "their amounts cannot be read.");
            continue;
        }
        const QJsonValue amount = entry.value(QStringLiteral("amount"));
        if (!amount.isDouble()) {
            acc.complete = false;
            acc.caveat = QStringLiteral(
                "Observed payments only — the node returned an entry whose amount could not be read.");
            continue;
        }
        acc.total_una += static_cast<qint64>(amount.toDouble());
        ++acc.count;
    }

    // Only the explicit assumed_utxo reason justifies snapshot wording.
    // Other incomplete replies may contain gaps or unresolved inputs.
    if (!page.value(QStringLiteral("history_complete")).isBool() ||
        !page.value(QStringLiteral("history_complete")).toBool()) {
        acc.complete = false;
        if (coverage.value(QStringLiteral("stop_reason")).toString() == QLatin1String("assumed_utxo")) {
            acc.caveat = QStringLiteral(
                "Observed payments only — this snapshot node cannot read payments before its snapshot base.");
        } else {
            acc.caveat = QStringLiteral(
                "Observed payments only — this node reported incomplete address history; "
                "the lifetime total is unavailable.");
        }
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

inline std::optional<int> nextPageStart(Received& acc, const QJsonObject& page,
                                       int requested_count) {
    if (acc.unusable) return std::nullopt;
    const auto next = nextFromHeight(page, requested_count);
    if (next && acc.requested_from_height && *next >= *acc.requested_from_height) {
        invalidate(acc, QStringLiteral("The node repeated a history page. Please retry."));
        return std::nullopt;
    }
    return next;
}

/// Paging gave up before the history ran out. The only case where the
/// total really is truncated.
inline void markPageCapReached(Received& acc) {
    if (acc.unusable) return;
    acc.complete = false;
    if (acc.caveat.isEmpty()) {
        acc.caveat = QStringLiteral(
            "Observed payments only — this address has more history than this check reads in one go.");
    }
}

/// The panel's amount line. A reorg/error must not retain even the amount
/// accumulated before the failed page; incomplete walks are labelled as
/// observed rather than as an exact lifetime figure.
inline QString renderLifetimeHtml(const Received& acc, const QString& formatted_amount,
                                  const QString& detail, bool final_page) {
    if (acc.unusable) {
        return QStringLiteral("<span style='color:#ff6b6b;'>%1</span>")
            .arg(acc.caveat.toHtmlEscaped());
    }
    return QStringLiteral(
               "<span style='font-size:19px; font-weight:700; color:#51cf66;'>%1</span>"
               "<span style='color:#9fb3c8; font-size:11px;'> %2</span>"
               "<br/><span style='color:#9fb3c8; font-size:11px;'>%3</span>")
        .arg(formatted_amount,
             acc.complete && final_page ? QStringLiteral("lifetime") : QStringLiteral("observed"),
             detail);
}

/// One page, for callers that do not page. Kept so a single reply can be
/// summed on its own.
inline Received sumReceived(const QJsonObject& page) {
    Received out;
    addPage(out, page);
    return out;
}

}  // namespace poolearnings
