#include "daemonoutput.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dinero::qt {

QString daemonOutputLogPath(const QString& datadir) {
    return QDir(datadir).filePath(QStringLiteral("embedded-daemon.log"));
}

bool configureDaemonOutput(QProcess* process, const QString& datadir, QString* error) {
    if (!process) {
        if (error) *error = QStringLiteral("Daemon process is missing");
        return false;
    }
#ifdef Q_OS_UNIX
    const bool datadirExisted = QFileInfo::exists(datadir);
#endif
    if (datadir.isEmpty() || !QDir().mkpath(datadir)) {
        if (error) *error = QStringLiteral("Cannot create daemon data directory: %1").arg(datadir);
        return false;
    }
#ifdef Q_OS_UNIX
    if (!datadirExisted &&
        !QFile::setPermissions(datadir, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner)) {
        if (error) *error = QStringLiteral("Cannot protect daemon data directory: %1").arg(datadir);
        return false;
    }
#endif

    const QString path = daemonOutputLogPath(datadir);
    if (QFileInfo(path).isSymLink()) {
        if (error) *error = QStringLiteral("Daemon output path is a symbolic link: %1").arg(path);
        return false;
    }
    QFile log(path);
#ifdef Q_OS_UNIX
    constexpr auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (QFileInfo::exists(path) && !log.setPermissions(ownerOnly)) {
        if (error) *error = QStringLiteral("Cannot protect daemon output file: %1").arg(path);
        return false;
    }
#endif
    // Truncate the previous launch's log. Creation uses owner-only mode from
    // the first open, before any potentially sensitive daemon output arrives.
#ifdef Q_OS_UNIX
    const QByteArray nativePath = QFile::encodeName(path);
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(nativePath.constData(), flags, 0600);
    if (fd < 0) {
        if (error) *error = QStringLiteral("Cannot open %1: %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        const int savedErrno = errno;
        ::close(fd);
        if (error) *error = QStringLiteral("Cannot protect daemon output file %1: %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }
    ::close(fd);
#else
    // Windows inherits the user data directory's NTFS ACL. Qt's permission
    // flags do not change NTFS ACLs, so use the older QFile API supported by
    // the release build instead of promising owner-only POSIX semantics.
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(path, log.errorString());
        return false;
    }
    log.close();
#endif

    // QProcess pipe notifications require the GUI event loop. The GUI can be
    // blocked in Keychain access during window construction; a verbose daemon
    // then fills its 64 KiB pipe and stops before RPC binds. Direct file
    // redirection keeps draining independently of the GUI on every platform.
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardOutputFile(path, QIODevice::Append);
    return true;
}

}  // namespace dinero::qt
