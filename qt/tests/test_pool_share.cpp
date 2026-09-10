// Copyright (c) 2026 Dinero Labs.
//
// The Pool tab's contributor table reports two different percentages and
// they are NOT the same number. `MinerStatus.bps` from the pool's ops
// endpoint is a share of the CONTRIBUTOR POT, which `split.rs` computes
// only after the operator fee has been taken off the top:
//
//     fee_una = reward * fee_bps / 10000
//     pot     = reward - fee_una
//     paid    = pot * contributor_bps / 10000
//
// So a contributor holding 5000 bps of the pot receives 45% of the block
// when the operator fee is 10%, not 50%. Showing the raw bps under a
// heading that says "Next-block share" overstates every contributor's
// payout by exactly the fee.
//
// The fee is operator-controlled and changeable at runtime, so the block
// share cannot be a constant applied at review time — it has to be
// derived from the fee_bps the pool is reporting right now.

#include <QtTest/QtTest>

#include "../src/poolshare.h"

class TestPoolShare : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // The split share is the raw bps and must stay that way: it is what
    // the pool itself reports, and an operator comparing the table
    // against a hand-run `curl /status` has to see the same number.
    void splitShareIsTheRawBps() {
        QCOMPARE(poolshare::splitShareText(5000), QStringLiteral("50.00%"));
        QCOMPARE(poolshare::splitShareText(10000), QStringLiteral("100.00%"));
        QCOMPARE(poolshare::splitShareText(1), QStringLiteral("0.01%"));
    }

    // The defect this test exists for. At the installer's default 10%
    // fee, half the pot is 45% of the block.
    void blockShareIsReducedByTheOperatorFee() {
        QCOMPARE(poolshare::blockShareText(5000, 1000), QStringLiteral("45.00%"));
        QCOMPARE(poolshare::blockShareText(10000, 1000), QStringLiteral("90.00%"));
        QCOMPARE(poolshare::blockShareText(2500, 2000), QStringLiteral("20.00%"));
    }

    // A zero-fee pool is the one case where the two columns agree. If
    // they agree for any OTHER fee, the fee is not being applied.
    void zeroFeeMakesBothSharesEqual() {
        QCOMPARE(poolshare::blockShareText(5000, 0), poolshare::splitShareText(5000));
        QCOMPARE(poolshare::blockShareText(10000, 0), QStringLiteral("100.00%"));
    }

    // split.rs clamps fee_bps with `.min(10_000)`; at 100% there is no
    // contributor pot at all and every contributor is paid nothing.
    void fullFeeLeavesContributorsNothing() {
        QCOMPARE(poolshare::blockShareText(10000, 10000), QStringLiteral("0.00%"));
        QCOMPARE(poolshare::blockShareText(1, 10000), QStringLiteral("0.00%"));
    }

    // `strictInt` accepts any integer the pool sends, including values a
    // correct pool would never produce. Clamp rather than render a
    // negative or >100% share.
    void outOfRangeInputsAreClamped() {
        QCOMPARE(poolshare::splitShareText(-1), QStringLiteral("0.00%"));
        QCOMPARE(poolshare::splitShareText(20000), QStringLiteral("100.00%"));
        QCOMPARE(poolshare::blockShareText(5000, -500), QStringLiteral("50.00%"));
        QCOMPARE(poolshare::blockShareText(5000, 25000), QStringLiteral("0.00%"));
        QCOMPARE(poolshare::blockShareText(-1, 1000), QStringLiteral("0.00%"));
    }

    // Two decimals, matching the operator-fee readout elsewhere on the
    // panel, and rounded rather than truncated.
    void percentagesAreRoundedToTwoDecimals() {
        QCOMPARE(poolshare::splitShareText(3333), QStringLiteral("33.33%"));
        // 3333 bps of a 90% pot = 29.997% -> 30.00%, not 29.99%.
        QCOMPARE(poolshare::blockShareText(3333, 1000), QStringLiteral("30.00%"));
    }
};

QTEST_APPLESS_MAIN(TestPoolShare)
#include "test_pool_share.moc"
