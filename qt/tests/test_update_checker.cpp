#include <QtTest>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "updatechecker.h"

namespace {
QUrl writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& body) {
    QFile f(dir.filePath(name));
    f.open(QIODevice::WriteOnly);
    f.write(body);
    return QUrl::fromLocalFile(f.fileName());
}
}

class TestUpdateChecker : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void readsTagAndNotice() {
        QTemporaryDir dir;
        QNetworkAccessManager nam;
        UpdateChecker c(&nam, writeFile(dir, "latest.json", R"({"tag_name":"v8.1.13"})"),
                        writeFile(dir, "notice.json", R"({"schema":"dinero.network-upgrade.v1","activation_height":125000})"));
        QSignalSpy spy(&c, &UpdateChecker::resultReady);
        c.checkNow();
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toString(), QString("v8.1.13"));
        QCOMPARE(spy.at(0).at(1).toJsonObject().value("activation_height").toInt(), 125000);
    }
    void missingNoticeGivesEmptyObject() {
        QTemporaryDir dir;
        QNetworkAccessManager nam;
        UpdateChecker c(&nam, writeFile(dir, "latest.json", R"({"tag_name":"v8.1.12"})"),
                        QUrl::fromLocalFile(dir.filePath("absent.json")));
        QSignalSpy spy(&c, &UpdateChecker::resultReady);
        c.checkNow();
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toString(), QString("v8.1.12"));
        QVERIFY(spy.at(0).at(1).toJsonObject().isEmpty());
    }
    void garbageEverywhereGivesNothing() {
        QTemporaryDir dir;
        QNetworkAccessManager nam;
        UpdateChecker c(&nam, writeFile(dir, "latest.json", "not json"),
                        writeFile(dir, "notice.json", "[1,2,3]"));
        QSignalSpy spy(&c, &UpdateChecker::resultReady);
        c.checkNow();
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.at(0).at(0).toString().isEmpty());
        QVERIFY(spy.at(0).at(1).toJsonObject().isEmpty());
        QCOMPARE(spy.count(), 1);
    }
};
QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "test_update_checker.moc"
