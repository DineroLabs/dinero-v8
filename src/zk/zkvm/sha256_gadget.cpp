// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/sha256_gadget.h"
#include <cstring>

namespace dinero {
namespace zk {
namespace zkvm {

// ============================================================================
// SHA256 constants
// ============================================================================

const std::array<uint32_t, 8> SHA256_H_INIT = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

const std::array<uint32_t, 64> SHA256_K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// Helper: get bit from witness as bool
static bool wit_bool(R1CS& cs, Variable v) {
    return !cs.get_value(v).is_zero();
}

// Helper: get 32-bit value from Word32 witness
static uint32_t wit_u32(R1CS& cs, const Word32& w) {
    uint32_t val = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (wit_bool(cs, w.bits[i])) val |= (1u << i);
    }
    return val;
}

// ============================================================================
// Word32 operations
// ============================================================================

Word32 word32_alloc(R1CS& cs, uint32_t value, const std::string& label) {
    Word32 w;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t bit = (value >> i) & 1;
        w.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>(bit)));
        gadgets::enforce_boolean(cs, w.bits[i], label + std::to_string(i));
    }
    return w;
}

// Shared constant bits for building Word32 constants without per-bit constraints.
// Only 2 constraints total (one for zero, one for one), reused everywhere.
// No statics — use R1CS::const_zero() / const_one() for structural constants.

// Structural constant: bits reference this R1CS instance's shared zero/one variables.
// Cost: 0 new constraints per call after the per-instance constants are initialized.
static Word32 word32_const(R1CS& cs, uint32_t value, const std::string& /*label*/) {
    Word32 w;
    for (unsigned i = 0; i < 32; ++i) {
        w.bits[i] = ((value >> i) & 1) ? cs.const_one() : cs.const_zero();
    }
    return w;
}

Variable word32_pack(R1CS& cs, const Word32& w, const std::string& label) {
    // Compute packed value from witness
    uint32_t val = wit_u32(cs, w);
    Variable packed = cs.alloc(Scalar(static_cast<uint64_t>(val)));

    // Build LC: sum(bits[i] * 2^i)
    LinearCombination lc;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        lc = lc + LinearCombination(Scalar(pow2), w.bits[i]);
        pow2 <<= 1;
    }

    // Constrain: lc * 1 = packed
    cs.constrain(lc, LinearCombination(VAR_ONE), LinearCombination(packed), label);
    return packed;
}

Word32 word32_from_var(R1CS& cs, Variable var, const std::string& label) {
    // Get raw bytes from scalar, extract low 32 bits
    const uint8_t* raw = cs.get_value(var).data();
    // Scalar is big-endian 32 bytes, low 32 bits are in bytes 28-31
    uint32_t u32_val = (static_cast<uint32_t>(raw[28]) << 24) |
                       (static_cast<uint32_t>(raw[29]) << 16) |
                       (static_cast<uint32_t>(raw[30]) << 8) |
                       static_cast<uint32_t>(raw[31]);

    Word32 w = word32_alloc(cs, u32_val, label);
    Variable packed = word32_pack(cs, w, label + "_pk");
    gadgets::assert_equal(cs, packed, var, label + "_eq");
    return w;
}

Word32 word32_xor(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label) {
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        z.bits[i] = gadgets::xor_bits(cs, a.bits[i], b.bits[i],
                                       label + std::to_string(i));
    }
    return z;
}

Word32 word32_and(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label) {
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        z.bits[i] = gadgets::and_bits(cs, a.bits[i], b.bits[i],
                                       label + std::to_string(i));
    }
    return z;
}

Word32 word32_not(R1CS& cs, const Word32& a, const std::string& label) {
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        z.bits[i] = gadgets::not_bit(cs, a.bits[i], label + std::to_string(i));
    }
    return z;
}

Word32 word32_rotr(const Word32& a, unsigned n) {
    n %= 32;
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        z.bits[i] = a.bits[(i + n) % 32];
    }
    return z;
}

