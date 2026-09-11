// Copyright (c) 2026 Dinero Labs.
//
// Forward compatibility with the pool's ops contract.
//
// `OpsStatus.schema_version` is documented in dinero-sv2 as "a monotonic
// contract version for strict consumers", with v1 fields kept present
// "so older Qt releases continue to work". The panel then rejected
// anything that was not exactly 2, which inverts that intent: a pool
// adding a field and bumping its version would take the Pool tab offline
// until a new wallet shipped.
//
// That is a live hazard, not a hypothetical. The pool and the wallet are
// separate repositories, separately released, and an operator upgrades
// their pool by hand. Strict equality means whoever upgrades first
// breaks their own wallet and has no way to tell why.
//
// The contract only ever ADDS fields, so a newer schema is readable by
// an older client. The check is therefore a floor, not an equality.

#include <QtTest/QtTest>

#include "../src/poolcontract.h"

class TestPoolContract : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void theSchemaThisPanelWasWrittenAgainstIsSupported() {
        QVERIFY(poolcontract::isSupportedSchema(2));
    }

    // The reason this file exists. A pool that adds `bans` and bumps to
    // 3 must not take the panel offline.
    void newerSchemasAreSupportedBecauseTheyOnlyAddFields() {
        QVERIFY(poolcontract::isSupportedSchema(3));
        QVERIFY(poolcontract::isSupportedSchema(4));
        QVERIFY(poolcontract::isSupportedSchema(99));
    }

    // Older than the fields this panel reads. Schema 1 is reported by
    // omitting the key entirely, which the panel handles on its legacy
    // path; an explicit 1 means the same thing and cannot satisfy the
    // v2 field checks.
    void olderSchemasAreNotSupported() {
        QVERIFY(!poolcontract::isSupportedSchema(1));
        QVERIFY(!poolcontract::isSupportedSchema(0));
    }

    // A malformed payload should not read as a future version.
    void aNonsenseVersionIsNotSupported() {
        QVERIFY(!poolcontract::isSupportedSchema(-1));
        QVERIFY(!poolcontract::isSupportedSchema(std::numeric_limits<qint64>::min()));
    }

    // The message an operator sees has to say what to do. "Unsupported
    // schema_version 1" alone sends them looking at the wallet, when the
    // thing to change is the pool.
    void theRefusalNamesTheFixRatherThanJustTheNumber() {
        const QString why = poolcontract::unsupportedSchemaReason(1);
        QVERIFY(why.contains("1"));
        QVERIFY(why.contains("upgrade", Qt::CaseInsensitive));
        QVERIFY(why.contains("pool", Qt::CaseInsensitive));
    }
};

QTEST_APPLESS_MAIN(TestPoolContract)
#include "test_pool_contract.moc"
