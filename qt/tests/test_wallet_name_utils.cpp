#include <QtTest/QtTest>

#include "../src/walletnameutils.h"

class WalletNameUtilsTest : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void normalizeCollapsesWhitespaceAndInvalidChars();
  void normalizeAvoidsWindowsReservedNames();
  void validateRejectsEmptyOrTooLongNames();
  void restoreNameUnique();
  void restoreNameIsValid();
  void restoreErrorClassify();
  void rollbackOnlyWhatMightExist();
};

void WalletNameUtilsTest::normalizeCollapsesWhitespaceAndInvalidChars() {
  QCOMPARE(normalizeWalletName(QStringLiteral("  Savings   Wallet  ")),
           QStringLiteral("Savings Wallet"));
  QCOMPARE(normalizeWalletName(QStringLiteral("family/work!2026")),
           QStringLiteral("familywork2026"));
  QCOMPARE(normalizeWalletName(QStringLiteral("Alpha_Beta-01")),
           QStringLiteral("Alpha_Beta-01"));
}

void WalletNameUtilsTest::normalizeAvoidsWindowsReservedNames() {
  QCOMPARE(normalizeWalletName(QStringLiteral("CON")), QStringLiteral("CON_"));
  QCOMPARE(normalizeWalletName(QStringLiteral("lpt1")), QStringLiteral("lpt1_"));
}

void WalletNameUtilsTest::validateRejectsEmptyOrTooLongNames() {
  QString normalized;
  QVERIFY(!validateWalletNameInput(QStringLiteral("   "), &normalized).isEmpty());
  QCOMPARE(normalized, QString());

  const QString longName(65, QChar('a'));
  QVERIFY(validateWalletNameInput(longName, &normalized).contains(QStringLiteral("64")));
  QCOMPARE(normalized.size(), 65);

  QVERIFY(validateWalletNameInput(QStringLiteral("wallet 01"), &normalized).isEmpty());
  QCOMPARE(normalized, QStringLiteral("wallet 01"));
}

void WalletNameUtilsTest::restoreNameUnique() {
  QCOMPARE(uniqueRestoreName(QStringLiteral("Main"), {}), QStringLiteral("Main"));
  QCOMPARE(uniqueRestoreName(QStringLiteral("Main"), {QStringLiteral("main")}), QStringLiteral("Main restored"));
  QCOMPARE(uniqueRestoreName(QStringLiteral("Main"), {QStringLiteral("Main"), QStringLiteral("Main restored")}),
           QStringLiteral("Main restored 2"));
  QCOMPARE(uniqueRestoreName(QStringLiteral("  Main  "), {QStringLiteral("Main")}), QStringLiteral("Main restored"));
}

void WalletNameUtilsTest::restoreNameIsValid() {
  const QString suggested = uniqueRestoreName(QStringLiteral("Savings"), {QStringLiteral("Savings")});
  QString normalized;
  QVERIFY(validateWalletNameInput(suggested, &normalized).isEmpty());
  QCOMPARE(normalized, suggested);
  const QString longBase(64, QChar('a'));
  const QString suggestedLong = uniqueRestoreName(longBase, {longBase});
  QVERIFY(validateWalletNameInput(suggestedLong).isEmpty());
}

void WalletNameUtilsTest::restoreErrorClassify() {
  QCOMPARE(classifyRestoreError(QStringLiteral("Wallet already exists: Main. Restore under a new wallet name; existing wallets cannot be overwritten.")),
           RestoreErrorKind::NameExists);
  QCOMPARE(classifyRestoreError(QStringLiteral("Invalid BIP39 mnemonic (checksum failed)")), RestoreErrorKind::Other);
  QCOMPARE(classifyRestoreError(QString()), RestoreErrorKind::None);
  // Recorded from a PR #813 daemon: same name with different capitals collides on
  // case-insensitive disks and surfaces as a file error.
  QCOMPARE(classifyRestoreError(QStringLiteral("Wallet restoration failed: Wallet database file already exists: /data/wallets/wallet_PROBE.db")),
           RestoreErrorKind::NameExists);
}

void WalletNameUtilsTest::rollbackOnlyWhatMightExist() {
  // The node answered with an error: it created nothing, so cancelling must not
  // delete a wallet of that name (it may be the user's existing wallet).
  QVERIFY(!keepRollbackCandidateAfterRpc(/*daemonResponded=*/true, /*success=*/false));
  // No answer (timeout): the wallet may exist now, so keep it for cleanup.
  QVERIFY(keepRollbackCandidateAfterRpc(false, false));
  // Success: the provisional wallet is ours to roll back on cancel.
  QVERIFY(keepRollbackCandidateAfterRpc(true, true));
}

QTEST_GUILESS_MAIN(WalletNameUtilsTest)

#include "test_wallet_name_utils.moc"
