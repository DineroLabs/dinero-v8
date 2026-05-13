#include <gtest/gtest.h>

#include "daemon/datadir_guard.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace {

int CurrentPidForTest() {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

std::filesystem::path MakeTempDir(const std::string& name) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               (name + "-" + std::to_string(CurrentPidForTest()) + "-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

struct TempDir {
    explicit TempDir(const std::string& name) : path(MakeTempDir(name)) {}
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

} // namespace

TEST(DatadirGuard, AcquireWritesPidFile) {
    TempDir tmp("datadir-guard-pid");
    dinero::daemon::DatadirGuard guard;
    std::string error;

    ASSERT_TRUE(guard.Acquire(tmp.path, error)) << error;
    ASSERT_TRUE(guard.IsHeld());

    const auto pid = dinero::daemon::DatadirGuard::ReadPidFile(tmp.path);
    ASSERT_TRUE(pid.has_value());
    EXPECT_EQ(*pid, CurrentPidForTest());
}

TEST(DatadirGuard, StalePidFileIsReplaced) {
    TempDir tmp("datadir-guard-stale");
    {
        std::ofstream stale_pid(dinero::daemon::DatadirGuard::PidFilePath(tmp.path));
        stale_pid << 999999 << "\n";
    }

    dinero::daemon::DatadirGuard guard;
    std::string error;
    ASSERT_TRUE(guard.Acquire(tmp.path, error)) << error;

    const auto pid = dinero::daemon::DatadirGuard::ReadPidFile(tmp.path);
    ASSERT_TRUE(pid.has_value());
    EXPECT_EQ(*pid, CurrentPidForTest());
}

TEST(DatadirGuard, RejectsNonDirectoryDatadir) {
    TempDir tmp("datadir-guard-file");
    const auto not_a_dir = tmp.path / "plain-file";
    {
        std::ofstream out(not_a_dir);
        out << "not a directory\n";
    }

    dinero::daemon::DatadirGuard guard;
    std::string error;
    ASSERT_FALSE(guard.Acquire(not_a_dir, error));
    EXPECT_NE(error.find("not a directory"), std::string::npos);
}

#ifndef _WIN32
TEST(DatadirGuard, BlocksSecondProcessWhileHeld) {
    TempDir tmp("datadir-guard-lock");
    dinero::daemon::DatadirGuard guard;
    std::string error;
    ASSERT_TRUE(guard.Acquire(tmp.path, error)) << error;

    pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        dinero::daemon::DatadirGuard child_guard;
        std::string child_error;
        const bool acquired = child_guard.Acquire(tmp.path, child_error);
        _exit(acquired ? 1 : 0);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
