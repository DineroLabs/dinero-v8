#include <QtTest/QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "poolpanel.h"
#include "pooltokenstore.h"
#include "rpcclient.h"

// Constructing the panel must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in pool cockpit test"); }

namespace {

const QString kFeeAddress = "din1pyqsjygeyy5nzw2pf9g4jctfw9ucrzv3nxs6nvdec8yark0pa8clshdj6ps";
const QString kToken = "test-ops-token";

// Stands in for the pool's loopback ops endpoint: GET /status with a bearer token.
class FakePool : public QObject {
public:
    FakePool() {
        QVERIFY(server_.listen(QHostAddress::LocalHost));
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    // Answer once the whole request head has arrived.
                    QByteArray request = socket->property("pending").toByteArray() + socket->readAll();
                    socket->setProperty("pending", request);
                    if (!request.contains("\r\n\r\n")) return;
                    ++requests;
                    const bool authorized =
                        request.toLower().contains(("authorization: bearer " + kToken).toLower().toUtf8());
                    const QByteArray body = authorized ? QJsonDocument(status()).toJson(QJsonDocument::Compact)
                                                       : QByteArray("{\"error\":\"unauthorized\"}");
                    socket->write((authorized ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 401 Unauthorized\r\n") +
                                  QByteArray("Content-Type: application/json\r\nContent-Length: ") +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString endpoint() const { return QString("http://127.0.0.1:%1").arg(server_.serverPort()); }
    int requests = 0;

    static QJsonObject status() {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        return QJsonObject{
            {"schema_version", 2}, {"schema_min_compatible", 2}, {"pool_version", "0.1.9"},
            {"uptime_secs", 796576}, {"connected_miners", 2}, {"fee_bps", 1000},
            {"window_entries", 7174}, {"window_span_secs", 14400},
            {"template_heartbeat_age_secs", 1}, {"template_phase", "polling_tip"},
            {"accepted_shares_total", 302343}, {"rejected_shares_total", 105673},
            {"blocks_found_total", 3996}, {"generated_at_unix", now},
            {"daemon_connected", true}, {"daemon_blocks", 121363}, {"daemon_headers", 121363},
            {"daemon_endpoint", "http://127.0.0.1:20998"}, {"stratum_bind", "0.0.0.0:4444"},
            {"payout_address", kFeeAddress}, {"template_prev_hash", QString(64, '0')},
            {"template_height", 121364}, {"template_id", 50037}, {"last_template_at_unix", now - 1},
            {"last_share", QJsonObject{{"accepted_at_unix", now - 2}, {"hash", QString(64, '1')}, {"kind", "shared"}}},
            {"last_block", QJsonObject{{"hash", QString(64, '2')}, {"observed_at_unix", now - 60},
                                       {"reason", ""}, {"status", "accepted"}}},
            {"rejection_reasons", QJsonObject{{"stale-share", 70721}, {"duplicate-share", 25593}}},
            {"bans", QJsonArray{}},
            {"miners", QJsonArray{
                QJsonObject{{"bps", 7313}, {"window_weight", "53183771145"},
                            {"payout_script_hex", "5120000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"}},
                QJsonObject{{"bps", 2686}, {"window_weight", "19540210161"},
                            {"payout_script_hex", "53201111111111111111111111111111111111111111111111111111111111111111"}}}},
        };
    }

private:
    QTcpServer server_;
};

// In-memory token store shared across panel instances, as the Keychain is across launches.
class FakeStore final : public PoolTokenStore {
public:
    explicit FakeStore(QHash<QString, QString>* items) : items_(items) {}
    bool available() const override { return true; }
    QString load(const QString& a) const override { return items_->value(a); }
    bool save(const QString& a, const QString& t) override { items_->insert(a, t); return true; }
    void remove(const QString& a) override { items_->remove(a); }
private:
    QHash<QString, QString>* items_;
};

template <class T>
T* named(QWidget& w, const char* name) { return w.findChild<T*>(QString::fromLatin1(name)); }

}  // namespace

class PoolCockpitTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QTcpServer deadNode_;  // earnings RPCs go here and are never answered

    std::unique_ptr<RpcClient> makeRpc() {
        auto rpc = std::make_unique<RpcClient>();
        rpc->setDatadir(dir_.path());
        rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(deadNode_.serverPort())));
        return rpc;
    }
    static void connectTo(PoolPanel& panel, const QString& endpoint, const QString& name, bool remember) {
        named<QLineEdit>(panel, "poolName")->setText(name);
        named<QLineEdit>(panel, "poolEndpoint")->setText(endpoint);
        named<QLineEdit>(panel, "poolToken")->setText(kToken);
        named<QCheckBox>(panel, "poolRemember")->setChecked(remember);
        QTest::mouseClick(named<QPushButton>(panel, "poolConnect"), Qt::LeftButton);
    }
    static bool connected(PoolPanel& panel) {
        return named<QLabel>(panel, "poolStatusMessage")->text().contains("Connected.");
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(deadNode_.listen(QHostAddress::LocalHost));
        QCoreApplication::setOrganizationName("DineroPoolCockpitTest");
        QCoreApplication::setApplicationName("IsolatedPoolCockpit");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
    }
    void init() { QSettings().clear(); }

