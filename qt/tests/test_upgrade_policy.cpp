#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include "upgradepolicy.h"

class TestUpgradePolicy : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void parse() {
        const auto v = UpgradePolicy::parseVersion("v8.1.12-metal-fix1");
        QCOMPARE(v.major, 8); QCOMPARE(v.minor, 1); QCOMPARE(v.patch, 12);
        QVERIFY(!UpgradePolicy::parseVersion("unknown").valid());
        QVERIFY(!UpgradePolicy::parseVersion("8.x.1").valid());
        QCOMPARE(UpgradePolicy::compare(UpgradePolicy::parseVersion("8.1.9"), UpgradePolicy::parseVersion("8.1.12")), -1);
    }
    void sharedVectors() {
        QFile f(QStringLiteral(UPGRADE_VECTORS));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        // Must equal the DineroDPI copy (DineroDPI/test-vectors/network_upgrade_policy_v1.json).
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                 QByteArray("2791fbd9800cac94ab80898578f11ceea8564527506d5d21f1e9642339b31dec"));
        const auto root = QJsonDocument::fromJson(bytes).object();
        const auto cases = root["cases"].toArray();
        QVERIFY(cases.size() >= 16);
        for (const auto& cv : cases) {
            const auto c = cv.toObject();
            const auto notice = c["notice"].isObject()
                ? UpgradePolicy::parseNotice(c["notice"].toObject(), "dinero-qt") : UpgradePolicy::Notice{};
            std::optional<quint32> nodeH;
            if (c["node_release_height"].isDouble()) nodeH = quint32(c["node_release_height"].toDouble());
            const auto r = UpgradePolicy::evaluate(c["app"].toString(), c["latest"].toString(), notice,
                                                   quint32(c["tip"].toDouble()), nodeH);
            const auto e = c["expect"].toObject();
            QVERIFY2(UpgradePolicy::stateName(r.state) == e["state"].toString(),
                     qPrintable(c["name"].toString() + " got " + UpgradePolicy::stateName(r.state)));
            const qint64 want = e["blocks_left"].isNull() ? -1 : qint64(e["blocks_left"].toDouble());
            QVERIFY2(r.blocksLeft == want, qPrintable(c["name"].toString() + " blocks_left " + QString::number(r.blocksLeft)));
        }
    }
};
QTEST_GUILESS_MAIN(TestUpgradePolicy)
#include "test_upgrade_policy.moc"