Word32 word32_shr(R1CS& cs, const Word32& a, unsigned n,
                   const std::string& /*label*/) {
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        if (i + n < 32) {
            z.bits[i] = a.bits[i + n];
        } else {
            z.bits[i] = cs.const_zero();
        }
    }
    return z;
}

// ============================================================================
// word32_add: packed-field adder (optimized)
// ============================================================================
//
// Instead of a 96-constraint carry chain, we:
//   1. Pack both inputs to field elements (2 constraints)
//   2. Add in the field (1 constraint: sum_field = pack_a + pack_b)
//   3. Decompose sum_field to 33 bits (33 boolean + 1 pack equality)
//   4. Output = low 32 bits (discard bit 32 = overflow carry)
//
// Total: ~37 constraints vs ~96 for carry chain (2.6x improvement).
// ============================================================================

Word32 word32_add(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label) {
    // Pack inputs to field elements
    Variable pa = word32_pack(cs, a, label + "pa");
    Variable pb = word32_pack(cs, b, label + "pb");

    // Add in the field: sum_field = pa + pb
    Variable sum_field = gadgets::add(cs, pa, pb, label + "sf");

    // Witness: compute the 33-bit result
    uint32_t a_val = wit_u32(cs, a);
    uint32_t b_val = wit_u32(cs, b);
    uint64_t full = static_cast<uint64_t>(a_val) + static_cast<uint64_t>(b_val);

    // Decompose to 33 bits (32 sum bits + 1 carry bit)
    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t bit = (full >> i) & 1;
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>(bit)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }

    // Carry bit (bit 32)
    uint32_t carry_bit = (full >> 32) & 1;
    Variable carry = cs.alloc(Scalar(static_cast<uint64_t>(carry_bit)));
    gadgets::enforce_boolean(cs, carry, label + "cy");

    // Constrain: sum_field = pack(result) + carry * 2^32
    // i.e., pack(result) + carry * 2^32 = sum_field
    LinearCombination packed_lc;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        packed_lc = packed_lc + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    // pow2 is now 2^32
    packed_lc = packed_lc + LinearCombination(Scalar(pow2), carry);

    // packed_lc * 1 = sum_field
    cs.constrain(packed_lc, LinearCombination(VAR_ONE),
                 LinearCombination(sum_field), label + "eq");

    return result;
}

// ============================================================================
// word32_add3: packed 3-input adder
// ============================================================================
//
// Three 32-bit values sum to at most 3*(2^32-1) < 2^34.
// So we decompose to 34 bits instead of doing two sequential adds.
// Saves ~37 constraints vs two word32_add calls.
// ============================================================================

Word32 word32_add3(R1CS& cs, const Word32& a, const Word32& b,
                    const Word32& c, const std::string& label) {
    Variable pa = word32_pack(cs, a, label + "pa");
    Variable pb = word32_pack(cs, b, label + "pb");
    Variable pc = word32_pack(cs, c, label + "pc");

    // sum = pa + pb + pc
    Variable ab = gadgets::add(cs, pa, pb, label + "ab");
    Variable sum_field = gadgets::add(cs, ab, pc, label + "sf");

    // Witness
    uint32_t av = wit_u32(cs, a);
    uint32_t bv = wit_u32(cs, b);
    uint32_t cv = wit_u32(cs, c);
    uint64_t full = static_cast<uint64_t>(av) + static_cast<uint64_t>(bv)
                  + static_cast<uint64_t>(cv);

    // Decompose to 34 bits (32 result + 2 overflow)
    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t bit = (full >> i) & 1;
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>(bit)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }

    // 2 overflow bits (bits 32 and 33)
    uint32_t ov0 = (full >> 32) & 1;
    uint32_t ov1 = (full >> 33) & 1;
    Variable over0 = cs.alloc(Scalar(static_cast<uint64_t>(ov0)));
    Variable over1 = cs.alloc(Scalar(static_cast<uint64_t>(ov1)));
    gadgets::enforce_boolean(cs, over0, label + "o0");
    gadgets::enforce_boolean(cs, over1, label + "o1");

    // Constrain: pack(result) + over0*2^32 + over1*2^33 = sum_field
    LinearCombination packed_lc;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        packed_lc = packed_lc + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    packed_lc = packed_lc + LinearCombination(Scalar(pow2), over0);       // 2^32
    packed_lc = packed_lc + LinearCombination(Scalar(pow2 << 1), over1);  // 2^33

    cs.constrain(packed_lc, LinearCombination(VAR_ONE),
                 LinearCombination(sum_field), label + "eq");

    return result;
}

