#pragma once

#include <atomic>
#include <string>
#include <string_view>

namespace dinero::daemon {

// A single atomic value keeps the refusal flag and its public reason in sync.
// Never put raw migration errors (which can contain local paths) on RPC.
enum class StorageUpgradeIssue { None, InsufficientDisk, OpenFileLimit, Other };

inline StorageUpgradeIssue ClassifyStorageUpgradeIssue(std::string_view detail) {
    std::string lower;
    lower.reserve(detail.size());
    for (const unsigned char ch : detail)
        lower.push_back(ch >= 'A' && ch <= 'Z' ? char(ch - 'A' + 'a') : char(ch));
    if (lower.find("not enough free disk") != std::string::npos ||
        lower.find("no space left on device") != std::string::npos ||
        lower.find("disk full") != std::string::npos)
        return StorageUpgradeIssue::InsufficientDisk;
    if (lower.find("too many open files") != std::string::npos ||
        lower.find("open-file limit") != std::string::npos)
        return StorageUpgradeIssue::OpenFileLimit;
    return StorageUpgradeIssue::Other;
}

inline const char* StorageUpgradeIssueCode(StorageUpgradeIssue issue) {
    switch (issue) {
        case StorageUpgradeIssue::None: return "none";
        case StorageUpgradeIssue::InsufficientDisk: return "insufficient_disk";
        case StorageUpgradeIssue::OpenFileLimit: return "open_file_limit";
        case StorageUpgradeIssue::Other: return "other";
    }
    return "other";
}

}  // namespace dinero::daemon
