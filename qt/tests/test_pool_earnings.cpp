// Copyright (c) 2026 Dinero Labs.
//
// Lifetime fee earnings for the Pool tab.
//
// `blockchain.getaddressbalance` answers "what is sitting at this address
// now" — a sum over the UTXO set. That number DROPS when the operator
// sweeps their fee to cold storage, which makes it wrong as an earnings
// readout even though it is right as a balance.
//
// Lifetime is instead summed from `blockchain.getaddresshistory`, whose
// entries carry `type` ("receive"/"send"), `amount` and `is_coinbase`.
// The node walks blocks backwards and tells us whether it could see the
// whole chain. An incomplete or unanchored walk is only observed receipts;
// a reorg invalidates the whole walk, including its earlier pages.
//
//   * `history_complete: false` — the node may have a snapshot base,
//     skipped heights, or unresolved inputs;
//   * a full page of results — the RPC caps at 200 entries, so a full
//     page may have been cut off;
//   * `amount_hidden: true` — a confidential output whose value the
//     node cannot show.
//
// Reporting a lower bound as if it were the total is the failure that
// matters here: an operator would conclude the pool paid them less than
// it did.

#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "../src/poolearnings.h"

namespace {

QJsonObject entry(const QString& type, qint64 amount, bool coinbase = true) {
    QJsonObject e;
    e["type"] = type;
    e["height"] = 900;
    e["amount"] = amount;
    e["amount_hidden"] = false;
    e["is_coinbase"] = coinbase;
    return e;
}

QJsonObject entryAt(const QString& type, qint64 amount, int height) {
    QJsonObject e = entry(type, amount);
    e["height"] = height;
    return e;
}

const QString kHash(64, QLatin1Char('a'));

QJsonObject history(const QJsonArray& txs, bool complete = true,
                    const QString& stop_reason = QStringLiteral("genesis")) {
    QJsonObject r;
    r["transactions"] = txs;
    r["history_complete"] = complete;
    r["history_coverage"] = QJsonObject{{"scan_from_height", 900},
                                         {"scan_from_hash", kHash},
                                         {"stop_reason", stop_reason},
                                         {"chain_changed", false}};
    return r;
}

}  // namespace

