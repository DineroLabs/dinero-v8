// The "recovery words are not confirmed" reminder (recoverywordspolicy.h).
#include "recoverywordspolicy.h"

#include <QtTest>

class RecoveryWordsPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // A wallet whose words were never confirmed is reminded; nothing else is.
    void theReminderShowsOnlyForAWalletWithUnconfirmedWords() {
        using RecoveryWordsPolicy::reminderNeeded;
        const QJsonObject fresh{{"hd_enabled", true}, {"mnemonic_backup_available", true},
                                {"backup_required", true}, {"backup_acknowledged", false}};
        QVERIFY(reminderNeeded(fresh));
        // Locked or unlocked makes no difference: the reminder is about the backup, and showing needs the unlock.
        QJsonObject locked = fresh;
        locked["locked"] = true;
        QVERIFY(reminderNeeded(locked));

        QJsonObject confirmed = fresh;
        confirmed["backup_required"] = false;
        confirmed["backup_acknowledged"] = true;
        QVERIFY(!reminderNeeded(confirmed));

        // An old raw-seed wallet has no words the node could show: no button that cannot work.
        QJsonObject rawSeed = fresh;
        rawSeed["mnemonic_backup_available"] = false;
        QVERIFY(!reminderNeeded(rawSeed));

        QVERIFY(!reminderNeeded(QJsonObject{}));  // no wallet, or an older node that does not say
        QVERIFY(!reminderNeeded(QJsonObject{{"hd_enabled", false}, {"mnemonic_backup_available", true}, {"backup_required", true}}));
        // A refusal (the wallet is still catching up) is not news about the backup.
        QJsonObject refused = fresh;
        refused["error"] = "wallet balance is not current";
        QVERIFY(!reminderNeeded(refused));
    }

    void confirmingSendsTheWordsTheNodeChecks() {
        using namespace RecoveryWordsPolicy;
        const QString words = "  abandon ability  able about above absent absorb abstract absurd abuse access accident \n";
        QJsonArray p = confirmParams(words, QJsonObject{});
        QCOMPARE(p.size(), 2);
        QCOMPARE(p.at(0).toString(), QString("abandon ability able about above absent absorb abstract absurd abuse access accident"));
        QCOMPARE(p.at(1).toBool(true), false);
        QCOMPARE(confirmButton(QJsonObject{}), QString("I have written these words down"));
        // A wallet that also needs a passphrase: the words alone do not restore it, and the user says both are kept.
        const QJsonObject withPassphrase{{"passphrase_required", true}};
        QCOMPARE(confirmParams(words, withPassphrase).at(1).toBool(false), true);
        QVERIFY(confirmButton(withPassphrase).contains("passphrase"));
        QVERIFY(reminderText().contains("only backup"));
    }
};

QTEST_APPLESS_MAIN(RecoveryWordsPolicyTest)
#include "test_recovery_words_policy.moc"
