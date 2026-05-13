#include <QtTest/QtTest>

#include "../src/walletnameutils.h"

class WalletNameUtilsTest : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void normalizeCollapsesWhitespaceAndInvalidChars();
  void normalizeAvoidsWindowsReservedNames();
  void validateRejectsEmptyOrTooLongNames();
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

QTEST_GUILESS_MAIN(WalletNameUtilsTest)

#include "test_wallet_name_utils.moc"
