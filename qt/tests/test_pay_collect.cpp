#include <QtTest/QtTest>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextEdit>
#include "dpiwidget.h"
#include "rpcclient.h"

// Constructing the widget must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in pay/collect test"); }

namespace {
QStringList visibleWording(QWidget* root) {
    QStringList out;
    for (auto* g : root->findChildren<QGroupBox*>()) out << g->title();
    for (auto* l : root->findChildren<QLabel*>()) out << l->text();
    for (auto* b : root->findChildren<QPushButton*>()) out << b->text();
    for (auto* e : root->findChildren<QLineEdit*>()) out << e->placeholderText();
    for (auto* e : root->findChildren<QTextEdit*>()) out << e->placeholderText();
    return out;
}
QStringList materialColours(QWidget* root) {
    QStringList hits;
    const QStringList material = {"#4CAF50", "#FF9800", "#2196F3", "#f44336", "#999;", "#999 "};
    QList<QWidget*> all = root->findChildren<QWidget*>();
    all << root;
    for (auto* w : all) {
        QString text = w->styleSheet();
        if (auto* l = qobject_cast<QLabel*>(w)) text += l->text();
        for (const QString& c : material)
            if (text.contains(c, Qt::CaseInsensitive)) hits << w->metaObject()->className() + (": " + c);
    }
    return hits;
}
}  // namespace

class PayCollectTest : public QObject {
    Q_OBJECT
    template <class T>
    static T* named(QWidget& w, const char* name) { return w.findChild<T*>(QString::fromLatin1(name)); }

private Q_SLOTS:
    void noDpiJargon() {
        RpcClient rpc;
        DpiWidget widget(&rpc);
        for (const QString& text : visibleWording(&widget)) {
            QString withoutAppName = text;
            withoutAppName.remove("DineroDPI");
            QVERIFY2(!withoutAppName.contains("DPI"), qPrintable(text));
        }
    }

    void usesTheAppColours() {
        RpcClient rpc;
        DpiWidget widget(&rpc);
        QVERIFY2(materialColours(&widget).isEmpty(), qPrintable(materialColours(&widget).join(", ")));
        // Results and status badges too, not only the idle screen.
        Q_EMIT rpc.rpcResult("dpi.createinvoice", QJsonObject{
            {"invoice", "aW52"}, {"invoice_id", "id1"}, {"destination", "din1pdest"},
            {"amount_din", 50.0}, {"timestamp", double(QDateTime::currentSecsSinceEpoch())},
            {"expiry_seconds", 900}});
        Q_EMIT rpc.rpcResult("dpi.verifypackage", QJsonObject{
            {"tier", "T1"}, {"risk_score", 0.1}, {"txid", "tx1"},
            {"checks", QJsonObject{{"invoice_bound", true}, {"amount_match", true}}}});
        Q_EMIT rpc.rpcResult("dpi.decodeinvoice", QJsonObject{
            {"amount_din", 5.0}, {"destination_address", "din1px"}, {"expired", true}});
        Q_EMIT rpc.rpcResult("dpi.payinvoice", QJsonObject{{"txid", "tx2"}, {"package", "cGtn"}});
        QVERIFY2(materialColours(&widget).isEmpty(), qPrintable(materialColours(&widget).join(", ")));
        for (const QString& text : visibleWording(&widget))
            QVERIFY2(!text.contains(QRegularExpression("\\bT[0-3] —")), qPrintable(text));
    }

