#include "crypto/pbkdf2.h"
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using dinero::crypto::PBKDF2_HMAC_SHA512;
std::vector<uint8_t> Hex(const std::string& text) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}
std::vector<uint8_t> Pattern(size_t size, unsigned multiplier) {
    std::vector<uint8_t> value(size);
    for (size_t i = 0; i < size; ++i) value[i] = static_cast<uint8_t>(i * multiplier);
    return value;
}
TEST(PBKDF2Compatibility, FixedWalletWorkFactors) {
    const auto password = Pattern(17, 73), salt = Pattern(32, 41);
    struct Vector {uint32_t rounds; const char* hex;};
    const Vector vectors[] = {
        {1, "1167066773ab5a7864034f6a7515cd9750f49bf176638e6ef08cd3dc1023a02780144e0f4671cb89fe94a93aebcd1d67dfc5f472aac2780753c65a1aa18c9803"},
        {2, "6cb1e77175ab42ef9547a79552705c260cca2deb149f5f0077062cb004523ec4a470c9afdf3e127d21e63aca8ac9cbb74f3e48a0b338e3768c7d707a92fe610f"},
        {2048, "6b28910c7afdc202e6f78d253d4b67022d83dc10f3a19d09122be8a45ee452c7af98871b5b060340d2e58616eb20a5ba771d5d188df25be0fb192c3273e756f7"},
        {100000, "c8363b214f887dd23801ae543756d9f439f4570db15b05f21af64be9f89516438d5ede655e9bad472ea19d1ef5f276b34be7da16e1f5e1f42a9573ce346af21e"},
        {210000, "abbfe0665a142a7247a1fc27a55eb0a812b19205d11af88a7eca53c900e1a5cad02918f51a92c27c6ee151dcd9d248b575928a79bdd4a98a405558cddd3c33af"},
        {600000, "25f95d51bd7d6806f19696033c508d54922d5ab65ad07ffb1ba90c2cdba4957298c689e4d8121291febcf8b250acc05cfc85627441fe4e3468cfe6120117f1e7"},
    };
    for (const auto& vector : vectors) {
        SCOPED_TRACE(vector.rounds);
        std::array<uint8_t, 64> out{};
        PBKDF2_HMAC_SHA512(password.data(), password.size(), salt.data(), salt.size(),
                           vector.rounds, out.data(), out.size());
        EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.end()), Hex(vector.hex));
    }
}
TEST(PBKDF2Compatibility, BinaryBoundariesAndMultipleBlocks) {
    for (size_t plen : {0u, 1u, 127u, 128u, 129u, 257u})
        for (size_t slen : {0u, 4u, 32u})
            for (size_t olen : {1u, 32u, 63u, 64u, 65u, 128u, 129u}) {
                SCOPED_TRACE(::testing::Message() << plen << "/" << slen << "/" << olen);
                const auto password = Pattern(plen, 73), salt = Pattern(slen, 41);
                std::vector<uint8_t> out(olen), oracle(olen);
                PBKDF2_HMAC_SHA512(password.data(), plen, salt.data(), slen, 3, out.data(), olen);
                ASSERT_EQ(PKCS5_PBKDF2_HMAC(plen ? reinterpret_cast<const char*>(password.data()) : "",
                    static_cast<int>(plen), salt.data(), static_cast<int>(slen), 3, EVP_sha512(),
                    static_cast<int>(olen), oracle.data()), 1);
                EXPECT_EQ(out, oracle);
            }
}
TEST(PBKDF2Compatibility, RejectsInvalidParametersWithoutOutputChanges) {
    std::array<uint8_t, 64> output; output.fill(0xa5); const auto before = output;
    const uint8_t byte = 7;
    const auto refuse = [&](const uint8_t* pass, size_t plen, const uint8_t* salt,
                             size_t slen, uint32_t rounds, size_t olen) {
        EXPECT_THROW(PBKDF2_HMAC_SHA512(pass, plen, salt, slen, rounds, output.data(), olen),
                     std::invalid_argument);
        EXPECT_EQ(output, before);
    };
    refuse(nullptr, 1, &byte, 1, 1, 64);
    refuse(&byte, 1, nullptr, 1, 1, 64);
    refuse(&byte, 1, &byte, 1, 0, 64);
    refuse(&byte, size_t(std::numeric_limits<int>::max()) + 1, &byte, 1, 1, 64);
    refuse(&byte, 1, &byte, std::numeric_limits<size_t>::max(), 1, 64);
    if constexpr (sizeof(size_t) > sizeof(uint32_t))
        refuse(&byte, 1, &byte, 1, 1, (uint64_t(std::numeric_limits<uint32_t>::max()) + 1) * 64);
    EXPECT_THROW(PBKDF2_HMAC_SHA512(&byte, 1, &byte, 1, 1, nullptr, 64), std::invalid_argument);
    EXPECT_EQ(output, before);
}
TEST(PBKDF2Compatibility, ZeroOutputIsAnInertRequest) {
    EXPECT_NO_THROW(PBKDF2_HMAC_SHA512(nullptr, 123, nullptr, 123, 0, nullptr, 0));
}
TEST(PBKDF2Compatibility, AliasedInputRemainsIntactUntilDerivationFinishes) {
    for (bool password_alias : {false, true}) {
        auto buffer = Pattern(129, 73); const auto salt = Pattern(32, 41);
        std::vector<uint8_t> expected(129);
        if (password_alias) {
            PBKDF2_HMAC_SHA512(buffer.data(), buffer.size(), salt.data(), salt.size(), 3,
                               expected.data(), expected.size());
            PBKDF2_HMAC_SHA512(buffer.data(), buffer.size(), salt.data(), salt.size(), 3,
                               buffer.data(), buffer.size());
        } else {
            PBKDF2_HMAC_SHA512(salt.data(), salt.size(), buffer.data(), buffer.size(), 3,
                               expected.data(), expected.size());
            PBKDF2_HMAC_SHA512(salt.data(), salt.size(), buffer.data(), buffer.size(), 3,
                               buffer.data(), buffer.size());
        }
        EXPECT_EQ(buffer, expected);
    }
}
TEST(PBKDF2Compatibility, UnavailableProviderLeavesOutputUntouched) {
    struct Restore {
        ~Restore() { EVP_set_default_properties(nullptr, nullptr); }
    } restore;
    std::array<uint8_t, 64> output; output.fill(0xa5); const auto before = output;
    const uint8_t byte = 7;
    ASSERT_EQ(EVP_set_default_properties(nullptr, "provider=dinero_missing_test_provider"), 1);
    EXPECT_THROW(PBKDF2_HMAC_SHA512(&byte, 1, &byte, 1, 3, output.data(), output.size()),
                 std::runtime_error);
    EXPECT_EQ(output, before);
}
} // namespace
