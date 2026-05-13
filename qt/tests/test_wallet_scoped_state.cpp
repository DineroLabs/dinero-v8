#include <QtTest/QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/advisorybanner.h"
#include "../src/transactiontracker.h"

namespace {

TrackedTransaction makeTrackedTransaction(const QString& txid) {
    TrackedTransaction tx;
    tx.txid = txid;
    tx.status = TxStatus::Pending;
    tx.createdAt = QDateTime::currentDateTimeUtc();
    tx.address = QStringLiteral("din1ptest");
    tx.amountUna = 123456789;
    return tx;
}

QJsonObject makeTrackedTransactionJson(const QString& txid) {
    QJsonObject tx;
    tx["txid"] = txid;
    tx["is_incoming"] = false;
    tx["amount_una"] = 123456789;
    tx["address"] = QStringLiteral("din1ptest");
    tx["status"] = QStringLiteral("pending");
    tx["confirmations"] = 0;
    tx["change_address"] = QString();
    tx["change_amount_una"] = 0;
    tx["fee_paid_una"] = 0;
    tx["mempool_missing_polls"] = 0;
    tx["last_seen_in_mempool"] = QString();
    tx["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    tx["replaced_by_txid"] = QString();
    tx["status_reason"] = QString();
    tx["notified_confirmations"] = 0;
    tx["reservation_id"] = QStringLiteral("legacy-reservation");
    tx["selected_input_outpoints"] = QJsonArray();
    tx["selected_input_amounts_una"] = QJsonObject();
    return tx;
}

void writeJsonFile(const QString& path, const QJsonDocument& document) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(QStringLiteral("Failed to open %1").arg(path)));
    file.write(document.toJson(QJsonDocument::Compact));
    file.close();
}

} // namespace

class WalletScopedStateTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void trackerIsolatesPerWalletState();
    void trackerMigratesLegacyStateOnce();
    void advisoryQueueIsolatesPerWalletState();
    void advisoryQueueMigratesLegacyStateOnce();
};

void WalletScopedStateTest::trackerIsolatesPerWalletState() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    TransactionTracker tracker(tempDir.path(), nullptr);
    tracker.setWalletScope(QStringLiteral("alice"));
    tracker.trackSend(makeTrackedTransaction(QStringLiteral("alice-tx")));
    tracker.stopPolling();

    QCOMPARE(static_cast<int>(tracker.transactions().size()), 1);
    QCOMPARE(tracker.transactions().front().txid, QStringLiteral("alice-tx"));

    tracker.setWalletScope(QStringLiteral("bob"));
    QCOMPARE(static_cast<int>(tracker.transactions().size()), 0);

    tracker.setWalletScope(QStringLiteral("alice"));
    tracker.stopPolling();
    QCOMPARE(static_cast<int>(tracker.transactions().size()), 1);
    QCOMPARE(tracker.transactions().front().txid, QStringLiteral("alice-tx"));
}

void WalletScopedStateTest::trackerMigratesLegacyStateOnce() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QJsonObject root;
    root["schema_version"] = 1;
    root["transactions"] = QJsonArray{makeTrackedTransactionJson(QStringLiteral("legacy-tx"))};
    const QString legacyPath = tempDir.path() + QStringLiteral("/tracked_transactions.json");
    writeJsonFile(legacyPath, QJsonDocument(root));
    QVERIFY(QFile::exists(legacyPath));

    TransactionTracker tracker(tempDir.path(), nullptr);
    tracker.setWalletScope(QStringLiteral("alice"));
    tracker.stopPolling();

    QCOMPARE(static_cast<int>(tracker.transactions().size()), 1);
    QCOMPARE(tracker.transactions().front().txid, QStringLiteral("legacy-tx"));
    QVERIFY(!QFile::exists(legacyPath));

    tracker.setWalletScope(QStringLiteral("bob"));
    QCOMPARE(static_cast<int>(tracker.transactions().size()), 0);

    tracker.setWalletScope(QStringLiteral("alice"));
    tracker.stopPolling();
    QCOMPARE(static_cast<int>(tracker.transactions().size()), 1);
    QCOMPARE(tracker.transactions().front().txid, QStringLiteral("legacy-tx"));
}

void WalletScopedStateTest::advisoryQueueIsolatesPerWalletState() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    AdvisoryBannerQueue queue(tempDir.path(), nullptr);
    queue.setAutoDisplayEnabled(false);
    queue.setWalletScope(QStringLiteral("alice"));
    queue.enqueue(TerminalAdvisoryEvent{
        QStringLiteral("alice-tx"),
        TxStatus::Evicted,
        QString(),
        QStringLiteral("alice-lineage")
    });

    QCOMPARE(queue.pendingEventCount(), 1);

    queue.setWalletScope(QStringLiteral("bob"));
    QCOMPARE(queue.pendingEventCount(), 0);

    queue.setWalletScope(QStringLiteral("alice"));
    QCOMPARE(queue.pendingEventCount(), 1);
}

void WalletScopedStateTest::advisoryQueueMigratesLegacyStateOnce() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QJsonArray events;
    QJsonObject event;
    event["event_id"] = QStringLiteral("legacy-event");
    event["lineage_key"] = QStringLiteral("legacy-lineage");
    event["txid"] = QStringLiteral("legacy-tx");
    event["type"] = static_cast<int>(TxStatus::Failed);
    event["message"] = QStringLiteral("Legacy warning");
    event["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    events.append(event);

    writeJsonFile(tempDir.path() + QStringLiteral("/advisory_events.json"), QJsonDocument(events));
    writeJsonFile(tempDir.path() + QStringLiteral("/advisory_acknowledged.json"), QJsonDocument(QJsonArray()));

    AdvisoryBannerQueue queue(tempDir.path(), nullptr);
    queue.setAutoDisplayEnabled(false);
    queue.setWalletScope(QStringLiteral("alice"));

    QCOMPARE(queue.pendingEventCount(), 1);
    QVERIFY(!QFile::exists(tempDir.path() + QStringLiteral("/advisory_events.json")));
    QVERIFY(!QFile::exists(tempDir.path() + QStringLiteral("/advisory_acknowledged.json")));

    queue.setWalletScope(QStringLiteral("bob"));
    QCOMPARE(queue.pendingEventCount(), 0);

    queue.setWalletScope(QStringLiteral("alice"));
    QCOMPARE(queue.pendingEventCount(), 1);
}

QTEST_GUILESS_MAIN(WalletScopedStateTest)

#include "test_wallet_scoped_state.moc"
