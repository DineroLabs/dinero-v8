#include "daemon/datadir_guard.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace dinero::daemon {

namespace {

int CurrentPid() {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

#ifndef _WIN32
bool ProcessLooksLikeDinerod(int pid) {
    if (pid <= 0) {
        return false;
    }

#if defined(__APPLE__)
    char proc_name_buf[PROC_PIDPATHINFO_MAXSIZE] = {};
    const int rc = proc_name(pid, proc_name_buf, sizeof(proc_name_buf));
    if (rc <= 0) {
        return false;
    }
    return std::string(proc_name_buf).find("dinerod") != std::string::npos;
#elif defined(__linux__)
    const std::string comm_path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream comm_file(comm_path);
    if (!comm_file.is_open()) {
        return false;
    }

    std::string proc_name;
    std::getline(comm_file, proc_name);
    return proc_name.find("dinerod") != std::string::npos;
#else
    return true;
#endif
}

bool ProcessExists(int pid) {
    if (pid <= 0) {
        return false;
    }

    if (::kill(pid, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}
#endif

std::string PathString(const std::filesystem::path& path) {
    return path.string();
}

} // namespace

DatadirGuard::~DatadirGuard() {
    Release();
}

std::filesystem::path DatadirGuard::LockFilePath(const std::filesystem::path& datadir) {
    return datadir / "dinerod.lock";
}

std::filesystem::path DatadirGuard::PidFilePath(const std::filesystem::path& datadir) {
    return datadir / "dinerod.pid";
}

std::optional<int> DatadirGuard::ReadPidFile(const std::filesystem::path& datadir) {
    std::ifstream pid_file(PidFilePath(datadir));
    if (!pid_file.is_open()) {
        return std::nullopt;
    }

    int pid = 0;
    pid_file >> pid;
    if (!pid_file || pid <= 0) {
        return std::nullopt;
    }

    return pid;
}

bool DatadirGuard::CheckOwnership(std::string& error) const {
#ifdef _WIN32
    (void)error;
    return true;
#else
    struct stat st {};
    if (::stat(datadir_.c_str(), &st) != 0) {
        error = "Failed to stat datadir " + PathString(datadir_) + ": " + std::strerror(errno);
        return false;
    }

    const uid_t current_uid = ::geteuid();
    if (st.st_uid != current_uid) {
        std::ostringstream oss;
        oss << "Refusing to start: datadir " << PathString(datadir_)
            << " is owned by uid " << st.st_uid
            << " but dinerod is running as uid " << current_uid;
        error = oss.str();
        return false;
    }

    return true;
#endif
}

bool DatadirGuard::WritePidFile(std::string& error) {
    const std::filesystem::path temp_path = pid_path_.string() + ".tmp";

    std::ofstream pid_file(temp_path, std::ios::trunc);
    if (!pid_file.is_open()) {
        error = "Failed to open PID file " + PathString(temp_path);
        return false;
    }

    pid_file << CurrentPid() << "\n";
    pid_file.flush();
    if (!pid_file) {
        error = "Failed to write PID file " + PathString(temp_path);
        return false;
    }
    pid_file.close();

    std::error_code ec;
    std::filesystem::rename(temp_path, pid_path_, ec);
    if (ec) {
        std::filesystem::remove(pid_path_, ec);
        ec.clear();
        std::filesystem::rename(temp_path, pid_path_, ec);
    }

    if (ec) {
        std::filesystem::remove(temp_path, ec);
        error = "Failed to publish PID file " + PathString(pid_path_) + ": " + ec.message();
        return false;
    }

    return true;
}

void DatadirGuard::RemovePidFile() noexcept {
    std::error_code ec;
    const auto pid = ReadPidFile(datadir_);
    if (pid && *pid != CurrentPid()) {
        return;
    }
    std::filesystem::remove(pid_path_, ec);
}

bool DatadirGuard::Acquire(const std::filesystem::path& datadir, std::string& error) {
    Release();

    datadir_ = std::filesystem::absolute(datadir);
    lock_path_ = LockFilePath(datadir_);
    pid_path_ = PidFilePath(datadir_);

    std::error_code ec;
    if (std::filesystem::exists(datadir_, ec) && !ec &&
        !std::filesystem::is_directory(datadir_, ec)) {
        error = "Configured datadir is not a directory: " + PathString(datadir_);
        return false;
    }

    ec.clear();
    std::filesystem::create_directories(datadir_, ec);
    if (ec) {
        error = "Failed to create datadir " + PathString(datadir_) + ": " + ec.message();
        return false;
    }

    if (!std::filesystem::is_directory(datadir_, ec) || ec) {
        error = "Configured datadir is not a directory: " + PathString(datadir_);
        return false;
    }

    if (!CheckOwnership(error)) {
        return false;
    }

#ifdef _WIN32
    lock_handle_ = ::CreateFileW(lock_path_.wstring().c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 nullptr,
                                 OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (lock_handle_ == INVALID_HANDLE_VALUE) {
        const DWORD last_error = GetLastError();
        std::ostringstream oss;
        oss << "Refusing to start: datadir " << PathString(datadir_)
            << " is already in use or lock file could not be opened (win32=" << last_error << ")";
        error = oss.str();
        lock_handle_ = nullptr;
        return false;
    }
#else
    lock_fd_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock_fd_ < 0) {
        error = "Failed to open datadir lock file " + PathString(lock_path_) + ": " + std::strerror(errno);
        return false;
    }

    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        std::ostringstream oss;
        oss << "Refusing to start: datadir " << PathString(datadir_) << " is already in use";
        const auto existing_pid = ReadPidFile(datadir_);
        if (existing_pid && ProcessExists(*existing_pid)) {
            oss << " by PID " << *existing_pid;
            if (ProcessLooksLikeDinerod(*existing_pid)) {
                oss << " (dinerod)";
            }
        }
        error = oss.str();
        ::close(lock_fd_);
        lock_fd_ = -1;
        return false;
    }

    const auto existing_pid = ReadPidFile(datadir_);
    if (existing_pid &&
        *existing_pid != CurrentPid() &&
        ProcessExists(*existing_pid) &&
        ProcessLooksLikeDinerod(*existing_pid)) {
        error = "Refusing to start: existing PID file points to live dinerod process " +
                std::to_string(*existing_pid) + " for datadir " + PathString(datadir_);
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
        return false;
    }
#endif

    if (!WritePidFile(error)) {
        Release();
        return false;
    }

    held_ = true;
    return true;
}

void DatadirGuard::Release() {
    if (!held_) {
#ifdef _WIN32
        if (lock_handle_) {
            ::CloseHandle(static_cast<HANDLE>(lock_handle_));
            lock_handle_ = nullptr;
        }
#else
        if (lock_fd_ >= 0) {
            ::flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
#endif
        return;
    }

    RemovePidFile();

#ifdef _WIN32
    if (lock_handle_) {
        ::CloseHandle(static_cast<HANDLE>(lock_handle_));
        lock_handle_ = nullptr;
    }
#else
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
#endif

    held_ = false;
}

} // namespace dinero::daemon
