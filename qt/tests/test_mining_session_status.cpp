#include <QtTest>
#include "../src/miningsessionstatus.h"
class MiningSessionStatusTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void runtimeBackendWins() {
        QCOMPARE(miningSessionStatus(true, "cpu", true), "CPU active (GPU unavailable; CPU fallback)");
        QCOMPARE(miningSessionStatus(true, "cpu", false), "CPU active");
        QCOMPARE(miningSessionStatus(true, "metal", true), "Metal active");
        QCOMPARE(miningSessionStatus(true, "cuda", true), "CUDA active");
        QCOMPARE(miningSessionStatus(true, "opencl", true), "OpenCL active");
        QCOMPARE(miningSessionStatus(true, "auto", true), "Backend unavailable");
        QCOMPARE(miningSessionStatus(false, "metal", true), "Not running");
    }
};
QTEST_APPLESS_MAIN(MiningSessionStatusTests)
#include "test_mining_session_status.moc"
