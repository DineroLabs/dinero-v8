#pragma once
#include <string>
#include <optional>

// Daemon-only version: no Qt dependency
namespace dinero::miningaddr {
    std::optional<std::string> load(const std::string& datadir, const std::string& netName, std::string* why = nullptr);
    bool save(const std::string& datadir, const std::string& netName, const std::string& address, std::string* why = nullptr);
}
