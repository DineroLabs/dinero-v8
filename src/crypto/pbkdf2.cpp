#include "crypto/pbkdf2.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dinero::crypto {
namespace {
constexpr size_t HashLength = 64;
struct SensitiveBytes {
    explicit SensitiveBytes(size_t size) : bytes(size) {}
    ~SensitiveBytes() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
    std::vector<uint8_t> bytes;
};
struct Intermediates {
    std::array<uint8_t, HashLength> u{}, t{};
    ~Intermediates() {
        OPENSSL_cleanse(u.data(), u.size());
        OPENSSL_cleanse(t.data(), t.size());
    }
};
}

void PBKDF2_HMAC_SHA512(const uint8_t* password, size_t password_len,
                      const uint8_t* salt, size_t salt_len, uint32_t iterations,
                      uint8_t* output, size_t output_len) {
    // A zero-length request does not access any buffers or fetch a provider.
    if (output_len == 0) return;
    const size_t blocks = output_len / HashLength + (output_len % HashLength != 0);
    if ((!password && password_len != 0) || (!salt && salt_len != 0) || !output ||
        iterations == 0 || password_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        salt_len > std::numeric_limits<size_t>::max() - 4 ||
        blocks > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Invalid PBKDF2 parameters");
    }

    // Reuse the keyed HMAC state within this derivation. The old one-shot HMAC
    // call fetched/allocated a context on every round. Work factors and the
    // PBKDF2 round/XOR algorithm remain unchanged; no state is shared by calls.
    std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> algorithm(
        EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);
    std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> context(
        algorithm ? EVP_MAC_CTX_new(algorithm.get()) : nullptr, EVP_MAC_CTX_free);
    char digest[] = "SHA512";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end()};
    const uint8_t empty = 0;
    if (!context || EVP_MAC_init(context.get(), password_len ? password : &empty,
                                 password_len, params) != 1) {
        throw std::runtime_error("PBKDF2 HMAC initialization failed");
    }
    const auto keyed_hmac = [&](const uint8_t* data, size_t size, uint8_t* result) {
        size_t written = 0;
        if (EVP_MAC_init(context.get(), nullptr, 0, nullptr) != 1 ||
            EVP_MAC_update(context.get(), data, size) != 1 ||
            EVP_MAC_final(context.get(), result, &written, HashLength) != 1 ||
            written != HashLength) {
            throw std::runtime_error("PBKDF2 HMAC computation failed");
        }
    };

    Intermediates state;
    SensitiveBytes derived(output_len);
    std::vector<uint8_t> salt_block(salt_len + 4);
    if (salt_len != 0) std::memcpy(salt_block.data(), salt, salt_len);
    // The wider loop counter cannot wrap after the last permitted RFC block.
    for (uint64_t block = 1; block <= blocks; ++block) {
        for (size_t i = 0; i < 4; ++i)
            salt_block[salt_len + i] = static_cast<uint8_t>(block >> (24 - 8 * i));
        keyed_hmac(salt_block.data(), salt_block.size(), state.u.data());
        state.t = state.u;
        for (uint32_t round = 1; round < iterations; ++round) {
            keyed_hmac(state.u.data(), state.u.size(), state.u.data());
            for (size_t i = 0; i < HashLength; ++i) state.t[i] ^= state.u[i];
        }
        const size_t offset = static_cast<size_t>(block - 1) * HashLength;
        std::memcpy(derived.bytes.data() + offset, state.t.data(),
                    std::min(HashLength, output_len - offset));
    }
    // No partial derived key escapes on allocation/provider failure. Staging
    // also keeps password/salt bytes intact when output aliases caller input.
    std::memcpy(output, derived.bytes.data(), output_len);
}
} // namespace dinero::crypto
