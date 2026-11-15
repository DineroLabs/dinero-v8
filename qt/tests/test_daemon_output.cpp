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
        constexpr char stderrMarker[] = "daemon-stderr-marker\n";
        if (std::fwrite(stderrMarker, 1, sizeof(stderrMarker) - 1, stderr) !=
            sizeof(stderrMarker) - 1) return 5;
        if (std::fflush(stderr) != 0) return 6;
        QFile marker(args[2]);
        if (!marker.open(QIODevice::WriteOnly)) return 4;
        marker.write("done");
        return 0;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) return 10;
    const QString markerPath = dir.filePath(QStringLiteral("finished"));
    const QString logPath = dinero::qt::daemonOutputLogPath(dir.path());
    QFile previous(logPath);
    if (!previous.open(QIODevice::WriteOnly) || previous.write("previous-run") != 12) return 16;
    previous.close();
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
    if (!log.read(64).startsWith("0123456789abcdef") ||
        !log.readAll().contains("daemon-stderr-marker")) return 17;
#ifndef Q_OS_WIN
    const auto permissions = QFileInfo(logPath).permissions();
    if (permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                       QFileDevice::ReadOther | QFileDevice::WriteOther)) return 18;
#endif
    return 0;
}
