#include <QtTest/QtTest>
#include "pooltokenstore.h"

class PoolTokenStoreTest : public QObject {
    Q_OBJECT
    const QString service_ = QStringLiteral("org.dinerolabs.dinero-qt.pool-ops-token.test");
    const QString account_ = QStringLiteral("http://127.0.0.1:59999");
private Q_SLOTS:
    void cleanup() { makeSystemPoolTokenStore(service_)->remove(account_); }
    void keychainRoundTrip() {
#ifndef Q_OS_MACOS
        QSKIP("Only macOS has a token store; other platforms paste the token each session.");
#endif
        auto store = makeSystemPoolTokenStore(service_);
        QVERIFY(store->available());
        QCOMPARE(store->load(account_), QString());
        QVERIFY(store->save(account_, "first-token"));
        QCOMPARE(makeSystemPoolTokenStore(service_)->load(account_), QString("first-token"));
        QVERIFY(store->save(account_, "second-token"));  // replaces, never duplicates
        QCOMPARE(store->load(account_), QString("second-token"));
        // Scoped by service: the real app's entry name never sees test data.
        QCOMPARE(makeSystemPoolTokenStore()->load(account_), QString());
        store->remove(account_);
        QCOMPARE(store->load(account_), QString());
    }
};
QTEST_GUILESS_MAIN(PoolTokenStoreTest)
#include "test_pool_token_store.moc"