// ============================================================================
// Multi-input fused adders (avoid intermediate Variable allocations)
// ============================================================================

// word32_add4: 4 x Word32 → low 32 bits of sum.
// Uses enforce_equal(decomp_lc, pa+pb+pc+pd) to avoid 2 intermediate Variables.
// Cost: 4 packs + 34 bools + 1 eq = 39 constraints
// (vs word32_add3 + word32_add = 40 + 37 = 77)
static Word32 word32_add4(R1CS& cs, const Word32& a, const Word32& b,
                           const Word32& c, const Word32& d,
                           const std::string& label) {
    Variable pa = word32_pack(cs, a, label + "pa");
    Variable pb = word32_pack(cs, b, label + "pb");
    Variable pc = word32_pack(cs, c, label + "pc");
    Variable pd = word32_pack(cs, d, label + "pd");

    uint64_t full = static_cast<uint64_t>(wit_u32(cs, a))
                  + static_cast<uint64_t>(wit_u32(cs, b))
                  + static_cast<uint64_t>(wit_u32(cs, c))
                  + static_cast<uint64_t>(wit_u32(cs, d));

    // Decompose to 34 bits: 32 result + 2 overflow
    // max = 4*(2^32-1) = 2^34-4, fits in 34 bits
    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>((full >> i) & 1)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }
    Variable ov0 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 32) & 1)));
    Variable ov1 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 33) & 1)));
    gadgets::enforce_boolean(cs, ov0, label + "o0");
    gadgets::enforce_boolean(cs, ov1, label + "o1");

    // decomp_lc = pa + pb + pc + pd
    LinearCombination decomp;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        decomp = decomp + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    decomp = decomp + LinearCombination(Scalar(pow2), ov0);        // 2^32
    decomp = decomp + LinearCombination(Scalar(pow2 << 1), ov1);   // 2^33

    LinearCombination rhs = LinearCombination(pa) + LinearCombination(pb)
                          + LinearCombination(pc) + LinearCombination(pd);
    cs.enforce_equal(decomp, rhs, label + "eq");
    return result;
}

// word32_add3_const: 3 x Word32 + uint32 constant → low 32 bits.
// Cost: 3 packs + 34 bools + 1 eq = 38 constraints
// (vs word32_add3 + word32_add_const = 40 + 35 = 75; saves 37)
static Word32 word32_add3_const(R1CS& cs, const Word32& a, const Word32& b,
                                 const Word32& c, uint32_t k,
                                 const std::string& label) {
    Variable pa = word32_pack(cs, a, label + "pa");
    Variable pb = word32_pack(cs, b, label + "pb");
    Variable pc = word32_pack(cs, c, label + "pc");

    uint64_t full = static_cast<uint64_t>(wit_u32(cs, a))
                  + static_cast<uint64_t>(wit_u32(cs, b))
                  + static_cast<uint64_t>(wit_u32(cs, c))
                  + static_cast<uint64_t>(k);

    // max = 3*(2^32-1)+k < 4*2^32 < 2^34, 2 overflow bits
    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>((full >> i) & 1)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }
    Variable ov0 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 32) & 1)));
    Variable ov1 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 33) & 1)));
    gadgets::enforce_boolean(cs, ov0, label + "o0");
    gadgets::enforce_boolean(cs, ov1, label + "o1");

    LinearCombination decomp;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        decomp = decomp + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    decomp = decomp + LinearCombination(Scalar(pow2), ov0);
    decomp = decomp + LinearCombination(Scalar(pow2 << 1), ov1);

    LinearCombination rhs = LinearCombination(pa) + LinearCombination(pb)
                          + LinearCombination(pc)
                          + LinearCombination(Scalar(static_cast<uint64_t>(k)), VAR_ONE);
    cs.enforce_equal(decomp, rhs, label + "eq");
    return result;
}

