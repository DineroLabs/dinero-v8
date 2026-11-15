#include "daemonoutput.h"

#include <QDir>
#include <QFile>
#include <QProcess>

namespace dinero::qt {

QString daemonOutputLogPath(const QString& datadir) {
    return QDir(datadir).filePath(QStringLiteral("embedded-daemon.log"));
}

bool configureDaemonOutput(QProcess* process, const QString& datadir, QString* error) {
    if (!process) {
        if (error) *error = QStringLiteral("Daemon process is missing");
        return false;
    }
    if (datadir.isEmpty() || !QDir().mkpath(datadir)) {
        if (error) *error = QStringLiteral("Cannot create daemon data directory: %1").arg(datadir);
        return false;
    }

    const QString path = daemonOutputLogPath(datadir);
    QFile log(path);
    // A new launch gets its own diagnostic stream. The file is opened before
    // spawning so a failed or slow startup still has a readable log.
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(path, log.errorString());
        return false;
    }
    if (!log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error) *error = QStringLiteral("Cannot protect daemon output file: %1").arg(path);
        log.close();
        return false;
    }
    log.close();

    // QProcess pipe notifications require the GUI event loop. The GUI can be
    // blocked in Keychain access during window construction; a verbose daemon
    // then fills its 64 KiB pipe and stops before RPC binds. Direct file
    // redirection keeps draining independently of the GUI on every platform.
    process->setStandardOutputFile(path, QIODevice::Append);
    process->setStandardErrorFile(path, QIODevice::Append);
    return true;
}

}  // namespace dinero::qt
