#include "daemonoutput.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == QStringLiteral("--emit")) {
        constexpr char bytes[] = "0123456789abcdef0123456789abcdef\n";
        for (int i = 0; i < 100000; ++i) {
            if (std::fwrite(bytes, 1, sizeof(bytes) - 1, stdout) != sizeof(bytes) - 1) return 2;
        }
        if (std::fflush(stdout) != 0) return 3;
        QFile marker(args[2]);
        if (!marker.open(QIODevice::WriteOnly)) return 4;
        marker.write("done");
        return 0;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) return 10;
    const QString markerPath = dir.filePath(QStringLiteral("finished"));
    const QString logPath = dinero::qt::daemonOutputLogPath(dir.path());
    QProcess daemon;
    QString error;
    if (!dinero::qt::configureDaemonOutput(&daemon, dir.path(), &error)) {
        std::fprintf(stderr, "output configuration failed: %s\n", error.toUtf8().constData());
        return 11;
    }
    daemon.start(app.applicationFilePath(), {QStringLiteral("--emit"), markerPath});
    if (!daemon.waitForStarted(5000)) return 12;

    // The GUI may be blocked in Keychain access before its event loop starts.
    // A pipe-based daemon would fill stdout and never reach this marker.
    bool completedWithoutEventLoop = false;
    for (int i = 0; i < 100; ++i) {
        if (QFileInfo::exists(markerPath)) {
            completedWithoutEventLoop = true;
            break;
        }
        QThread::msleep(50);
    }
    if (!completedWithoutEventLoop) {
        daemon.kill();
        daemon.waitForFinished(5000);
        std::fputs("daemon blocked before GUI event loop\n", stderr);
        return 13;
    }
    if (!daemon.waitForFinished(5000) || daemon.exitCode() != 0) return 14;
    QFile log(logPath);
    if (!log.open(QIODevice::ReadOnly) || log.size() < 3000000) return 15;
    return 0;
}