// word32_add4_const: 4 x Word32 + uint32 constant → low 32 bits.
// Cost: 4 packs + 35 bools + 1 eq = 40 constraints
// (vs word32_add3(a,b,c) + word32_add3(Kw,d,t) = 40+40=80; saves 40)
static Word32 word32_add4_const(R1CS& cs, const Word32& a, const Word32& b,
                                 const Word32& c, const Word32& d, uint32_t k,
                                 const std::string& label) {
    Variable pa = word32_pack(cs, a, label + "pa");
    Variable pb = word32_pack(cs, b, label + "pb");
    Variable pc = word32_pack(cs, c, label + "pc");
    Variable pd = word32_pack(cs, d, label + "pd");

    uint64_t full = static_cast<uint64_t>(wit_u32(cs, a))
                  + static_cast<uint64_t>(wit_u32(cs, b))
                  + static_cast<uint64_t>(wit_u32(cs, c))
                  + static_cast<uint64_t>(wit_u32(cs, d))
                  + static_cast<uint64_t>(k);

    // max = 4*(2^32-1)+k < 5*2^32 < 2^35, 3 overflow bits
    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>((full >> i) & 1)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }
    Variable ov0 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 32) & 1)));
    Variable ov1 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 33) & 1)));
    Variable ov2 = cs.alloc(Scalar(static_cast<uint64_t>((full >> 34) & 1)));
    gadgets::enforce_boolean(cs, ov0, label + "o0");
    gadgets::enforce_boolean(cs, ov1, label + "o1");
    gadgets::enforce_boolean(cs, ov2, label + "o2");

    LinearCombination decomp;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        decomp = decomp + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    decomp = decomp + LinearCombination(Scalar(pow2), ov0);           // 2^32
    decomp = decomp + LinearCombination(Scalar(pow2 << 1), ov1);      // 2^33
    decomp = decomp + LinearCombination(Scalar(pow2 << 2), ov2);      // 2^34

    LinearCombination rhs = LinearCombination(pa) + LinearCombination(pb)
                          + LinearCombination(pc) + LinearCombination(pd)
                          + LinearCombination(Scalar(static_cast<uint64_t>(k)), VAR_ONE);
    cs.enforce_equal(decomp, rhs, label + "eq");
    return result;
}

// ============================================================================
// SHA256 core functions
// ============================================================================

Word32 sha256_ch(R1CS& cs, const Word32& e, const Word32& f, const Word32& g) {
    // Ch(e,f,g) = e*(f-g) + g per bit.
    // Single constraint per bit: A=(e_i) * B=(f_i-g_i) = C=(z_i-g_i)
    // Reduced from 3 constraints/bit (sub+mul+add) to 1 constraint/bit.
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        bool e_v = !cs.get_value(e.bits[i]).is_zero();
        bool f_v = !cs.get_value(f.bits[i]).is_zero();
        bool g_v = !cs.get_value(g.bits[i]).is_zero();
        uint64_t z_v = e_v ? (f_v ? 1 : 0) : (g_v ? 1 : 0);
        z.bits[i] = cs.alloc(Scalar(z_v));
        // e_i * (f_i - g_i) = z_i - g_i
        cs.constrain(
            LinearCombination(e.bits[i]),
            LinearCombination(f.bits[i]) - LinearCombination(g.bits[i]),
            LinearCombination(z.bits[i]) - LinearCombination(g.bits[i]),
            "ch_" + std::to_string(i)
        );
    }
    return z;
}

