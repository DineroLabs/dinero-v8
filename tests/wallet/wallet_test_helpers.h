#pragma once

#include <filesystem>
#include <string>
#include <random>
#include <cstdlib>
#include <sstream>

namespace dinero::wallet::test {

/// Generate random suffix for temp directories
inline std::string random_suffix() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);
    return std::to_string(dis(gen));
}

/// Create temporary test directory with random suffix
inline std::filesystem::path create_temp_dir(const std::string& prefix) {
    auto path = std::filesystem::path("/tmp") / (prefix + "_" + random_suffix());
    std::filesystem::create_directories(path);
    return path;
}

/// Test seed constants (BIP39 standard test vectors)
inline const std::string TEST_SEED_1 =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

inline const std::string TEST_SEED_2 =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";

inline const std::string TEST_SEED_3 =
    "test raw radar develop emotion spike reward frozen current echo echo gain";

/// Coinbase maturity (blocks required before coinbase UTXO is spendable)
inline constexpr uint32_t COINBASE_MATURITY = 100;

} // namespace dinero::wallet::test
