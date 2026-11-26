#include <QtTest/QtTest>
#include <QApplication>
#include <QPushButton>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
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

    // The status line is the tagline only, whatever the wallet's state: it
    // used to read "Not-Mining - … - Post-Quantum Native | No wallet - enter
    // address manually or create wallet", and kept that after an address was typed.
    void statusLineIsTheTaglineInEveryWalletState() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* status = window->findChild<QLabel*>("miningStatusLine");
        QVERIFY(status);
        const QString tagline = "Dinero: Real Money For Free People";
        QCOMPARE(status->text(), tagline);

        window->walletSwitchInFlight_ = false;
        window->walletRescanning_ = false;
        window->edtMiningAddress_->clear();
        window->currentWalletName_.clear();  // no wallet
        window->walletUnlocked_ = false;
        window->updateWalletUIState();
        QCOMPARE(status->text(), tagline);

        window->currentWalletName_ = "Hotel";  // a wallet, locked
        window->updateWalletUIState();
        QCOMPARE(status->text(), tagline);

        window->walletUnlocked_ = true;  // unlocked
        window->updateWalletUIState();
        QCOMPARE(status->text(), tagline);
        // What the wallet's state means for the button is still said, on the button.
        QVERIFY(!window->btnUseWalletAddr_->toolTip().isEmpty());
    }

    void useWalletSelectsFirstTaprootAddressWithoutReceiveTab() {
        auto window = makeWindow();
        QVERIFY(window);
        window->currentWalletName_ = "Hotel";
        window->walletUnlocked_ = true;
        window->walletSwitchInFlight_ = false;
        window->walletRescanning_ = false;
        window->tblAddresses_->setRowCount(0);
        window->updateWalletUIState();

        window->edtMiningAddress_->setText("din1pmanual");
        window->onSetMiningAddress();
        QVERIFY(window->miningWalletAddressPending_);
        QCOMPARE(window->edtMiningAddress_->text(), QString("din1pmanual"));

        const QJsonArray addresses{
            QJsonObject{{"address", "din1plater"}, {"account", 0}, {"change", 0}, {"index", 8}},
            QJsonObject{{"address", "din1rquantum"}, {"account", 0}, {"change", 0}, {"index", 0}},
            QJsonObject{{"address", "din1pimported"}, {"account", -1}, {"change", 0}, {"index", 0}},
            QJsonObject{{"address", "din1pchange"}, {"account", 0}, {"change", 1}, {"index", 0}},
            QJsonObject{{"address", "din1pfirst"}, {"account", 0}, {"change", 0}, {"index", 0}}
        };
        window->onRpcResult("mining.use_wallet_address", addresses);
        QVERIFY(!window->miningWalletAddressPending_);
        QCOMPARE(window->edtMiningAddress_->text(), QString("din1pfirst"));
        QCOMPARE(window->tblAddresses_->rowCount(), 0);

        // A paste made after the click wins over the delayed RPC reply.
        window->onSetMiningAddress();
        window->edtMiningAddress_->setText("din1pnewmanual");
        window->onRpcResult("mining.use_wallet_address", addresses);
        QCOMPARE(window->edtMiningAddress_->text(), QString("din1pnewmanual"));
    }

    void useWalletDoesNotFallBackToAnotherAddress() {
        auto window = makeWindow();
        QVERIFY(window);
        window->currentWalletName_ = "Hotel";
        window->walletUnlocked_ = true;
        window->walletSwitchInFlight_ = false;
        window->walletRescanning_ = false;
        window->edtMiningAddress_->setText("din1pmanual");
        window->onSetMiningAddress();
        QVERIFY(window->miningWalletAddressPending_);

        QTimer::singleShot(0, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget)) box->accept();
            }
        });
        window->onRpcResult("mining.use_wallet_address",
            QJsonArray{QJsonObject{{"address", "din1plater"},
                                   {"account", 0}, {"change", 0}, {"index", 1}}});
        QVERIFY(!window->miningWalletAddressPending_);
        QCOMPARE(window->edtMiningAddress_->text(), QString("din1pmanual"));
    }

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

    void controlsFollowOutputAcrossWideResize() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* out = output(*window);
        auto* plus = window->findChild<QPushButton*>("miningZoomIn");
        auto* minus = window->findChild<QPushButton*>("miningZoomOut");
        auto* red = window->findChild<QPushButton*>("miningHashColorRed");
        QVERIFY(out && plus && minus && red);
        const int originalWidth = out->width();
        const auto atRightEdge = [&] {
            const QRect area(out->mapTo(window.get(), QPoint(0, 0)), out->size());
            const QRect button(plus->mapTo(window.get(), QPoint(0, 0)), plus->size());
            return area.right() - button.right() >= 0 && area.right() - button.right() <= 30;
        };
        window->resize(3000, 1100);
        QTRY_VERIFY(out->width() > originalWidth + 500);
        QTRY_VERIFY(atRightEdge());
        QVERIFY(red->geometry().right() < minus->geometry().left());
        QVERIFY(minus->geometry().right() < plus->geometry().left());
        window->resize(1440, 1000);
        QTRY_VERIFY(atRightEdge());
    }

    void hashColorsSwitchImmediatelyAndResetOnNewWindow() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* out = output(*window);
        auto* zoom = window->findChild<QPushButton*>("miningZoomIn");
        auto* minus = window->findChild<QPushButton*>("miningZoomOut");
        QVERIFY(out && zoom && minus);
        QCOMPARE(out->grab().toImage().pixelColor(out->width() / 2, 10), QColor("#000000"));
        const struct { const char* name; quint32 rgb; } choices[] = {
            {"miningHashColorGreen", 0x00ff00},
            {"miningHashColorBlue", 0x489cff},
            {"miningHashColorWhite", 0xffffff},
            {"miningHashColorRed", 0xff5050},
        };
        QPushButton* previous = nullptr;
        for (const auto& choice : choices) {
            auto* button = window->findChild<QPushButton*>(choice.name);
            QVERIFY(button);
            QVERIFY(button->isVisible());
            QCOMPARE(button->size(), QSize(12, 20));
            QCOMPARE(button->iconSize(), QSize(12, 12));
            QVERIFY(button->x() > out->width() / 2);
            QVERIFY(button->geometry().right() < minus->geometry().left());
            QCOMPARE(button->y(), zoom->y());
            const QColor center = button->grab().toImage().pixelColor(button->width() / 2,
                                                                       button->height() / 2);
            QCOMPARE(center.red(), int((choice.rgb >> 16) & 0xff));
            QCOMPARE(center.green(), int((choice.rgb >> 8) & 0xff));
            QCOMPARE(center.blue(), int(choice.rgb & 0xff));
            if (previous) {
                QVERIFY(previous->geometry().right() < button->geometry().left());
                QVERIFY(button->geometry().left() - previous->geometry().right() <= 2);
            }
            previous = button;
        }
        QCOMPARE(window->miningHashColorRgb_, 0x00ff00u);
        QVERIFY(window->findChild<QPushButton*>("miningHashColorGreen")->isChecked());
        for (const auto& choice : choices) {
            auto* button = window->findChild<QPushButton*>(choice.name);
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(window->miningHashColorRgb_, choice.rgb);
            QVERIFY(button->isChecked());
        }
        QTest::mouseClick(window->findChild<QPushButton*>("miningHashColorRed"), Qt::LeftButton);
        QCOMPARE(window->miningHashColorRgb_, 0xff5050u);
        window.reset();

        auto again = makeWindow();
        QVERIFY(again);
        QCOMPARE(again->miningHashColorRgb_, 0x00ff00u);
        QVERIFY(again->findChild<QPushButton*>("miningHashColorGreen")->isChecked());
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