Word32 sha256_maj(R1CS& cs, const Word32& a, const Word32& b, const Word32& c) {
    // Majority: m = (a&b)|(a&c)|(b&c).
    // Reduced from 3 constraints/bit to 2 constraints/bit by eliminating the
    // explicit `r` allocation. r = a+b+c-2m is expressed as a linear combination
    // and its boolean constraint is merged into a single product check: r*(1-r)=0.
    //
    // Constraint 1: bool(m)         — m_i * (1 - m_i) = 0
    // Constraint 2: bool(r as LC)   — (a+b+c-2m) * (1 - a - b - c + 2m) = 0
    //   This simultaneously proves r ∈ {0,1} and a+b+c = 2m+r (the decomposition).
    Word32 z;
    for (unsigned i = 0; i < 32; ++i) {
        bool ab = wit_bool(cs, a.bits[i]);
        bool bb = wit_bool(cs, b.bits[i]);
        bool cb = wit_bool(cs, c.bits[i]);
        uint32_t maj_val = (ab && bb) || (ab && cb) || (bb && cb) ? 1 : 0;

        z.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>(maj_val)));
        gadgets::enforce_boolean(cs, z.bits[i], "mj_m" + std::to_string(i));

        // r = a + b + c - 2*m  (as LC, no alloc needed)
        // Enforce r*(1-r) = 0:
        //   A = (a_i + b_i + c_i - 2*m_i)
        //   B = (1 - a_i - b_i - c_i + 2*m_i)  [= 1 - A]
        //   C = 0
        LinearCombination r_lc;
        r_lc = r_lc + LinearCombination(a.bits[i]);
        r_lc = r_lc + LinearCombination(b.bits[i]);
        r_lc = r_lc + LinearCombination(c.bits[i]);
        r_lc = r_lc - LinearCombination(Scalar(uint64_t(2)), z.bits[i]);

        LinearCombination one_minus_r_lc;
        one_minus_r_lc = LinearCombination::constant(Scalar::one());
        one_minus_r_lc = one_minus_r_lc - LinearCombination(a.bits[i]);
        one_minus_r_lc = one_minus_r_lc - LinearCombination(b.bits[i]);
        one_minus_r_lc = one_minus_r_lc - LinearCombination(c.bits[i]);
        one_minus_r_lc = one_minus_r_lc + LinearCombination(Scalar(uint64_t(2)), z.bits[i]);

        cs.constrain(r_lc, one_minus_r_lc,
                     LinearCombination::constant(Scalar::zero()),
                     "mj_r" + std::to_string(i));
    }
    return z;
}

Word32 sha256_big_sigma0(R1CS& cs, const Word32& a) {
    Word32 r2 = word32_rotr(a, 2);
    Word32 r13 = word32_rotr(a, 13);
    Word32 r22 = word32_rotr(a, 22);
    Word32 t = word32_xor(cs, r2, r13, "S0a");
    return word32_xor(cs, t, r22, "S0b");
}

Word32 sha256_big_sigma1(R1CS& cs, const Word32& e) {
    Word32 r6 = word32_rotr(e, 6);
    Word32 r11 = word32_rotr(e, 11);
    Word32 r25 = word32_rotr(e, 25);
    Word32 t = word32_xor(cs, r6, r11, "S1a");
    return word32_xor(cs, t, r25, "S1b");
}

Word32 sha256_small_sigma0(R1CS& cs, const Word32& x) {
    Word32 r7 = word32_rotr(x, 7);
    Word32 r18 = word32_rotr(x, 18);
    Word32 s3 = word32_shr(cs, x, 3, "s0s");
    Word32 t = word32_xor(cs, r7, r18, "s0a");
    return word32_xor(cs, t, s3, "s0b");
}

Word32 sha256_small_sigma1(R1CS& cs, const Word32& x) {
    Word32 r17 = word32_rotr(x, 17);
    Word32 r19 = word32_rotr(x, 19);
    Word32 s10 = word32_shr(cs, x, 10, "s1s");
    Word32 t = word32_xor(cs, r17, r19, "s1a");
    return word32_xor(cs, t, s10, "s1b");
}

// ============================================================================
// SHA256 compression
// ============================================================================

SHA256State sha256_init(R1CS& cs) {
    SHA256State state;
    for (unsigned i = 0; i < 8; ++i) {
        state.h[i] = word32_const(cs, SHA256_H_INIT[i], "H" + std::to_string(i));
    }
    return state;
}

