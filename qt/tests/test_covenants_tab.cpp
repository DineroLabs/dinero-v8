#include <QtTest/QtTest>
#include <QComboBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include "mainwindow.h"
#include "rpcclient.h"

// The Covenants tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in covenants tab test"); }

class CovenantsTabTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    QTcpServer endpoint_;

    void isolate() {
        QVERIFY(dir_.isValid());
        if (!endpoint_.isListening()) QVERIFY(endpoint_.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroCovenantsTabTest");
        QCoreApplication::setApplicationName("IsolatedCovenantsTab");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().setValue("updates/check_enabled", false);
    }
    void pointAtTestEndpoint(MainWindow& window) {
        if (auto* rpc = window.findChild<RpcClient*>()) {
            rpc->setDatadir(dir_.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort())));
        }
    }
    static void openTab(MainWindow& window, const QString& name) {
        auto* tabs = window.findChild<QTabWidget*>();
        QVERIFY(tabs);
        for (int i = 0; i < tabs->count(); ++i) {
            const QString stored = tabs->tabBar()->tabData(i).toString();
            if ((stored.isEmpty() ? tabs->tabText(i) : stored) == name) {
                tabs->setCurrentIndex(i);
                QCoreApplication::processEvents();
                return;
            }
        }
        QFAIL(qPrintable("no tab named " + name));
    }
    template <class T>
    static T* named(MainWindow& window, const char* objectName) {
        return window.findChild<T*>(QString::fromLatin1(objectName));
    }

