#include <QtTest/QtTest>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include "mainwindow.h"
#include "rpcclient.h"

// The Wallet tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in wallet layout test"); }

class WalletLayoutTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QTcpServer endpoint_;

    template <class T>
    static T* named(QWidget& w, const char* name) { return w.findChild<T*>(QString::fromLatin1(name)); }

    std::unique_ptr<MainWindow> makeWindow(int width = 1440, int height = 1000) {
        auto window = std::make_unique<MainWindow>(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        if (auto* rpc = window->findChild<RpcClient*>()) {
            rpc->setDatadir(dir_.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort())));
        }
        window->resize(width, height);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get())) return nullptr;
        openTab(*window, "Wallet");
        return window;
    }
    static void openTab(MainWindow& window, const QString& name) {
        auto* tabs = window.findChild<QTabWidget*>();
        for (int i = 0; i < tabs->count(); ++i) {
            const QString stored = tabs->tabBar()->tabData(i).toString();
            if ((stored.isEmpty() ? tabs->tabText(i) : stored) == name) tabs->setCurrentIndex(i);
        }
        QCoreApplication::processEvents();
    }
    // The node's wallet.getbalance reply: 636,799.99999731 DIN total in the example
    // the owner saw, 7% of it quantum-safe, 100 DIN maturing, 2.5 DIN unconfirmed.
    static void feedBalance(MainWindow& window) {
        auto* rpc = window.findChild<RpcClient*>();
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"confirmed", 636799.99999731}, {"unconfirmed", 2.5}, {"immature", 100.0},
            {"pq_balance_din", 42900.0}});
        QCoreApplication::processEvents();
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(endpoint_.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroWalletLayoutTest");
        QCoreApplication::setApplicationName("IsolatedWalletLayout");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().setValue("updates/check_enabled", false);
    }

    void balanceReadsWithThousandsSeparators() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("636,799.99999731 DIN"));
        QCOMPARE(named<QLabel>(*window, "lblTransparentTaprootBalance")->text(), QString("593,899.99999731 DIN"));
        QCOMPARE(named<QLabel>(*window, "lblTransparentP2mrBalance")->text(), QString("42,900.00000000 DIN"));
    }

    void maxStillSendsTheWholeBalance() {
        // Max reads the balance back from its label; separators must not change the amount.
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        openTab(*window, "Send");
        auto* max = named<QPushButton>(*window, "sendMax");
        QVERIFY(max);
        max->setEnabled(true);
        QTest::mouseClick(max, Qt::LeftButton);
        QCOMPARE(named<QLineEdit>(*window, "sendAmount")->text(), QString("636799.99998731"));
    }

    void columnsAndNotYetSpendable() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* publicColumn = named<QWidget>(*window, "walletPublicColumn");
        auto* privateColumn = named<QWidget>(*window, "walletPrivateColumn");
        auto* pendingRow = named<QWidget>(*window, "walletNotYetSpendable");
        QVERIFY(publicColumn && privateColumn && pendingRow);
        // Public and private side by side, like the Overview's columns.
        const QPoint pub = publicColumn->mapTo(window.get(), QPoint(0, 0));
        const QPoint priv = privateColumn->mapTo(window.get(), QPoint(0, 0));
        QCOMPARE(pub.y(), priv.y());
        QVERIFY(priv.x() > pub.x() + publicColumn->width() - 1);
        // Maturing mining rewards and unconfirmed funds are not shielded money.
        auto* mining = named<QLabel>(*window, "lblImmature");
        auto* pending = named<QLabel>(*window, "lblUnconfirmed");
        QVERIFY(mining && pending);
        QVERIFY(!privateColumn->isAncestorOf(mining));
        QVERIFY(!privateColumn->isAncestorOf(pending));
        QVERIFY(pendingRow->isAncestorOf(mining) && pendingRow->isAncestorOf(pending));
        QCOMPARE(mining->text(), QString("100.00000000 DIN"));
        QCOMPARE(pending->text(), QString("+2.50000000 DIN"));
        // Amounts line up on the right edge of their column.
        QVERIFY(named<QLabel>(*window, "lblTransparentTaprootBalance")->alignment() & Qt::AlignRight);
        QVERIFY(named<QLabel>(*window, "lblShieldedBalance")->alignment() & Qt::AlignRight);
    }

    void quantumSafeShareIsANudgeNotAnAlarm() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);  // 7% quantum-safe
        auto* bar = window->findChild<QProgressBar*>("walletPqBar");
        QVERIFY(bar);
        QCOMPARE(bar->value(), 7);
        QVERIFY2(!bar->styleSheet().contains("#c0392b"), "the low share is still painted alarm red");
        QVERIFY2(bar->styleSheet().contains("#f0b429"), qPrintable(bar->styleSheet()));
    }

    void seedExplainerIsOneLineUntilAsked() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* details = named<QLabel>(*window, "walletSeedDetails");
        auto* about = named<QPushButton>(*window, "walletSeedAbout");
        QVERIFY(details && about);
        QVERIFY(!details->isVisible());
        QTest::mouseClick(about, Qt::LeftButton);
        QVERIFY(details->isVisible());
        bool backup = false;
        for (auto* b : window->findChildren<QPushButton*>())
            if (b->text().contains("Seed Backup") && b->isVisible()) backup = true;
        QVERIFY2(backup, "Seed Backup / Mobile Restore must stay one click away");
    }

    void walletButtonsShareOneRowAndSize() {
        auto window = makeWindow();
        QVERIFY(window);
        QList<QPushButton*> buttons;
        for (const char* name : {"walletLoad", "walletLock", "walletEncrypt", "walletRescan", "walletCreate"}) {
            auto* b = named<QPushButton>(*window, name);
            QVERIFY2(b, name);
            if (b->isVisible()) buttons << b;  // Load is hidden in some wallet states
        }
        QVERIFY(buttons.size() >= 4);
        const int y = buttons.first()->mapTo(window.get(), QPoint(0, 0)).y();
        const int w = buttons.first()->width();
        for (auto* b : buttons) {
            QCOMPARE(b->mapTo(window.get(), QPoint(0, 0)).y(), y);
            QVERIFY2(qAbs(b->width() - w) <= 2,
                     qPrintable(QString("'%1' is %2 px, others %3 px").arg(b->text()).arg(b->width()).arg(w)));
        }
    }

    void noEmptyStripsOrStretchedBoxes() {
        auto window = makeWindow(1440, 2000);  // much taller than the content
        QVERIFY(window);
        feedBalance(*window);
        auto* assets = named<QLabel>(*window, "lblAssets");
        QVERIFY(assets);
        QVERIFY2(!assets->isVisible(), "an empty assets line still draws a dark strip");
        for (auto* box : window->findChildren<QGroupBox*>()) {
            if (!box->isVisible()) continue;
            const QString title = box->title();
            if (!title.contains("Balance") && !title.contains("Receive Address") && !title.contains("HD Wallet"))
                continue;
            QVERIFY2(box->height() <= box->sizeHint().height() + 4,
                     qPrintable(QString("%1 is %2 px tall, needs %3").arg(title).arg(box->height())
                                    .arg(box->sizeHint().height())));
        }
    }
};
QTEST_MAIN(WalletLayoutTest)
#include "test_wallet_layout.moc"
