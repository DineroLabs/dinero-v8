// SHA-256d kernel for DineroCoin BlockHeader v1 (128 bytes), Metal flavour.
//
// I/O contract matches IGpuBackend:
//   header (32 u32, little-endian on the wire)
//   target (8 u32, big-endian words, MSW at index 0)
//   nonce_start (u32)
//   batch_size (u32)
//   result_nonces (u32[capacity])
//   result_count (u32 total winners detected)
//   capacity (u32)

#include <metal_stdlib>
using namespace metal;

constant uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constant uint32_t H[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t ep0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t ep1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t sig0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t sig1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

inline uint32_t swap_endian(uint32_t x) {
    return ((x << 24) & 0xff000000u) |
           ((x <<  8) & 0x00ff0000u) |
           ((x >>  8) & 0x0000ff00u) |
           ((x >> 24) & 0x000000ffu);
}

void sha256_transform(thread uint32_t* state, thread const uint32_t* block) {
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = block[i];
    }
    for (int i = 16; i < 64; i++) {
        W[i] = sig1(W[i - 2]) + W[i - 7] + sig0(W[i - 15]) + W[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + W[i];
        uint32_t t2 = ep0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

inline bool hash_meets_target(thread const uint32_t* hash,
                              device const uint32_t* target) {
    for (int i = 0; i < 8; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return false;
}

kernel void sha256d_mine(
    device const uint32_t* header       [[buffer(0)]],
    device const uint32_t* target       [[buffer(1)]],
    constant uint32_t& nonce_start      [[buffer(2)]],
    constant uint32_t& batch_size       [[buffer(3)]],
    device uint32_t* result_nonces      [[buffer(4)]],
    device atomic_uint* result_count    [[buffer(5)]],
    constant uint32_t& result_capacity  [[buffer(6)]],
    uint gid                            [[thread_position_in_grid]])
{
    if (gid >= batch_size) return;

    uint32_t nonce = nonce_start + gid;

    uint32_t block1[16];
    for (int i = 0; i < 16; i++) {
        block1[i] = swap_endian(header[i]);
    }

    uint32_t block2[16];
    for (int i = 0; i < 12; i++) {
        block2[i] = swap_endian(header[16 + i]);
    }
    block2[12] = swap_endian(nonce);
    block2[13] = swap_endian(header[29]);
    block2[14] = swap_endian(header[30]);
    block2[15] = swap_endian(header[31]);

    uint32_t state1[8];
    for (int i = 0; i < 8; i++) state1[i] = H[i];
    sha256_transform(state1, block1);
    sha256_transform(state1, block2);

    uint32_t pad_block[16];
    pad_block[0] = 0x80000000u;
    for (int i = 1; i < 14; i++) pad_block[i] = 0u;
    pad_block[14] = 0u;
    pad_block[15] = 1024u;
    sha256_transform(state1, pad_block);

    uint32_t block3[16];
    for (int i = 0; i < 8; i++) block3[i] = state1[i];
    block3[8] = 0x80000000u;
    for (int i = 9; i < 15; i++) block3[i] = 0u;
    block3[15] = 256u;

    uint32_t state2[8];
    for (int i = 0; i < 8; i++) state2[i] = H[i];
    sha256_transform(state2, block3);

    if (hash_meets_target(state2, target)) {
        uint32_t slot = atomic_fetch_add_explicit(result_count, 1u, memory_order_relaxed);
        if (slot < result_capacity) {
            result_nonces[slot] = nonce;
        }
    }
}
