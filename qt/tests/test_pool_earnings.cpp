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
// whole chain, so the three ways the total can be a LOWER BOUND rather
// than the truth all have to be detected and said out loud:
//
//   * `history_complete: false` — the node bootstrapped from an
//     AssumeUTXO snapshot and does not hold pre-base block bodies;
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
    e["amount"] = amount;
    e["amount_hidden"] = false;
    e["is_coinbase"] = coinbase;
    return e;
}

QJsonObject history(const QJsonArray& txs, bool complete = true) {
    QJsonObject r;
    r["transactions"] = txs;
    r["history_complete"] = complete;
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
            history({entry("receive", 500), entry("receive", 250), entry("receive", 125)}), 200);
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
            history({entry("receive", 500), entry("send", 400), entry("receive", 100)}), 200);
        QCOMPARE(r.total_una, 600);
        QCOMPARE(r.count, 2);
        QVERIFY(r.complete);
    }

    void emptyHistoryIsZeroNotAnError() {
        const auto r = poolearnings::sumReceived(history({}), 200);
        QCOMPARE(r.total_una, 0);
        QCOMPARE(r.count, 0);
        QVERIFY(r.complete);
    }

    // A snapshot-bootstrapped node cannot see pre-base blocks at all, so
    // the sum is a floor. `history_complete` is the node saying so.
    void snapshotNodeReportsALowerBound() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500)}, /*complete=*/false), 200);
        QCOMPARE(r.total_una, 500);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("snapshot", Qt::CaseInsensitive));
    }

    // A full page means the RPC stopped at its cap; there may be older
    // receipts it never reached.
    void afullPageMayHaveBeenTruncated() {
        QJsonArray txs;
        for (int i = 0; i < 200; ++i) txs.append(entry("receive", 1));
        const auto r = poolearnings::sumReceived(history(txs), 200);
        QCOMPARE(r.total_una, 200);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("200"));
    }

    // One short of the cap is a complete answer.
    void aPartialPageIsComplete() {
        QJsonArray txs;
        for (int i = 0; i < 199; ++i) txs.append(entry("receive", 1));
        const auto r = poolearnings::sumReceived(history(txs), 200);
        QVERIFY(r.complete);
    }

    // A confidential receipt carries no `amount` at all. Skipping it
    // silently would understate the total with nothing on screen to say
    // why.
    void hiddenAmountsAreFlaggedNotSilentlyDropped() {
        QJsonObject hidden;
        hidden["type"] = "receive";
        hidden["amount_hidden"] = true;
        QJsonArray txs{entry("receive", 500), hidden};
        const auto r = poolearnings::sumReceived(history(txs), 200);
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
        bad["amount_hidden"] = false;
        bad["amount"] = "not a number";
        const auto r = poolearnings::sumReceived(history({entry("receive", 500), bad}), 200);
        QCOMPARE(r.total_una, 500);
        QVERIFY(!r.complete);
    }

    // The card is labelled "fee earnings", and a pool's fee is always a
    // coinbase output. Nothing stops an operator reusing the address as
    // an ordinary wallet address, and an ordinary payment landing there
    // is not pool income.
    void nonCoinbaseReceiptsAreNotPoolEarnings() {
        const auto r = poolearnings::sumReceived(
            history({entry("receive", 500), entry("receive", 400, /*coinbase=*/false)}), 200);
        QCOMPARE(r.total_una, 500);
        QCOMPARE(r.count, 1);
    }

    // A node that does not report the flag at all must not be read as
    // "nothing here was mined", which would zero the whole figure.
    void aMissingCoinbaseFlagStillCounts() {
        QJsonObject e;
        e["type"] = "receive";
        e["amount"] = 500;
        e["amount_hidden"] = false;
        const auto r = poolearnings::sumReceived(history({e}), 200);
        QCOMPARE(r.total_una, 500);
        QCOMPARE(r.count, 1);
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
        const auto r = poolearnings::sumReceived(history(txs, /*complete=*/false), 200);
        QVERIFY(!r.complete);
        QVERIFY(r.caveat.contains("snapshot", Qt::CaseInsensitive));
    }
};

QTEST_APPLESS_MAIN(TestPoolEarnings)
#include "test_pool_earnings.moc"