SHA256State sha256_state_from_midstate(R1CS& cs,
                                        const std::array<uint32_t, 8>& midstate) {
    SHA256State state;
    for (unsigned i = 0; i < 8; ++i) {
        state.h[i] = word32_const(cs, midstate[i], "MS" + std::to_string(i));
    }
    return state;
}

std::array<Word32, 16> sha256_pack_block(R1CS& cs,
                                          const std::vector<Word32>& bytes) {
    std::array<Word32, 16> block;
    Variable zero = cs.const_zero();
    for (unsigned w = 0; w < 16; ++w) {
        Word32 word;
        for (unsigned b = 0; b < 4; ++b) {
            const size_t idx = w * 4 + b;
            const unsigned shift = (3 - b) * 8; // big-endian within word
            for (unsigned bit = 0; bit < 8; ++bit) {
                word.bits[shift + bit] =
                    (idx < bytes.size()) ? bytes[idx].bits[bit] : zero;
            }
        }
        block[w] = word;
    }
    return block;
}

SHA256State sha256_compress(R1CS& cs, const SHA256State& state,
                             const std::array<Word32, 16>& block) {
    // Message schedule
    std::array<Word32, 64> W;
    for (unsigned i = 0; i < 16; ++i) W[i] = block[i];

    for (unsigned i = 16; i < 64; ++i) {
        std::string p = "W" + std::to_string(i);
        Word32 s0 = sha256_small_sigma0(cs, W[i-15]);
        Word32 s1 = sha256_small_sigma1(cs, W[i-2]);
        // W[i] = W[i-16] + s0 + W[i-7] + s1 — fused 4-input add (~39 vs 77 constraints)
        W[i] = word32_add4(cs, W[i-16], s0, W[i-7], s1, p);
    }

    // Working variables
    Word32 a=state.h[0], b=state.h[1], c=state.h[2], d=state.h[3];
    Word32 e=state.h[4], f=state.h[5], g=state.h[6], hh=state.h[7];

    for (unsigned i = 0; i < 64; ++i) {
        std::string p = "R" + std::to_string(i);
        Word32 S1 = sha256_big_sigma1(cs, e);
        Word32 ch = sha256_ch(cs, e, f, g);

        // T1 = hh + S1 + Ch + K[i] + W[i] — fused 4-input+const add (~40 vs 80 constraints)
        Word32 T1 = word32_add4_const(cs, hh, S1, ch, W[i], SHA256_K[i], p+"T1");

        Word32 S0 = sha256_big_sigma0(cs, a);
        Word32 maj = sha256_maj(cs, a, b, c);

        hh = g; g = f; f = e;
        e = word32_add(cs, d, T1, p+"e");
        d = c; c = b; b = a;
        // a = T1 + S0 + maj — fused 3-input add (~40 vs 74 constraints)
        a = word32_add3(cs, T1, S0, maj, p+"a");
    }

    SHA256State out;
    out.h[0] = word32_add(cs, state.h[0], a, "F0");
    out.h[1] = word32_add(cs, state.h[1], b, "F1");
    out.h[2] = word32_add(cs, state.h[2], c, "F2");
    out.h[3] = word32_add(cs, state.h[3], d, "F3");
    out.h[4] = word32_add(cs, state.h[4], e, "F4");
    out.h[5] = word32_add(cs, state.h[5], f, "F5");
    out.h[6] = word32_add(cs, state.h[6], g, "F6");
    out.h[7] = word32_add(cs, state.h[7], hh, "F7");
    return out;
}

// ============================================================================
// Helper: byte → Word32 (8 active bits, 24 zero)
// ============================================================================

// Padding bytes as shared constants — 0 new constraints.
static Word32 byte_to_w32(R1CS& cs, uint8_t value, const std::string& /*label*/) {
    Word32 w;
    for (unsigned i = 0; i < 32; ++i) {
        if (i < 8) {
            w.bits[i] = ((value >> i) & 1) ? cs.const_one() : cs.const_zero();
        } else {
            w.bits[i] = cs.const_zero();
        }
    }
    return w;
}

// ============================================================================
// Helper: pack bytes into 16 × Word32 block (big-endian SHA256 format)
// ============================================================================

