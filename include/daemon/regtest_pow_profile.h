#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace dinero::daemon {

// Called with the datadir lock held, before even the genesis guard opens a DB.
inline void CheckRegtestPowDatadir(const std::filesystem::path& path, bool enabled) {
    const auto marker = path / "regtest-pow-profile";
    if (std::filesystem::exists(marker)) {
        if (!enabled) throw std::runtime_error("PoW profile requires --regtest-enforce-pow");
        return;
    }
    if (!enabled) return;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().filename() != "dinerod.lock" && entry.path().filename() != "dinerod.pid")
            throw std::runtime_error("PoW profile requires a fresh datadir; no automatic migration");
    }
}

inline void BindRegtestPowProfile(const std::filesystem::path& path, const std::string& checksum) {
    const auto marker = path / "regtest-pow-profile";
    const auto expected = "regtest-pow-profile-v1\n" + checksum + "\n";
    if (std::filesystem::exists(marker)) {
        std::ifstream input(marker, std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(input)), {});
        if (!input || actual != expected)
            throw std::runtime_error("PoW profile mismatch: use the original parameters or a fresh datadir");
        return;
    }
    const auto temporary = path / "regtest-pow-profile.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << expected;
        output.close();
        if (!output) throw std::runtime_error("PoW profile could not be written");
    }
    std::filesystem::rename(temporary, marker);
}

} // namespace dinero::daemon
