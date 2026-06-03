// issue #224: size-based log rotation + reopen for the daemon Logger.
// gtest assertions gate in release builds (unlike assert(), which is a no-op
// under NDEBUG).

#include "common/logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {
std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

// When the active log crosses max_bytes it rotates, and only max_files archives
// are kept (older dropped) — so disk use is bounded.
TEST(LoggerRotation, RotatesAndCapsArchives) {
    const auto dir = fs::temp_directory_path() / "dinero_logrot_rotate";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string logpath = (dir / "debug.log").string();

    dinero::Logger logger;
    logger.setLogLevel(dinero::LogLevel::INFO);
    logger.setLogFile(logpath);
    logger.setRotation(/*max_bytes=*/1000, /*max_files=*/3);

    const std::string msg(50, 'x');  // each line ~ "[INFO] " + 50 = 58 bytes
    for (int i = 0; i < 400; ++i) {
        logger.info(msg);
    }
    logger.shutdown();  // flush + close

    ASSERT_TRUE(fs::exists(logpath));
    // Active file is bounded (cap + at most one over-the-line write).
    EXPECT_LT(fs::file_size(logpath), static_cast<std::uintmax_t>(1000 + 200));
    // Exactly max_files archives are kept.
    EXPECT_TRUE(fs::exists(logpath + ".1"));
    EXPECT_TRUE(fs::exists(logpath + ".2"));
    EXPECT_TRUE(fs::exists(logpath + ".3"));
    EXPECT_FALSE(fs::exists(logpath + ".4"));  // capped at max_files=3

    fs::remove_all(dir);
}

// max_bytes == 0 disables rotation (legacy unbounded behavior).
TEST(LoggerRotation, RotationDisabledWhenZero) {
    const auto dir = fs::temp_directory_path() / "dinero_logrot_off";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string logpath = (dir / "debug.log").string();

    dinero::Logger logger;
    logger.setLogFile(logpath);
    logger.setRotation(/*max_bytes=*/0, /*max_files=*/3);  // disabled
    for (int i = 0; i < 200; ++i) {
        logger.info(std::string(50, 'y'));
    }
    logger.shutdown();

    EXPECT_FALSE(fs::exists(logpath + ".1"));  // never rotated
    EXPECT_GT(fs::file_size(logpath), static_cast<std::uintmax_t>(200 * 50));

    fs::remove_all(dir);
}

// requestReopen() (the SIGHUP path) makes the next write reopen the configured
// path — so external logrotate that renamed the file out gets a fresh file
// instead of the daemon leaking writes to the renamed inode.
TEST(LoggerRotation, ReopenAfterExternalRename) {
    const auto dir = fs::temp_directory_path() / "dinero_logrot_reopen";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string logpath = (dir / "debug.log").string();

    dinero::Logger logger;
    logger.setLogFile(logpath);
    logger.setRotation(/*max_bytes=*/0, /*max_files=*/5);  // rotation off; testing reopen
    logger.info("before");

    // Simulate external logrotate moving the file aside.
    fs::rename(logpath, logpath + ".moved");
    ASSERT_FALSE(fs::exists(logpath));

    logger.requestReopen();
    logger.info("after");  // triggers reopen -> recreates the original path
    logger.shutdown();

    ASSERT_TRUE(fs::exists(logpath));                                  // reopened
    EXPECT_NE(ReadFile(logpath).find("after"), std::string::npos);     // new line in new file
    EXPECT_NE(ReadFile(logpath + ".moved").find("before"), std::string::npos);  // old line in moved file

    fs::remove_all(dir);
}
