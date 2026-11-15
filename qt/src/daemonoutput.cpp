#include "daemonoutput.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    const bool datadirExisted = QFileInfo::exists(datadir);
    if (datadir.isEmpty() || !QDir().mkpath(datadir)) {
        if (error) *error = QStringLiteral("Cannot create daemon data directory: %1").arg(datadir);
        return false;
    }
    if (!datadirExisted &&
        !QFile::setPermissions(datadir, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner)) {
        if (error) *error = QStringLiteral("Cannot protect daemon data directory: %1").arg(datadir);
        return false;
    }

    const QString path = daemonOutputLogPath(datadir);
    if (QFileInfo(path).isSymLink()) {
        if (error) *error = QStringLiteral("Daemon output path is a symbolic link: %1").arg(path);
        return false;
    }
    QFile log(path);
    constexpr auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (QFileInfo::exists(path) && !log.setPermissions(ownerOnly)) {
        if (error) *error = QStringLiteral("Cannot protect daemon output file: %1").arg(path);
        return false;
    }
    // Truncate the previous launch's log. Creation uses owner-only mode from
    // the first open, before any potentially sensitive daemon output arrives.
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate, ownerOnly)) {
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(path, log.errorString());
        return false;
    }
    log.close();

    // QProcess pipe notifications require the GUI event loop. The GUI can be
    // blocked in Keychain access during window construction; a verbose daemon
    // then fills its 64 KiB pipe and stops before RPC binds. Direct file
    // redirection keeps draining independently of the GUI on every platform.
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardOutputFile(path, QIODevice::Append);
    return true;
}

}  // namespace dinero::qt