class TestPoolEarnings : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // The whole point: a swept fee still counts. Three blocks paid in,
    // two of them since spent, lifetime is still all three.
    void sumsEveryReceiptRegardlessOfLaterSpending() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500), entry("receive", 250), entry("receive", 125)}));
        QCOMPARE(r.total_una, 875);
        QCOMPARE(r.count, 3);
        QVERIFY(r.complete);
        QVERIFY(r.caveat.isEmpty());
    }

    // Sends are the operator moving their own money out. Counting them
    // would double-count, and counting them as negative would turn the
    // lifetime figure back into a balance.
    void spendsAreNotReceipts() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500), entry("send", 400), entry("receive", 100)}));
        QCOMPARE(r.total_una, 600);
        QCOMPARE(r.count, 2);
        QVERIFY(r.complete);
    }

    void emptyHistoryIsZeroNotAnError() {
        const auto r = poolearnings::sumReceived(history({}));
        QCOMPARE(r.total_una, 0);
        QCOMPARE(r.count, 0);
        QVERIFY(r.complete);
    }

    // Snapshot wording is justified only by the explicit stop reason.
    void snapshotNodeExplainsTheMissingHistory() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500)}, /*complete=*/false, "assumed_utxo"));
        QCOMPARE(r.total_una, 500);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("snapshot", Qt::CaseInsensitive));
    }

    void otherPartialReasonsDoNotClaimSnapshotOrALowerBound() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500)}, /*complete=*/false, "count_limit"));
        QCOMPARE(r.total_una, 500);
        QVERIFY(!r.complete);
        QVERIFY(!r.caveat.contains("snapshot", Qt::CaseInsensitive));
        QVERIFY(!r.caveat.contains("At least", Qt::CaseInsensitive));
        QVERIFY(r.caveat.contains("incomplete", Qt::CaseInsensitive));
    }

    // A full page is not the end of the history, it is the RPC's cap.
    // DineroSJ's fee address holds 8,082 receipts; stopping at the first
    // page reported 2,000 DIN against a true 33,845 DIN. So a full page
    // must ask for the next one, starting below the oldest height seen.
    void aFullPageAsksForTheNextOne() {
        QJsonArray txs;
        for (int i = 0; i < 200; ++i) txs.append(entryAt("receive", 1, 5000 + i));
        const auto next = poolearnings::nextFromHeight(history(txs), 200);
        QVERIFY(next.has_value());
        QCOMPARE(*next, 4999);
    }

    // One short of the cap means the node had nothing more to give.
    void aPartialPageEndsThePaging() {
        QJsonArray txs;
        for (int i = 0; i < 199; ++i) txs.append(entryAt("receive", 1, 5000 + i));
        QVERIFY(!poolearnings::nextFromHeight(history(txs), 200).has_value());
    }

    // Genesis is the floor. Asking for -1 would either error or restart
    // the walk at the tip, which would loop forever.
    void pagingStopsAtGenesis() {
        QJsonArray txs;
        for (int i = 0; i < 200; ++i) txs.append(entryAt("receive", 1, 0));
        QVERIFY(!poolearnings::nextFromHeight(history(txs), 200).has_value());
    }

    // Pages accumulate: the panel folds each reply into one running total
    // rather than replacing it.
    void pagesAccumulateIntoOneTotal() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900),
                                            entryAt("receive", 400, 800)}));
        poolearnings::historyRequest("din1pOURS", 2, 799, acc);
        QJsonObject second = history({entryAt("receive", 100, 700)});
        QJsonObject coverage = second.value("history_coverage").toObject();
        coverage["scan_from_height"] = 799;
        coverage["scan_from_hash"] = QString(64, QLatin1Char('b'));
        second["history_coverage"] = coverage;
        poolearnings::addPage(acc, second);
        QCOMPARE(acc.total_una, 1000);
        QCOMPARE(acc.count, 3);
        QVERIFY(acc.complete);
    }

    // The production panel uses historyRequest/nextPageStart, so these
    // assertions cover the exact RPC params rather than a separate mock.
    void pageTwoCarriesPageOnesCanonicalAnchor() {
        const QJsonArray first = poolearnings::historyRequest("din1pOURS", 2);
        QCOMPARE(first.size(), 2);
        poolearnings::Received acc;
        const QJsonObject page = history({entryAt("receive", 500, 900),
                                          entryAt("receive", 400, 800)});
        poolearnings::addPage(acc, page);
        const auto next = poolearnings::nextPageStart(acc, page, 2);
        QVERIFY(next.has_value());
        QCOMPARE(*next, 799);
        const QJsonArray later = poolearnings::historyRequest("din1pOURS", 2, *next, acc);
        QCOMPARE(later.size(), 4);
        QCOMPARE(later.at(0).toString(), QStringLiteral("din1pOURS"));
        QCOMPARE(later.at(1).toInt(), 2);
        QCOMPARE(later.at(2).toInt(), 799);
        QCOMPARE(later.at(3).toObject().value("height").toInt(), 900);
        QCOMPARE(later.at(3).toObject().value("hash").toString(), kHash);
        QJsonObject second = history({entryAt("receive", 100, 700)});
        QJsonObject coverage = second.value("history_coverage").toObject();
        coverage["scan_from_height"] = 799;
        coverage["scan_from_hash"] = QString(64, QLatin1Char('b'));
        second["history_coverage"] = coverage;
        poolearnings::addPage(acc, second);
        QCOMPARE(acc.total_una, 1000);
        QVERIFY(acc.complete);
        QCOMPARE(acc.anchor->height, 900);
        QCOMPARE(acc.anchor->hash, kHash);
    }

    void reorgAfterPageOneDiscardsAllReceiptsAndStopsPaging() {
        poolearnings::Received acc;
        const QJsonObject first = history({entryAt("receive", 500, 900),
                                           entryAt("receive", 400, 800)});
        poolearnings::addPage(acc, first);
        QCOMPARE(acc.total_una, 900);
        QJsonObject replaced = history({entryAt("receive", 100, 700),
                                        entryAt("receive", 50, 600)}, false);
        poolearnings::historyRequest("din1pOURS", 2, 799, acc);
        QJsonObject coverage = replaced.value("history_coverage").toObject();
        coverage["scan_from_height"] = 799;
        coverage["scan_from_hash"] = QString(64, QLatin1Char('b'));
        coverage["chain_changed"] = true;
        replaced["history_coverage"] = coverage;
        poolearnings::addPage(acc, replaced);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
        QCOMPARE(acc.count, 0);
        QVERIFY(!poolearnings::nextPageStart(acc, replaced, 2).has_value());
        QVERIFY(acc.caveat.contains("retry", Qt::CaseInsensitive));
        const QString html = poolearnings::renderLifetimeHtml(
            acc, QStringLiteral("900.00000000 DIN"), QStringLiteral("lifetime"), true);
        QVERIFY(!html.contains(QStringLiteral("900.00000000")));
        QVERIFY(!html.contains(QStringLiteral("> lifetime<")));
    }

    void nestedRpcErrorAfterPageOneDiscardsAllReceipts() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900)}));
        QJsonObject error;
        poolearnings::historyRequest("din1pOURS", 1, 899, acc);
        error["error"] = QJsonObject{{"code", -8}, {"message", "invalid anchor"}};
        poolearnings::addPage(acc, error);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
        QCOMPARE(acc.count, 0);
        QVERIFY(!poolearnings::nextPageStart(acc, error, 1).has_value());
        const QString html = poolearnings::renderLifetimeHtml(
            acc, QStringLiteral("500.00000000 DIN"), QStringLiteral("lifetime"), true);
        QVERIFY(!html.contains(QStringLiteral("500.00000000")));
    }

    void anchoredPageWithoutCoverageFailsClosed() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900)}));
        QJsonObject old_server_reply;
        poolearnings::historyRequest("din1pOURS", 1, 899, acc);
        old_server_reply["transactions"] = QJsonArray{entryAt("receive", 400, 800)};
        old_server_reply["history_complete"] = true;
        poolearnings::addPage(acc, old_server_reply);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
        QVERIFY(!acc.complete);
    }

    void anchoredPageWithMalformedHashFailsClosed() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900)}));
        QJsonObject malformed = history({entryAt("receive", 400, 800)});
        poolearnings::historyRequest("din1pOURS", 1, 899, acc);
        QJsonObject coverage = malformed.value("history_coverage").toObject();
        coverage["scan_from_height"] = 899;
        coverage["scan_from_hash"] = "not-a-block-hash";
        malformed["history_coverage"] = coverage;
        poolearnings::addPage(acc, malformed);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
    }

    void anchoredPageWithWrongScanStartFailsClosed() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900)}));
        poolearnings::historyRequest("din1pOURS", 1, 899, acc);
        QJsonObject wrong_start = history({entryAt("receive", 400, 800)});
        // The hash is valid-looking, but it describes the wrong scan start.
        poolearnings::addPage(acc, wrong_start);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
    }

    void malformedTransactionsCannotBecomeAnExactZero() {
        poolearnings::Received acc;
        QJsonObject malformed = history({});
        malformed.remove("transactions");
        poolearnings::addPage(acc, malformed);
        QVERIFY(acc.unusable);
        QVERIFY(!acc.complete);
        const QString html = poolearnings::renderLifetimeHtml(
            acc, QStringLiteral("0.00000000 DIN"), QStringLiteral(""), true);
        QVERIFY(!html.contains(QStringLiteral("0.00000000")));
    }

    void malformedFullPageCannotEndAsAnExactTotal() {
        QJsonArray txs;
        for (int i = 0; i < 200; ++i) {
            QJsonObject bad = entry("receive", 1);
            bad.remove("height");
            txs.append(bad);
        }
        poolearnings::Received acc;
        const QJsonObject page = history(txs);
        poolearnings::addPage(acc, page);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
        QVERIFY(!poolearnings::nextPageStart(acc, page, 200).has_value());
        const QString html = poolearnings::renderLifetimeHtml(
            acc, QStringLiteral("200.00000000 DIN"), QStringLiteral(""), true);
        QVERIFY(!html.contains(QStringLiteral("200.00000000")));
    }

    void firstPageCannotContainHeightsAboveItsScanStart() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 901)}));
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
    }

    void repeatedPageBeyondRequestedHeightCannotCountTwice() {
        poolearnings::Received acc;
        const QJsonObject first = history({entryAt("receive", 500, 900),
                                           entryAt("receive", 400, 800)});
        poolearnings::addPage(acc, first);
        poolearnings::historyRequest("din1pOURS", 2, 799, acc);
        // An old or faulty daemon ignores from_height and returns page one.
        poolearnings::addPage(acc, first);
        QVERIFY(acc.unusable);
        QCOMPARE(acc.total_una, 0);
        QVERIFY(!poolearnings::nextPageStart(acc, first, 2).has_value());
    }

    void olderDaemonWithoutAnchorContinuesOnlyAsObserved() {
        poolearnings::Received acc;
        QJsonObject old_reply = history({entryAt("receive", 500, 900),
                                         entryAt("receive", 400, 800)});
        old_reply.remove("history_coverage");
        poolearnings::addPage(acc, old_reply);
        QVERIFY(!acc.complete);
        QVERIFY(!acc.unusable);
        const auto next = poolearnings::nextPageStart(acc, old_reply, 2);
        QVERIFY(next.has_value());
        const QJsonArray later = poolearnings::historyRequest("din1pOURS", 2, *next, acc);
        QCOMPARE(later.size(), 3);
        QVERIFY(acc.caveat.contains("Observed", Qt::CaseInsensitive));
        const QString html = poolearnings::renderLifetimeHtml(
            acc, QStringLiteral("900.00000000 DIN"), acc.caveat, true);
        QVERIFY(html.contains(QStringLiteral("observed")));
        QVERIFY(!html.contains(QStringLiteral("> lifetime<")));
    }

    // A capped walk is incomplete; a reorg could also invalidate any
    // floor claim, so label the shown receipts as observations only.
    void hittingThePageCapIsReportedAsObserved() {
        poolearnings::Received acc;
        poolearnings::addPage(acc, history({entryAt("receive", 500, 900)}));
        poolearnings::markPageCapReached(acc);
        QVERIFY(!acc.complete);
        QVERIFY(acc.caveat.contains("Observed"));
    }

    // A confidential receipt carries no `amount` at all. Skipping it
    // silently would understate the total with nothing on screen to say
    // why.
    void hiddenAmountsAreFlaggedNotSilentlyDropped() {
        QJsonObject hidden;
        hidden["type"] = "receive";
        hidden["height"] = 900;
        hidden["amount_hidden"] = true;
        QJsonArray txs{entry("receive", 500), hidden};
        const auto r = poolearnings::sumReceived(history(txs));
        QCOMPARE(r.total_una, 500);
        QCOMPARE(r.count, 1);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("confidential", Qt::CaseInsensitive));
    }

    // An entry whose amount is not an integer is a malformed reply, not
    // a zero-value receipt.
    void malformedAmountsCountAsIncomplete() {
        QJsonObject bad;
        bad["type"] = "receive";
        bad["height"] = 900;
        bad["amount_hidden"] = false;
        bad["amount"] = "not a number";
        const auto r = poolearnings::sumReceived(history({entry("receive", 500), bad}));
        QCOMPARE(r.total_una, 500);
        QVERIFY(!r.complete);
    }

    // The card is labelled "fee earnings", and a pool's fee is always a
    // coinbase output. Nothing stops an operator reusing the address as
    // an ordinary wallet address, and an ordinary payment landing there
    // is not pool income.
    void nonCoinbaseReceiptsAreNotPoolEarnings() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500), entry("receive", 400, /*coinbase=*/false)}));
        QCOMPARE(r.total_una, 500);
        QCOMPARE(r.count, 1);
    }

    // A node that does not report the flag at all must not be read as
    // "nothing here was mined", which would zero the whole figure.
    void aMissingCoinbaseFlagStillCounts() {
        QJsonObject e;
        e["type"] = "receive";
        e["height"] = 900;
        e["amount"] = 500;
        e["amount_hidden"] = false;
        const auto r = poolearnings::sumReceived(history({e}));
        QCOMPARE(r.total_una, 500);
        QCOMPARE(r.count, 1);
        QVERIFY(!r.complete);
    }

    // mainwindow's address explorer calls the SAME two RPCs on the SAME
    // RpcClient, and every panel connected to it sees every reply. Without
    // an address check, searching an address in the explorer would repaint
    // the Pool tab's earnings with that address's numbers.
    void repliesForAnotherAddressAreRejected() {
        QJsonObject r;
        r["address"] = "din1pTHEIRS";
        QVERIFY(!poolearnings::isForAddress(r, "din1pOURS"));
        QVERIFY(poolearnings::isForAddress(r, "din1pTHEIRS"));
    }

    void sameAddressAloneCannotDistinguishExplorerReply() {
        QJsonObject explorer;
        explorer["address"] = "din1pSAME";
        explorer["transactions"] = QJsonArray{entryAt("receive", 100, 900)};
        // Both screens request this same address. The transport's per-call
        // alias, tested in RpcReplyAlias, is the required discriminator.
        QVERIFY(poolearnings::isForAddress(explorer, "din1pSAME"));
    }

    // An older node that does not echo the address back leaves us with
    // nothing to compare, so the in-flight bookkeeping is all we have.
    void aReplyWithNoAddressIsAccepted() {
        QVERIFY(poolearnings::isForAddress(QJsonObject{}, "din1pOURS"));
    }

    // The node's own inability to see the chain outranks the other two:
    // it is the one the operator can actually act on (reindex).
    void snapshotCaveatOutranksTruncation() {
        QJsonArray txs;
        for (int i = 0; i < 200; ++i) txs.append(entry("receive", 1));
        auto r = poolearnings::sumReceived(history(txs, /*complete=*/false, "assumed_utxo"));
        poolearnings::markPageCapReached(r);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("snapshot", Qt::CaseInsensitive));
    }
};

QTEST_APPLESS_MAIN(TestPoolEarnings)
#include "test_pool_earnings.moc"
