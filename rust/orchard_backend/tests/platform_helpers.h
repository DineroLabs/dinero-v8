#pragma once
// Test-only host helpers. No wallet, runtime or release code uses these.
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace dinero::orchard::test {
// _spawnv joins argv with spaces; quote each argument for the MSVC CRT parser.
// Keep backslashes verbatim except before quotes and the closing delimiter.
inline std::string WindowsArgument(std::string_view value) {
    if (value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("NUL in test process argument");
    std::string result(1, '"');
    size_t slashes = 0;
    for (const char c : value) {
        if (c == '\\') { ++slashes; continue; }
        result.append(c == '"' ? 2 * slashes + 1 : slashes, '\\');
        result.push_back(c);
        slashes = 0;
    }
    result.append(2 * slashes, '\\');
    result.push_back('"');
    return result;
}

class TemporaryDirectory final {
public:
    std::filesystem::path p;
    explicit TemporaryDirectory(std::string_view prefix) {
        const auto base = std::filesystem::temp_directory_path();
        std::random_device random;
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            std::string name(prefix);
            name += '-';
            for (unsigned word = 0; word < 4; ++word) {
                const auto value = random();
                for (unsigned digit = 0; digit < 8; ++digit)
                    name += hex[(value >> (4 * digit)) & 15];
            }
            const auto candidate = base / name;
            std::error_code error;
            // Only a successful exclusive creation grants cleanup ownership.
            // Existing paths are never adopted, opened or removed.
            if (std::filesystem::create_directory(candidate, error)) {
                p = candidate;
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("create test directory", candidate, error);
        }
        throw std::runtime_error("Cannot allocate unique test directory");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(p, ignored);
    }
};
} // namespace dinero::orchard::test
