#include <QtTest/QtTest>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextEdit>
#include "mainwindow.h"
#include "rpcclient.h"

// The Mining tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in mining output zoom test"); }

class MiningOutputZoomTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QTcpServer endpoint_;

    std::unique_ptr<MainWindow> makeWindow() {
        auto window = std::make_unique<MainWindow>(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        if (auto* rpc = window->findChild<RpcClient*>()) {
            rpc->setDatadir(dir_.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort())));
        }
        window->resize(1440, 1000);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get())) return nullptr;
        auto* tabs = window->findChild<QTabWidget*>();
        for (int i = 0; i < tabs->count(); ++i) {
            const QString stored = tabs->tabBar()->tabData(i).toString();
            if ((stored.isEmpty() ? tabs->tabText(i) : stored) == "Mining") tabs->setCurrentIndex(i);
        }
        QCoreApplication::processEvents();
        return window;
    }
    static QTextEdit* output(MainWindow& w) { return w.findChild<QTextEdit*>("miningOutput"); }
    static int fontPx(QTextEdit* out) {
        const QRegularExpression re("font-size:\\s*(\\d+)px");
        const auto m = re.match(out->styleSheet());
        return m.hasMatch() ? m.captured(1).toInt() : -1;
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(endpoint_.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroMiningZoomTest");
        QCoreApplication::setApplicationName("IsolatedMiningZoom");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().setValue("updates/check_enabled", false);
    }
    void init() { QSettings().remove("mining/outputFontPx"); }

    void buttonsSitInTheTopRightCorner() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* out = output(*window);
        auto* minus = window->findChild<QPushButton*>("miningZoomOut");
        auto* plus = window->findChild<QPushButton*>("miningZoomIn");
        QVERIFY(out && minus && plus);
        QVERIFY(minus->isVisible() && plus->isVisible());
        const QRect area(out->mapTo(window.get(), QPoint(0, 0)), out->size());
        const QRect p(plus->mapTo(window.get(), QPoint(0, 0)), plus->size());
        const QRect m(minus->mapTo(window.get(), QPoint(0, 0)), minus->size());
        QVERIFY2(area.right() - p.right() <= 30, "+ is not at the right edge");
        QVERIFY2(p.top() - area.top() <= 30, "+ is not on the top line");
        QVERIFY2(m.right() < p.left() && qAbs(m.top() - p.top()) <= 2, "- should sit just left of +");
    }

    void buttonsChangeTheTextSizeAndRememberIt() {
        {
            auto window = makeWindow();
            QVERIFY(window);
            auto* out = output(*window);
            QCOMPARE(fontPx(out), 10);
            QTest::mouseClick(window->findChild<QPushButton*>("miningZoomIn"), Qt::LeftButton);
            QCOMPARE(fontPx(out), 11);
            QTest::mouseClick(window->findChild<QPushButton*>("miningZoomOut"), Qt::LeftButton);
            QTest::mouseClick(window->findChild<QPushButton*>("miningZoomOut"), Qt::LeftButton);
            QCOMPARE(fontPx(out), 9);
            QCOMPARE(out->font().pixelSize(), 9);  // the stylesheet sets it in px
        }
        auto again = makeWindow();
        QVERIFY(again);
        QCOMPARE(fontPx(output(*again)), 9);  // remembered across launches
    }

    void sizeStaysWithinReadableBounds() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* plus = window->findChild<QPushButton*>("miningZoomIn");
        auto* minus = window->findChild<QPushButton*>("miningZoomOut");
        for (int i = 0; i < 20; ++i) QTest::mouseClick(minus, Qt::LeftButton);
        QCOMPARE(fontPx(output(*window)), 6);
        QVERIFY(!minus->isEnabled());
        for (int i = 0; i < 30; ++i) QTest::mouseClick(plus, Qt::LeftButton);
        QCOMPARE(fontPx(output(*window)), 18);
        QVERIFY(!plus->isEnabled());
    }

    void commandPlusAndMinusWorkOnTheMiningOutput() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* out = output(*window);
        out->setFocus();
        QCoreApplication::processEvents();
        QTest::keyClick(out, Qt::Key_Plus, Qt::ControlModifier);   // Cmd + on macOS
        QCOMPARE(fontPx(out), 11);
        QTest::keyClick(out, Qt::Key_Equal, Qt::ControlModifier);  // Cmd = (the + key unshifted)
        QCOMPARE(fontPx(out), 12);
        QTest::keyClick(out, Qt::Key_Minus, Qt::ControlModifier);  // Cmd -
        QCOMPARE(fontPx(out), 11);
    }
};
QTEST_MAIN(MiningOutputZoomTest)
#include "test_mining_output_zoom.moc"
