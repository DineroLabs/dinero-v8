#include <QtTest/QtTest>
#include "segwitaddress.h"

using segwitaddress::addressToScriptHex;

class SegwitAddressTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void taprootAndP2mrBecomeScripts() {
        // Vectors from an independent reference implementation (also used by PoolCockpitFormat).
        QCOMPARE(addressToScriptHex("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0shg7l3c"),
                 QString("5120000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
        QCOMPARE(addressToScriptHex("  tdin1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0su86pwd "),
                 QString("5120000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
        // P2MR is witness version 3 (include/wallet/p2mr_address.h): OP_3 = 0x53.
        QCOMPARE(addressToScriptHex("din1rzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsjls6g6"),
                 QString("53201111111111111111111111111111111111111111111111111111111111111111"));
    }
    void mistypedAddressesAreRefused() {
        // Last character changed: a typo must never become a payout script.
        QCOMPARE(addressToScriptHex("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0shg7l3q"), QString());
        QCOMPARE(addressToScriptHex("din1rzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsjls6g7"), QString());
    }
    void unsupportedShapesAreRefused() {
        // Witness v2 is not a Dinero payout type.
        QCOMPARE(addressToScriptHex("din1zqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0sl48sln"), QString());
        // v1 with a 20-byte program.
        QCOMPARE(addressToScriptHex("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnvyxk4u"), QString());
        // Another chain's HRP.
        QCOMPARE(addressToScriptHex("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0"), QString());
        QCOMPARE(addressToScriptHex(""), QString());
        QCOMPARE(addressToScriptHex("din1"), QString());
    }
};
QTEST_GUILESS_MAIN(SegwitAddressTest)
#include "test_segwit_address.moc"
