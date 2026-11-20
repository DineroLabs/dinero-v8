#include <QtTest/QtTest>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include "hardwarewalletwidget.h"
#include "rpcclient.h"

// Constructing the widget must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in hardware wallet layout test"); }

class HardwareWalletLayoutTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void transportAndActionsUseConsistentLineIcons() {
        RpcClient rpc;
        HardwareWalletWidget widget(&rpc);
        auto* tabs = widget.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), QString("File / SD Card"));
        QCOMPARE(tabs->tabText(1), QString("QR Code"));
        QCOMPARE(tabs->tabText(2), QString("USB"));
        for (int i = 0; i < tabs->count(); ++i) {
            QVERIFY(!tabs->tabIcon(i).isNull());
            QVERIFY(!tabs->tabIcon(i).pixmap(20, 20).isNull());
        }
        const QStringList actions = {
            "Export Dinero PSBT to File", "Import Signed Dinero PSBT",
            "Analyze Dinero PSBT", "Sign via Connected USB",
            "Finalize Dinero PSBT", "Broadcast Transaction",
            "Show QR Code", "Scan Signed QR", "Detect Devices",
            "Connect", "Get Address", "Verify on Device",
            "Export Account Descriptors", "Import Into Active Wallet"
        };
        for (const QString& action : actions) {
            QPushButton* match = nullptr;
            for (auto* button : widget.findChildren<QPushButton*>()) {
                if (button->text() == action) { match = button; break; }
            }
            QVERIFY2(match, qPrintable(action));
            QVERIFY2(!match->icon().isNull(), qPrintable(action));
        }
    }

    void spareHeightStaysBelowTheContent() {
        RpcClient rpc;
        HardwareWalletWidget widget(&rpc);
        widget.resize(1440, 2400);  // much taller than any sub-tab's content
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        auto* tabs = widget.findChild<QTabWidget*>();
        QVERIFY(tabs);
        for (int t = 0; t < tabs->count(); ++t) {
            tabs->setCurrentIndex(t);
            QCoreApplication::processEvents();
            QWidget* page = tabs->widget(t);
            for (auto* box : page->findChildren<QGroupBox*>()) {
                if (!box->isVisible()) continue;
                QVERIFY2(box->height() <= box->sizeHint().height() + 4,
                         qPrintable(QString("%1 / %2 is %3 px tall, needs %4").arg(tabs->tabText(t), box->title())
                                        .arg(box->height()).arg(box->sizeHint().height())));
            }
        }
        QLabel* psbtLabel = nullptr;
        for (auto* l : widget.findChildren<QLabel*>())
            if (l->text() == "Dinero PSBT (Base64):") psbtLabel = l;
        QVERIFY(psbtLabel);
        tabs->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY2(psbtLabel->height() <= 2 * psbtLabel->fontMetrics().height() + 16,
                 qPrintable(QString("PSBT label is %1 px tall").arg(psbtLabel->height())));
    }
};
QTEST_MAIN(HardwareWalletLayoutTest)
#include "test_hardware_wallet_layout.moc"
