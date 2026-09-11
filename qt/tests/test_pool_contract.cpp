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

#include <optional>

#include "../src/poolcontract.h"

class TestPoolContract : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // `std::nullopt` = the pool did not declare what it is compatible
    // with, which is every pool shipping today.
    static std::optional<qint64> undeclared() { return std::nullopt; }

    void theSchemaThisPanelWasWrittenAgainstIsSupported() {
        QVERIFY(poolcontract::isSupportedSchema(2, undeclared()));
        QVERIFY(poolcontract::isSupportedSchema(2, 2));
    }

    // Older than the fields this panel reads. Schema 1 is normally
    // reported by omitting the key, which the panel handles on its
    // legacy path; an explicit 1 means the same and cannot satisfy the
    // v2 field checks.
    void olderSchemasAreNotSupported() {
        QVERIFY(!poolcontract::isSupportedSchema(1, undeclared()));
        QVERIFY(!poolcontract::isSupportedSchema(0, undeclared()));
        QVERIFY(!poolcontract::isSupportedSchema(-1, undeclared()));
    }

    // The point of the declaration. A pool that added fields and bumped
    // says it is still readable by a client written for 2, and it is.
    void aNewerPoolThatDeclaresCompatibilityIsRead() {
        QVERIFY(poolcontract::isSupportedSchema(3, 2));
        QVERIFY(poolcontract::isSupportedSchema(99, 2));
    }

    // The case a bare floor got wrong. A pool that renamed, removed or
    // REINTERPRETED a field raises its compatibility floor above ours,
    // and must be refused — a field can keep its name and type while
    // changing meaning, which no amount of field validation catches.
    void aNewerPoolThatBrokeCompatibilityIsRefused() {
        QVERIFY(!poolcontract::isSupportedSchema(3, 3));
        QVERIFY(!poolcontract::isSupportedSchema(4, 3));
    }

    // Silence is not a promise. A pool newer than this panel that does
    // not say what it is compatible with cannot be assumed compatible —
    // that assumption is exactly what would render a reinterpreted
    // field as though its meaning had not changed.
    void aNewerPoolThatDeclaresNothingIsRefused() {
        QVERIFY(!poolcontract::isSupportedSchema(3, undeclared()));
        QVERIFY(!poolcontract::isSupportedSchema(4, undeclared()));
    }

    // A declaration that claims compatibility with a future version it
    // cannot itself be is nonsense, not a promise.
    void anIncoherentDeclarationIsRefused() {
        QVERIFY(!poolcontract::isSupportedSchema(2, 3));
    }

    // Each refusal has a different fix, and the message has to say
    // which: upgrade the pool, or upgrade the wallet.
    void eachRefusalNamesTheHalfToUpgrade() {
        const QString too_old = poolcontract::unsupportedSchemaReason(1, undeclared());
        QVERIFY(too_old.contains("pool", Qt::CaseInsensitive));

        const QString broke = poolcontract::unsupportedSchemaReason(3, 3);
        QVERIFY(broke.contains("wallet", Qt::CaseInsensitive));

        const QString silent = poolcontract::unsupportedSchemaReason(3, undeclared());
        QVERIFY(silent.contains("wallet", Qt::CaseInsensitive));
        QVERIFY(silent.contains("3"));
    }
};

QTEST_APPLESS_MAIN(TestPoolContract)
#include "test_pool_contract.moc"
