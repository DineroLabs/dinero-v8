// Working RIPEMD-160 implementation (MIT license)
#include "dinero/core/crypto/ripemd160.h"
#include <cstring>

namespace dinero {

static inline uint32_t rol(uint32_t x, unsigned n) {
    n &= 31u;
    return (x << n) | (x >> ((32u - n) & 31u));
}

static inline uint32_t load32_le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint32_t f0(uint32_t x,uint32_t y,uint32_t z){ return x ^ y ^ z; }
static inline uint32_t f1(uint32_t x,uint32_t y,uint32_t z){ return (x & y) | (~x & z); }
static inline uint32_t f2(uint32_t x,uint32_t y,uint32_t z){ return (x | ~y) ^ z; }
static inline uint32_t f3(uint32_t x,uint32_t y,uint32_t z){ return (x & z) | (y & ~z); }
static inline uint32_t f4(uint32_t x,uint32_t y,uint32_t z){ return x ^ (y | ~z); }

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

static void compress(uint32_t st[5], const uint8_t block[64]) {
    uint32_t X[16];
    for (int i=0;i<16;++i) X[i]=load32_le(block + 4*i);

    uint32_t a=st[0], b=st[1], c=st[2], d=st[3], e=st[4];
    uint32_t A=a,     B=b,     C=c,     D=d,     E=e;

    for (int j=0;  j<16; ++j){ uint32_t t = rol(a + f0(b,c,d) + X[r[j ]] + K0, s[j ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int j=16; j<32; ++j){ uint32_t t = rol(a + f1(b,c,d) + X[r[j ]] + K1, s[j ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int j=32; j<48; ++j){ uint32_t t = rol(a + f2(b,c,d) + X[r[j ]] + K2, s[j ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int j=48; j<64; ++j){ uint32_t t = rol(a + f3(b,c,d) + X[r[j ]] + K3, s[j ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }
    for (int j=64; j<80; ++j){ uint32_t t = rol(a + f4(b,c,d) + X[r[j ]] + K4, s[j ]) + e; a=e; e=d; d=rol(c,10); c=b; b=t; }

    for (int j=0;  j<16; ++j){ uint32_t t = rol(A + f4(B,C,D) + X[rr[j ]] + KK0, ss[j ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int j=16; j<32; ++j){ uint32_t t = rol(A + f3(B,C,D) + X[rr[j ]] + KK1, ss[j ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int j=32; j<48; ++j){ uint32_t t = rol(A + f2(B,C,D) + X[rr[j ]] + KK2, ss[j ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int j=48; j<64; ++j){ uint32_t t = rol(A + f1(B,C,D) + X[rr[j ]] + KK3, ss[j ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }
    for (int j=64; j<80; ++j){ uint32_t t = rol(A + f0(B,C,D) + X[rr[j ]] + KK4, ss[j ]) + E; A=E; E=D; D=rol(C,10); C=B; B=t; }

    uint32_t T = st[1] + c + D;
    st[1]      = st[2] + d + E;
    st[2]      = st[3] + e + A;
    st[3]      = st[4] + a + B;
    st[4]      = st[0] + b + C;
    st[0]      = T;
}

void RIPEMD160_Init(RIPEMD160_CTX* ctx) {
    ctx->s[0]=0x67452301u; ctx->s[1]=0xEFCDAB89u; ctx->s[2]=0x98BADCFEu;
    ctx->s[3]=0x10325476u; ctx->s[4]=0xC3D2E1F0u;
    ctx->bytes = 0;
    std::memset(ctx->buf, 0, sizeof(ctx->buf));
}

void RIPEMD160_Update(RIPEMD160_CTX* ctx, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t n = static_cast<size_t>(ctx->bytes & 63u);
    ctx->bytes += (uint64_t)len;

    if (n) {
        size_t fill = 64u - n;
        if (len < fill) { std::memcpy(ctx->buf + n, p, len); return; }
        std::memcpy(ctx->buf + n, p, fill);
        compress(ctx->s, ctx->buf);
        p += fill; len -= fill; n = 0;
    }
    while (len >= 64u) {
        compress(ctx->s, p);
        p += 64; len -= 64;
    }
    if (len) std::memcpy(ctx->buf + n, p, len);
}

void RIPEMD160_Final(uint8_t out[20], RIPEMD160_CTX* ctx) {
    size_t n = static_cast<size_t>(ctx->bytes & 63u);
    ctx->buf[n++] = 0x80;
    if (n > 56u) {
        std::memset(ctx->buf + n, 0, 64u - n);
        compress(ctx->s, ctx->buf);
        n = 0;
    }
    std::memset(ctx->buf + n, 0, 56u - n);
    uint64_t bits = ctx->bytes * 8u;
    for (int i=0;i<8;++i) ctx->buf[56u + (size_t)i] = (uint8_t)(bits >> (8u*(unsigned)i));
    compress(ctx->s, ctx->buf);

    for (int i=0;i<5;++i) {
        uint32_t w = ctx->s[i];
        out[4*i+0] = (uint8_t)(w      );
        out[4*i+1] = (uint8_t)(w >>  8);
        out[4*i+2] = (uint8_t)(w >> 16);
        out[4*i+3] = (uint8_t)(w >> 24);
    }
    std::memset(ctx, 0, sizeof(*ctx));
}

} // namespace dinero
