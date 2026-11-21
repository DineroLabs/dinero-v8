#pragma once

#include <QDir>
#include <QFile>
#include <QString>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// Hold the same OS-level datadir lock as dinerod while copying wallet SQLite
// files. Qt may be disconnected from an external daemon, so connection state
// alone cannot prove that a raw file copy is safe.
class WalletBackupDatadirLock final {
public:
  explicit WalletBackupDatadirLock(const QString& dataDir) {
    const QString path = QDir(dataDir).filePath(QStringLiteral("dinerod.lock"));
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    handle_ = ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) handle_ = nullptr;
#else
    const QByteArray native = QFile::encodeName(path);
    fd_ = ::open(native.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ >= 0 && ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }
  ~WalletBackupDatadirLock() {
#ifdef Q_OS_WIN
    if (handle_) ::CloseHandle(handle_);
#else
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
#endif
  }
  WalletBackupDatadirLock(const WalletBackupDatadirLock&) = delete;
  WalletBackupDatadirLock& operator=(const WalletBackupDatadirLock&) = delete;
  bool held() const {
#ifdef Q_OS_WIN
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
  }
private:
#ifdef Q_OS_WIN
  HANDLE handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};
