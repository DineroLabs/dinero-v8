#include <QtTest/QtTest>

#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>

#include "rpcclient.h"
#include "shieldedwidget.h"

class ShieldedWidgetLockTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void lockedWalletIsDefinitiveAndRetryUnlocks();
    void staleJournalNeedsExplicitClearBeforeRetry();
    void transportFailureRemainsUncertain();
};

namespace {
void seedSubmittingJournals() {
    QSettings s;
    const QString operationRoot = "shielded/operationJournal/India/";
    for (const QString& operation : {QString("shield"), QString("unshield")}) {
        s.setValue(operationRoot + operation + "/stage", "submitting");
        s.setValue(operationRoot + operation + "/amountUna", 2000000000LL);
        s.setValue(operationRoot + operation + "/feeUna", 0);
    }
    const QString transferRoot = "shielded/transferJournal/India";
    s.setValue(transferRoot + "/stage", "submitting");
    s.setValue(transferRoot + "/address", "dins1testrecipient");
    s.setValue(transferRoot + "/amountUna", 100000000LL);
    s.setValue(transferRoot + "/feeUna", 0);
    s.sync();
}

void configureSettings(const QString& path) {
    QCoreApplication::setOrganizationName("DineroShieldedWidgetLockTest");
    QCoreApplication::setApplicationName("IsolatedState");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
    QSettings().clear();
}
}  // namespace

void ShieldedWidgetLockTest::lockedWalletIsDefinitiveAndRetryUnlocks() {
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    configureSettings(settingsDir.path());
    seedSubmittingJournals();

    QTcpServer endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    RpcClient rpc;
    rpc.setDatadir(settingsDir.path());
    rpc.setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));

    ShieldedWidget widget(&rpc);
    widget.setWalletScope("India");
    Q_EMIT rpc.rpcResult("wallet.shieldedbalance", QJsonObject{{"spend_enabled", true}});

    auto* shield = widget.findChild<QPushButton*>("shieldButton");
    auto* transfer = widget.findChild<QPushButton*>("shieldedTransferButton");
    auto* unshield = widget.findChild<QPushButton*>("unshieldButton");
    QVERIFY(shield && transfer && unshield);
    QVERIFY(!shield->isEnabled());
    QVERIFY(!transfer->isEnabled());
    QVERIFY(!unshield->isEnabled());
    QVERIFY(widget.findChild<QLabel*>("shieldedStatusBanner")->text().contains("unlock wallet"));

    widget.setWalletUnlocked(true);
    QVERIFY(shield->isEnabled());
    QVERIFY(transfer->isEnabled());
    QVERIFY(unshield->isEnabled());
    QCOMPARE(shield->text(), QString("Review Outcome"));
    QCOMPARE(transfer->text(), QString("Review Outcome"));
    QCOMPARE(unshield->text(), QString("Review Outcome"));

    Q_EMIT rpc.rpcError("wallet.shield", -32603, "wallet_locked");
    Q_EMIT rpc.rpcError("wallet.transfer", -32603, "wallet_locked");
    Q_EMIT rpc.rpcError("wallet.unshield", -32603, "wallet_locked");

    const QString guidance = "Wallet is locked — unlock wallet to continue.";
    QCOMPARE(widget.findChild<QLabel*>("shieldResult")->text(), guidance);
    QCOMPARE(widget.findChild<QLabel*>("shieldedTransferResult")->text(), guidance);
    QCOMPARE(widget.findChild<QLabel*>("unshieldResult")->text(), guidance);
    QVERIFY(widget.findChild<QLabel*>("shieldedStatusBanner")->text().contains("unlock wallet"));
    QCOMPARE(QSettings().value("shielded/operationJournal/India/shield/stage").toString(),
             QString("rejected"));
    QCOMPARE(QSettings().value("shielded/transferJournal/India/stage").toString(),
             QString("rejected"));
    QCOMPARE(QSettings().value("shielded/operationJournal/India/unshield/stage").toString(),
             QString("rejected"));
    QVERIFY(!shield->isEnabled());
    QVERIFY(!transfer->isEnabled());
    QVERIFY(!unshield->isEnabled());

    widget.setWalletUnlocked(true);
    QVERIFY(shield->isEnabled());
    QVERIFY(transfer->isEnabled());
    QVERIFY(unshield->isEnabled());
    QCOMPARE(shield->text(), QString("Review and Retry"));
    QCOMPARE(transfer->text(), QString("Review and Retry"));
    QCOMPARE(unshield->text(), QString("Review and Retry"));
}

void ShieldedWidgetLockTest::transportFailureRemainsUncertain() {
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    configureSettings(settingsDir.path());
    seedSubmittingJournals();

    QTcpServer endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    RpcClient rpc;
    rpc.setDatadir(settingsDir.path());
    rpc.setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));

    ShieldedWidget widget(&rpc);
    widget.setWalletScope("India");
    widget.setWalletUnlocked(true);
    Q_EMIT rpc.rpcResult("wallet.shieldedbalance", QJsonObject{{"spend_enabled", true}});
    Q_EMIT rpc.rpcError("wallet.unshield", -1, "connection lost");

    auto* unshield = widget.findChild<QPushButton*>("unshieldButton");
    QVERIFY(unshield);
    QVERIFY(!unshield->isEnabled());
    QVERIFY(widget.findChild<QLabel*>("unshieldResult")->text().contains("outcome uncertain"));
    QCOMPARE(QSettings().value("shielded/operationJournal/India/unshield/stage").toString(),
             QString("submitting"));
}

void ShieldedWidgetLockTest::staleJournalNeedsExplicitClearBeforeRetry() {
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    configureSettings(settingsDir.path());
    seedSubmittingJournals();

    QTcpServer endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    RpcClient rpc;
    rpc.setDatadir(settingsDir.path());
    rpc.setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));

    ShieldedWidget widget(&rpc);
    widget.setWalletScope("India");
    widget.setWalletUnlocked(true);
    Q_EMIT rpc.rpcResult("wallet.shieldedbalance", QJsonObject{{"spend_enabled", true}});

    auto* unshield = widget.findChild<QPushButton*>("unshieldButton");
    auto* result = widget.findChild<QLabel*>("unshieldResult");
    QVERIFY(unshield && result);
    QCOMPARE(unshield->text(), QString("Review Outcome"));
    QVERIFY(result->text().contains("Nothing will be sent"));

    unshield->click();
    QVERIFY(!QSettings().contains("shielded/operationJournal/India/unshield/stage"));
    QCOMPARE(unshield->text(), QString("Unshield"));
    QVERIFY(result->text().contains("cleared"));
    QVERIFY(unshield->isEnabled());
}

QTEST_MAIN(ShieldedWidgetLockTest)
#include "test_shielded_widget_lock.moc"
