#include "ripemd160_standalone.h"
#include <cstring>

namespace dinero {
namespace crypto {

// RIPEMD160 implementation based on the project's canonical internal
// implementation. Keep this standalone wrapper vector-compatible with
// src/crypto/ripemd160.cpp.

namespace {

inline uint32_t rol(uint32_t x, unsigned n) {
    n &= 31u;
    return (x << n) | (x >> ((32u - n) & 31u));
}

inline uint32_t ReadLE32(const unsigned char* ptr) {
    return static_cast<uint32_t>(ptr[0])
         | (static_cast<uint32_t>(ptr[1]) << 8)
         | (static_cast<uint32_t>(ptr[2]) << 16)
         | (static_cast<uint32_t>(ptr[3]) << 24);
}

inline void WriteLE32(unsigned char* ptr, uint32_t x) {
    ptr[0] = static_cast<unsigned char>(x);
    ptr[1] = static_cast<unsigned char>(x >> 8);
    ptr[2] = static_cast<unsigned char>(x >> 16);
    ptr[3] = static_cast<unsigned char>(x >> 24);
}

inline uint32_t f0(uint32_t x,uint32_t y,uint32_t z){ return x ^ y ^ z; }
inline uint32_t f1(uint32_t x,uint32_t y,uint32_t z){ return (x & y) | (~x & z); }
inline uint32_t f2(uint32_t x,uint32_t y,uint32_t z){ return (x | ~y) ^ z; }
inline uint32_t f3(uint32_t x,uint32_t y,uint32_t z){ return (x & z) | (y & ~z); }
inline uint32_t f4(uint32_t x,uint32_t y,uint32_t z){ return x ^ (y | ~z); }

static constexpr uint32_t K0 = 0x00000000u, K1 = 0x5A827999u, K2 = 0x6ED9EBA1u,
                          K3 = 0x8F1BBCDCu, K4 = 0xA953FD4Eu;
static constexpr uint32_t KK0= 0x50A28BE6u, KK1= 0x5C4DD124u, KK2= 0x6D703EF3u,
                          KK3= 0x7A6D76E9u, KK4= 0x00000000u;

static const uint8_t r [80] = {
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
  3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12,
  1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
  4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13
};
static const uint8_t rr[80] = {
  5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12,
  6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
  15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13,
  8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
  12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11
};
static const uint8_t s [80] = {
  11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8,
  7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
  11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5,
  11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
  9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6
};
static const uint8_t ss[80] = {
  8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6,
  9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
  9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5,
  15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
  8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11
};

void Transform(uint32_t* st, const unsigned char* chunk) {
    uint32_t X[16];
    for (int idx = 0; idx < 16; ++idx) X[idx] = ReadLE32(chunk + 4 * idx);

    uint32_t a=st[0], b=st[1], c=st[2], d=st[3], e=st[4];
    uint32_t A=a,    B=b,    C=c,    D=d,    E=e;

    for (int idx=0;  idx<16; ++idx){ uint32_t t = rol(a + f0(b,c,d) + X[r[idx ]] + K0, s[idx ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int idx=16; idx<32; ++idx){ uint32_t t = rol(a + f1(b,c,d) + X[r[idx ]] + K1, s[idx ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int idx=32; idx<48; ++idx){ uint32_t t = rol(a + f2(b,c,d) + X[r[idx ]] + K2, s[idx ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int idx=48; idx<64; ++idx){ uint32_t t = rol(a + f3(b,c,d) + X[r[idx ]] + K3, s[idx ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int idx=64; idx<80; ++idx){ uint32_t t = rol(a + f4(b,c,d) + X[r[idx ]] + K4, s[idx ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }

    for (int idx=0;  idx<16; ++idx){ uint32_t t = rol(A + f4(B,C,D) + X[rr[idx ]] + KK0, ss[idx ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int idx=16; idx<32; ++idx){ uint32_t t = rol(A + f3(B,C,D) + X[rr[idx ]] + KK1, ss[idx ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int idx=32; idx<48; ++idx){ uint32_t t = rol(A + f2(B,C,D) + X[rr[idx ]] + KK2, ss[idx ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int idx=48; idx<64; ++idx){ uint32_t t = rol(A + f1(B,C,D) + X[rr[idx ]] + KK3, ss[idx ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int idx=64; idx<80; ++idx){ uint32_t t = rol(A + f0(B,C,D) + X[rr[idx ]] + KK4, ss[idx ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }

    uint32_t T = st[1] + c + D;
    st[1]      = st[2] + d + E;
    st[2]      = st[3] + e + A;
    st[3]      = st[4] + a + B;
    st[4]      = st[0] + b + C;
    st[0]      = T;
}

} // anonymous namespace

CRIPEMD160::CRIPEMD160() {
    Reset();
}

CRIPEMD160& CRIPEMD160::Reset() {
    bytes = 0;
    s[0] = 0x67452301ul;
    s[1] = 0xEFCDAB89ul;
    s[2] = 0x98BADCFEul;
    s[3] = 0x10325476ul;
    s[4] = 0xC3D2E1F0ul;
    return *this;
}

CRIPEMD160& CRIPEMD160::Write(const unsigned char* data, size_t len) {
    const unsigned char* end = data + len;
    size_t bufsize = bytes % 64;
    if (bufsize && bufsize + len >= 64) {
        memcpy(buf + bufsize, data, 64 - bufsize);
        bytes += 64 - bufsize;
        data += 64 - bufsize;
        Transform(s, buf);
        bufsize = 0;
    }
    while (end - data >= 64) {
        Transform(s, data);
        bytes += 64;
        data += 64;
    }
    if (end > data) {
        memcpy(buf + bufsize, data, end - data);
        bytes += end - data;
    }
    return *this;
}

void CRIPEMD160::Finalize(unsigned char hash[OUTPUT_SIZE]) {
    static const unsigned char pad[64] = {0x80};
    unsigned char sizedesc[8];
    WriteLE32(sizedesc, bytes << 3);
    WriteLE32(sizedesc + 4, bytes >> 29);
    Write(pad, 1 + ((119 - (bytes % 64)) % 64));
    Write(sizedesc, 8);
    WriteLE32(hash, s[0]);
    WriteLE32(hash + 4, s[1]);
    WriteLE32(hash + 8, s[2]);
    WriteLE32(hash + 12, s[3]);
    WriteLE32(hash + 16, s[4]);
}

} // namespace crypto
} // namespace dinero