private Q_SLOTS:
    void initTestCase() { isolate(); }

    void covenantsTabOffersOnlyCovenantKinds() {
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        pointAtTestEndpoint(window);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* mode = named<QComboBox>(window, "sendMode");
        auto* pub = named<QPushButton>(window, "covenantKindPublic");
        auto* priv = named<QPushButton>(window, "covenantKindPrivate");
        QVERIFY(mode && pub && priv);

        openTab(window, "Covenants");
        QVERIFY2(!mode->isVisible(), "the payment Mode menu should not appear on Covenants");
        QVERIFY(pub->isVisible() && priv->isVisible());
        QVERIFY(pub->isChecked());
        QTest::mouseClick(priv, Qt::LeftButton);
        QCOMPARE(mode->currentData().toString(), QString("private_contract"));
        QVERIFY(priv->isChecked() && !pub->isChecked());
        QVERIFY2(pub->isVisible(), "the toggle must stay reachable in private mode");
        QTest::mouseClick(pub, Qt::LeftButton);
        QCOMPARE(mode->currentData().toString(), QString("public_contract"));

        openTab(window, "Send");
        QVERIFY(mode->isVisible());
        QVERIFY(!pub->isVisible() && !priv->isVisible());
    }

    void unavailableTemplatesAreGroupedAsComingSoon() {
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        pointAtTestEndpoint(window);
        auto* combo = named<QComboBox>(window, "contractTemplate");
        QVERIFY(combo);
        auto* model = qobject_cast<QStandardItemModel*>(combo->model());
        QVERIFY(model);
        QStringList selectable;
        int firstDisabledRow = -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            auto* item = model->item(row);
            if (combo->itemData(row).toString().isEmpty()) continue;  // separator
            if (item->isEnabled()) {
                QVERIFY2(firstDisabledRow < 0, "an available template sits below the coming-soon group");
                selectable << item->text();
            } else {
                if (firstDisabledRow < 0) firstDisabledRow = row;
                QVERIFY2(item->text().contains("coming soon"), qPrintable(item->text()));
                QVERIFY(!item->toolTip().isEmpty());
            }
        }
        QCOMPARE(selectable, QStringList({"Simple Lock", "Batch Payment"}));
        QVERIFY(firstDisabledRow > 0);

        // Choosing Batch Payment must show the batch page, not whatever page shares its index.
        auto* stack = named<QStackedWidget>(window, "contractTemplateStack");
        QVERIFY(stack);
        combo->setCurrentIndex(combo->findData("payroll"));
        QVERIFY(stack->currentWidget()->findChild<QTableWidget*>());
        combo->setCurrentIndex(combo->findData("vault"));
        QVERIFY(!stack->currentWidget()->findChild<QTableWidget*>());
    }

    void reviewSummarizesTheContract() {
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        pointAtTestEndpoint(window);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        openTab(window, "Covenants");
        auto* review = named<QGroupBox>(window, "covenantReview");
        auto* locked = named<QLabel>(window, "covenantReviewLocked");
        auto* delivered = named<QLabel>(window, "covenantReviewDelivered");
        auto* status = named<QLabel>(window, "covenantReviewStatus");
        auto* recipient = named<QLineEdit>(window, "sendRecipient");
        auto* amount = named<QLineEdit>(window, "sendAmount");
        QVERIFY(review && locked && delivered && status && recipient && amount);
        QVERIFY(review->isVisible());

        recipient->setText("din1ptestrecipient");
        amount->setText("1.5");
        QVERIFY2(locked->text().contains("1.50000000 DIN"), qPrintable(locked->text()));
        QVERIFY2(delivered->text().contains("1.49999000 DIN"), qPrintable(delivered->text()));
        // No wallet is loaded in this test, which is why Create Contract is greyed.
        QVERIFY2(status->text().contains("Create or load a wallet first"), qPrintable(status->text()));
        QVERIFY2(!status->text().contains("Enter a"), qPrintable(status->text()));

        recipient->clear();
        QVERIFY2(status->text().contains("Enter a recipient address"), qPrintable(status->text()));

        // Private covenants have their own controls; the public review steps aside.
        QTest::mouseClick(named<QPushButton>(window, "covenantKindPrivate"), Qt::LeftButton);
        QVERIFY(!review->isVisible());
    }

    void formAndReviewShareARowLikeOverview() {
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        pointAtTestEndpoint(window);
        for (int width : {1100, 1440, 1920}) {
            window.resize(width, 1000);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            openTab(window, "Covenants");
            QCoreApplication::processEvents();
            auto* form = named<QWidget>(window, "covenantComposerHost");
            auto* review = named<QGroupBox>(window, "covenantReview");
            QVERIFY(form && review);
            const QRect f(form->mapTo(&window, QPoint(0, 0)), form->size());
            const QRect r(review->mapTo(&window, QPoint(0, 0)), review->size());
            QVERIFY2(r.left() > f.right(), "review must sit to the right of the form");
            QCOMPARE(r.top(), f.top());
            const double share = double(f.width()) / double(f.width() + r.width());
            QVERIFY2(share > 0.57 && share < 0.63,
                     qPrintable(QString("form takes %1 of the row at %2 px").arg(share).arg(width)));
        }
    }

    void emptyContractsListShowsAMessage() {
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        pointAtTestEndpoint(window);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        openTab(window, "Covenants");
        auto* rpc = window.findChild<RpcClient*>();
        auto* empty = named<QLabel>(window, "contractsEmpty");
        auto* table = named<QTableWidget>(window, "contractsTable");
        QVERIFY(rpc && empty && table);

        Q_EMIT rpc->rpcResult("wallet.covenant.list", QJsonObject{{"descriptors", QJsonArray{}}});
        QVERIFY(empty->isVisible());
        QVERIFY(!table->isVisible());

        Q_EMIT rpc->rpcResult("wallet.covenant.list", QJsonObject{{"descriptors", QJsonArray{
            QJsonObject{{"profile", "vault"}, {"label", "Savings"}, {"created_at", 1790000000},
                        {"recovery_descriptor", "tr(x)"}}}}});
        QCoreApplication::processEvents();
        QVERIFY(!empty->isVisible());
        QVERIFY(table->isVisible());
        int used = 0;
        for (int c = 0; c < table->columnCount(); ++c) used += table->horizontalHeader()->sectionSize(c);
        QVERIFY2(used >= table->viewport()->width() - 2,
                 qPrintable(QString("columns use %1 of %2 px").arg(used).arg(table->viewport()->width())));
    }
};
QTEST_MAIN(CovenantsTabTest)
#include "test_covenants_tab.moc"
