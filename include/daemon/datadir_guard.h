#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace dinero::daemon {

class DatadirGuard {
public:
    DatadirGuard() = default;
    ~DatadirGuard();

    DatadirGuard(const DatadirGuard&) = delete;
    DatadirGuard& operator=(const DatadirGuard&) = delete;

    bool Acquire(const std::filesystem::path& datadir, std::string& error);
    void Release();

    bool IsHeld() const { return held_; }

    static std::filesystem::path LockFilePath(const std::filesystem::path& datadir);
    static std::filesystem::path PidFilePath(const std::filesystem::path& datadir);
    static std::optional<int> ReadPidFile(const std::filesystem::path& datadir);

private:
    bool CheckOwnership(std::string& error) const;
    bool WritePidFile(std::string& error);
    void RemovePidFile() noexcept;

    std::filesystem::path datadir_;
    std::filesystem::path lock_path_;
    std::filesystem::path pid_path_;
    bool held_ = false;

#ifdef _WIN32
    void* lock_handle_ = nullptr;
#else
    int lock_fd_ = -1;
#endif
};

} // namespace dinero::daemon