static std::array<Word32, 16> pack_block(
    R1CS& cs, const std::vector<Word32>& bytes, size_t offset,
    const std::string& label
) {
    std::array<Word32, 16> block;
    Variable zv = cs.const_zero();

    for (unsigned w = 0; w < 16; ++w) {
        Word32 word;
        for (unsigned b = 0; b < 4; ++b) {
            size_t idx = offset + w * 4 + b;
            unsigned shift = (3 - b) * 8;  // Big-endian
            for (unsigned bi = 0; bi < 8; ++bi) {
                if (idx < bytes.size()) {
                    word.bits[shift + bi] = bytes[idx].bits[bi];
                } else {
                    word.bits[shift + bi] = zv;
                }
            }
        }
        block[w] = word;
    }
    return block;
}

// ============================================================================
// SHA256 compression with fully-constant message schedule
// ============================================================================

// Integer-only message schedule expansion (outside circuit).
static uint32_t u32_sigma0(uint32_t x) {
    return ((x >> 7)  | (x << 25)) ^
           ((x >> 18) | (x << 14)) ^
           (x >> 3);
}
static uint32_t u32_sigma1(uint32_t x) {
    return ((x >> 17) | (x << 15)) ^
           ((x >> 19) | (x << 13)) ^
           (x >> 10);
}
static std::array<uint32_t, 64> expand_const_schedule(
    const std::array<uint32_t, 16>& block
) {
    std::array<uint32_t, 64> W;
    for (unsigned i = 0; i <  16; ++i) W[i] = block[i];
    for (unsigned i = 16; i < 64; ++i)
        W[i] = u32_sigma1(W[i-2]) + W[i-7] + u32_sigma0(W[i-15]) + W[i-16];
    return W;
}

// Add a known 32-bit constant to a Word32.
// Avoids allocating and packing a second Word32 for the constant.
// Cost: 1 (pack a) + 32 (result bool) + 1 (carry bool) + 1 (enforce_equal) = 35 constraints.
static Word32 word32_add_const(R1CS& cs, const Word32& a, uint32_t c,
                                const std::string& label) {
    Variable pa = word32_pack(cs, a, label + "pa");
    uint32_t a_val = wit_u32(cs, a);
    uint64_t full = static_cast<uint64_t>(a_val) + static_cast<uint64_t>(c);

    Word32 result;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t bit = (full >> i) & 1;
        result.bits[i] = cs.alloc(Scalar(static_cast<uint64_t>(bit)));
        gadgets::enforce_boolean(cs, result.bits[i], label + "b" + std::to_string(i));
    }
    uint32_t carry_bit = (full >> 32) & 1;
    Variable carry = cs.alloc(Scalar(static_cast<uint64_t>(carry_bit)));
    gadgets::enforce_boolean(cs, carry, label + "cy");

    // pack(result) + carry*2^32 = pa + c*1
    LinearCombination lhs;
    uint64_t pow2 = 1;
    for (unsigned i = 0; i < 32; ++i) {
        lhs = lhs + LinearCombination(Scalar(pow2), result.bits[i]);
        pow2 <<= 1;
    }
    lhs = lhs + LinearCombination(Scalar(pow2), carry);
    LinearCombination rhs = LinearCombination(pa)
                          + LinearCombination(Scalar(static_cast<uint64_t>(c)), VAR_ONE);
    cs.enforce_equal(lhs, rhs, label + "eq");
    return result;
}

