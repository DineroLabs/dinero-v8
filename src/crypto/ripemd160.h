#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace dinero {

struct RIPEMD160_CTX {
    uint32_t s[5];
    uint64_t bytes;
    uint8_t  buf[64];
};

void RIPEMD160_Init(RIPEMD160_CTX* ctx);
void RIPEMD160_Update(RIPEMD160_CTX* ctx, const void* data, size_t len);
void RIPEMD160_Final(uint8_t out[20], RIPEMD160_CTX* ctx);

// Convenience
inline std::array<uint8_t,20> RIPEMD160(const uint8_t* data, size_t len) {
    RIPEMD160_CTX ctx;
    RIPEMD160_Init(&ctx);
    RIPEMD160_Update(&ctx, data, len);
    std::array<uint8_t,20> out{};
    RIPEMD160_Final(out.data(), &ctx);
    return out;
}

} // namespace dinero