    void rememberedPoolReconnectsOnLaunch() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        {
            PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
            connectTo(panel, pool.endpoint(), "SJ", /*remember=*/true);
            QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        }
        QCOMPARE(keychain.value(pool.endpoint()), kToken);  // token went to the store...
        for (const QString& key : QSettings().allKeys())     // ...and never to settings
            QVERIFY2(!QSettings().value(key).toString().contains(kToken), qPrintable(key));

        // Next launch: the pool is picked, filled in and connected without typing.
        PoolPanel relaunched(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        QCOMPARE(named<QComboBox>(relaunched, "poolProfile")->currentText(), QString("SJ"));
        QCOMPARE(named<QLineEdit>(relaunched, "poolEndpoint")->text(), pool.endpoint());
        QTRY_VERIFY2_WITH_TIMEOUT(connected(relaunched), qPrintable(named<QLabel>(relaunched, "poolStatusMessage")->text()), 5000);

        // Forget removes both the saved pool and its token.
        QTest::mouseClick(named<QPushButton>(relaunched, "poolForget"), Qt::LeftButton);
        QVERIFY(keychain.isEmpty());
        PoolPanel afterForget(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        QVERIFY(named<QComboBox>(afterForget, "poolProfile")->findText("SJ") < 0);
        QCOMPARE(named<QLineEdit>(afterForget, "poolToken")->text(), QString());
    }

    void unrememberedTokenIsNotStored() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        {
            PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
            connectTo(panel, pool.endpoint(), "TX", /*remember=*/false);
            QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        }
        QVERIFY(keychain.isEmpty());
        PoolPanel relaunched(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        QCOMPARE(named<QLineEdit>(relaunched, "poolEndpoint")->text(), pool.endpoint());  // endpoint is remembered
        QCOMPARE(named<QLineEdit>(relaunched, "poolToken")->text(), QString());           // the token is not
        QTest::qWait(300);
        QCOMPARE(pool.requests, 1);  // no automatic connect without a token
    }

    void feeAddressFillsInAndEarningsStart() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        connectTo(panel, pool.endpoint(), "SJ", false);
        QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        QCOMPARE(named<QLineEdit>(panel, "poolFeeAddress")->text(), kFeeAddress);
        QVERIFY2(named<QLabel>(panel, "poolLifetime")->text().contains("reading the chain"),
                 qPrintable(named<QLabel>(panel, "poolLifetime")->text()));
    }

    void contributorsReadAsAddresses() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        connectTo(panel, pool.endpoint(), "SJ", false);
        QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        auto* table = named<QTableWidget>(panel, "poolContributors");
        QVERIFY(table);
        QCOMPARE(table->item(0, 0)->text(),
                 QString("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0shg7l3c"));
        QVERIFY(table->item(0, 0)->toolTip().contains("5120000102"));
        QVERIFY(table->item(1, 0)->text().startsWith("din1r"));
        QCOMPARE(table->item(0, 3)->text(), QString("53,183,771,145"));
    }

    void numbersAreReadable() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        connectTo(panel, pool.endpoint(), "SJ", false);
        QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        const QString status = named<QLabel>(panel, "poolStatusMessage")->text();
        QVERIFY2(status.contains("up 9 days 5 h"), qPrintable(status));
        const QString window = named<QLabel>(panel, "poolWindow")->text();
        QVERIFY2(window.contains("7,174 shares over 4 h"), qPrintable(window));
        const QString shares = named<QLabel>(panel, "poolShares")->text();
        QVERIFY2(shares.contains("302,343 accepted") && shares.contains("105,673 rejected"), qPrintable(shares));
    }

    void historySaysItIsStillCollecting() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        connectTo(panel, pool.endpoint(), "SJ", false);
        QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        for (const char* name : {"poolHistory5m", "poolHistory24h"}) {
            const QString text = named<QLabel>(panel, name)->text();
            QVERIFY2(text.contains("collecting since"), qPrintable(QString("%1: %2").arg(name, text)));
        }
    }

    void usesTheAppColours() {
        FakePool pool;
        QHash<QString, QString> keychain;
        auto rpc = makeRpc();
        PoolPanel panel(rpc.get(), nullptr, std::make_unique<FakeStore>(&keychain));
        connectTo(panel, pool.endpoint(), "SJ", false);
        QTRY_VERIFY2_WITH_TIMEOUT(connected(panel), qPrintable(named<QLabel>(panel, "poolStatusMessage")->text()), 5000);
        QList<QWidget*> all = panel.findChildren<QWidget*>();
        all << &panel;
        for (QWidget* w : all) {
            QString text = w->styleSheet();
            if (auto* l = qobject_cast<QLabel*>(w)) text += l->text();
            for (const char* off : {"#7bd88f", "#e06c75", "#d8a37b"})
                QVERIFY2(!text.contains(off, Qt::CaseInsensitive),
                         qPrintable(QString("%1 uses %2").arg(w->objectName(), off)));
        }
    }
};
QTEST_MAIN(PoolCockpitTest)
#include "test_pool_cockpit.moc"