    void emptyResultsStayHidden() {
        RpcClient rpc;
        DpiWidget widget(&rpc);
        widget.resize(1440, 1000);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        auto* verifyResult = named<QWidget>(widget, "collectVerifyResult");
        auto* verifyDetails = named<QWidget>(widget, "collectVerifyDetails");
        auto* invoicePlaceholder = named<QLabel>(widget, "collectInvoicePlaceholder");
        auto* pasteLabel = named<QLabel>(widget, "collectPastePrompt");
        QVERIFY(verifyResult && verifyDetails && invoicePlaceholder && pasteLabel);
        QVERIFY(!verifyResult->isVisible());
        QVERIFY(!verifyDetails->isVisible());
        QVERIFY(invoicePlaceholder->isVisible());
        QVERIFY2(pasteLabel->height() <= 2 * pasteLabel->fontMetrics().height() + 16,
                 qPrintable(QString("prompt label is %1 px tall").arg(pasteLabel->height())));

        Q_EMIT rpc.rpcResult("dpi.verifypackage", QJsonObject{{"tier", "T0"}, {"checks", QJsonObject{}}});
        QVERIFY(verifyResult->isVisible());
        QVERIFY(verifyDetails->isVisible());

        auto* tabs = widget.findChild<QTabWidget*>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);  // Pay
        auto* proof = named<QGroupBox>(widget, "payPackageGroup");
        QVERIFY(proof);
        QVERIFY(!proof->isVisible());
        Q_EMIT rpc.rpcResult("dpi.payinvoice", QJsonObject{{"txid", "tx2"}, {"package", "cGtn"}});
        QVERIFY(proof->isVisible());
    }

    void buttonsSayWhatIsMissing() {
        RpcClient rpc;
        DpiWidget widget(&rpc);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        auto* create = named<QPushButton>(widget, "collectCreate");
        auto* createHint = named<QLabel>(widget, "collectCreateHint");
        auto* amount = named<QLineEdit>(widget, "collectAmount");
        QVERIFY(create && createHint && amount);
        QVERIFY(!create->isEnabled());
        QVERIFY2(createHint->text().contains("Enter an amount"), qPrintable(createHint->text()));
        amount->setText("50");
        QVERIFY(create->isEnabled());
        QVERIFY(!createHint->isVisible());
        amount->setText("0");
        QVERIFY(!create->isEnabled());
        QVERIFY(createHint->text().contains("more than 0"));

        auto* verify = named<QPushButton>(widget, "collectVerify");
        auto* verifyHint = named<QLabel>(widget, "collectVerifyHint");
        QVERIFY(verify && verifyHint);
        QVERIFY(!verify->isEnabled());
        QVERIFY2(verifyHint->text().contains("Create an invoice first"), qPrintable(verifyHint->text()));

        auto* tabs = widget.findChild<QTabWidget*>();
        tabs->setCurrentIndex(1);
        auto* review = named<QPushButton>(widget, "payReview");
        auto* reviewHint = named<QLabel>(widget, "payReviewHint");
        auto* invoice = named<QTextEdit>(widget, "payInvoiceInput");
        QVERIFY(review && reviewHint && invoice);
        QVERIFY(!review->isEnabled());
        QVERIFY2(reviewHint->text().contains("Paste an invoice"), qPrintable(reviewHint->text()));
        invoice->setPlainText("aW52b2ljZQ==");
        QVERIFY(review->isEnabled());
        QVERIFY(!reviewHint->isVisible());
    }

    void formAndInvoiceShareARow() {
        RpcClient rpc;
        DpiWidget widget(&rpc);
        for (int width : {1100, 1440, 1920}) {
            widget.resize(width, 1000);
            widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&widget));
            QCoreApplication::processEvents();
            auto* form = named<QWidget>(widget, "collectForm");
            auto* card = named<QWidget>(widget, "collectInvoiceCard");
            QVERIFY(form && card);
            const QRect f(form->mapTo(&widget, QPoint(0, 0)), form->size());
            const QRect c(card->mapTo(&widget, QPoint(0, 0)), card->size());
            QVERIFY2(c.left() > f.right(), "invoice card must sit to the right of the form");
            QCOMPARE(c.top(), f.top());
            const double share = double(f.width()) / double(f.width() + c.width());
            QVERIFY2(share > 0.57 && share < 0.63,
                     qPrintable(QString("form takes %1 of the row at %2 px").arg(share).arg(width)));
        }
    }
};
QTEST_MAIN(PayCollectTest)
#include "test_pay_collect.moc"