SHA256State sha256_compress_const_block(R1CS& cs, const SHA256State& state,
                                         const std::array<uint32_t, 16>& block_words) {
    // Precompute full 64-word schedule outside the circuit — no constraints.
    std::array<uint32_t, 64> W = expand_const_schedule(block_words);

    Word32 a=state.h[0], b=state.h[1], c=state.h[2], d=state.h[3];
    Word32 e=state.h[4], f=state.h[5], g=state.h[6], hh=state.h[7];

    for (unsigned i = 0; i < 64; ++i) {
        std::string p = "CW" + std::to_string(i);
        Word32 S1  = sha256_big_sigma1(cs, e);       // 64 constraints
        Word32 ch  = sha256_ch(cs, e, f, g);          // 32 constraints
        // T1 = hh+S1+Ch+(K[i]+W[i]) — fused 3-input+const add (~38 vs 75 constraints)
        uint32_t kw = SHA256_K[i] + W[i];  // wraps mod 2^32
        Word32 T1  = word32_add3_const(cs, hh, S1, ch, kw, p+"T1");

        Word32 S0  = sha256_big_sigma0(cs, a);        // 64 constraints
        Word32 maj = sha256_maj(cs, a, b, c);          // 64 constraints

        hh = g; g = f; f = e;
        e = word32_add(cs, d, T1, p+"e");              // ~37 constraints
        d = c; c = b; b = a;
        // a = T1 + S0 + maj — fused 3-input add (~40 vs 74 constraints)
        a = word32_add3(cs, T1, S0, maj, p+"a");
    }

    SHA256State out;
    out.h[0] = word32_add(cs, state.h[0], a,  "CF0");
    out.h[1] = word32_add(cs, state.h[1], b,  "CF1");
    out.h[2] = word32_add(cs, state.h[2], c,  "CF2");
    out.h[3] = word32_add(cs, state.h[3], d,  "CF3");
    out.h[4] = word32_add(cs, state.h[4], e,  "CF4");
    out.h[5] = word32_add(cs, state.h[5], f,  "CF5");
    out.h[6] = word32_add(cs, state.h[6], g,  "CF6");
    out.h[7] = word32_add(cs, state.h[7], hh, "CF7");
    return out;
}

// ============================================================================
// Full SHA256 with NIST padding
// ============================================================================

std::array<Word32, 8> sha256_full(R1CS& cs,
                                    const std::vector<Word32>& message_bytes) {
    size_t msg_len = message_bytes.size();

    // Padded length: msg + 1 (0x80) + zeros + 8 (length), rounded to 64
    size_t padded_len = msg_len + 1 + 8;
    while (padded_len % 64 != 0) padded_len++;

    // Build padded byte sequence
    std::vector<Word32> padded;
    padded.reserve(padded_len);

    for (size_t i = 0; i < msg_len; ++i)
        padded.push_back(message_bytes[i]);

    padded.push_back(byte_to_w32(cs, 0x80, "p80"));

    while (padded.size() < padded_len - 8)
        padded.push_back(byte_to_w32(cs, 0x00, "p0_" + std::to_string(padded.size())));

    // 64-bit big-endian bit length
    uint64_t bit_len = msg_len * 8;
    for (int i = 7; i >= 0; --i) {
        uint8_t byte = (bit_len >> (i * 8)) & 0xFF;
        padded.push_back(byte_to_w32(cs, byte, "pL" + std::to_string(7 - i)));
    }

    // Compress blocks
    SHA256State state = sha256_init(cs);
    size_t num_blocks = padded_len / 64;

    for (size_t b = 0; b < num_blocks; ++b) {
        auto block = pack_block(cs, padded, b * 64, "B" + std::to_string(b));
        state = sha256_compress(cs, state, block);
    }

    return state.h;
}

// ============================================================================
// Double SHA256
// ============================================================================

std::array<Word32, 8> double_sha256(R1CS& cs,
                                      const std::vector<Word32>& message_bytes) {
    auto inner = sha256_full(cs, message_bytes);

    // Inner hash → 32 bytes (big-endian word order)
    std::vector<Word32> inner_bytes;
    inner_bytes.reserve(32);
    Variable zv = cs.const_zero();

    for (unsigned w = 0; w < 8; ++w) {
        for (int b = 3; b >= 0; --b) {
            Word32 byte_w;
            for (unsigned i = 0; i < 32; ++i) {
                if (i < 8) {
                    byte_w.bits[i] = inner[w].bits[b * 8 + i];
                } else {
                    byte_w.bits[i] = zv;
                }
            }
            inner_bytes.push_back(byte_w);
        }
    }

    return sha256_full(cs, inner_bytes);
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
