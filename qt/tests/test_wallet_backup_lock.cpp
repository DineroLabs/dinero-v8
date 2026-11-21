#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "../src/walletbackuplock.h"

class WalletBackupLockTest : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void refusesConcurrentOwnerAndReleases();
};

void WalletBackupLockTest::refusesConcurrentOwnerAndReleases() {
  QTemporaryDir datadir;
  QVERIFY(datadir.isValid());
  {
    WalletBackupDatadirLock first(datadir.path());
    QVERIFY(first.held());
    QVERIFY(QFile::exists(QDir(datadir.path()).filePath("dinerod.lock")));
    WalletBackupDatadirLock second(datadir.path());
    QVERIFY(!second.held());
  }
  WalletBackupDatadirLock afterRelease(datadir.path());
  QVERIFY(afterRelease.held());
}

QTEST_GUILESS_MAIN(WalletBackupLockTest)
#include "test_wallet_backup_lock.moc"
