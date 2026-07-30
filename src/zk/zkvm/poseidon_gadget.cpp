// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/poseidon_gadget.h"
#include "zk/zkvm/gadgets.h"
#include "crypto/evp_secp256k1.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstring>
#include <cstdio>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Poseidon parameters
// ---------------------------------------------------------------------------

static constexpr size_t kStateSize  = 3;    // t = 3
static constexpr size_t kFullRounds = 8;    // Rf = 8 (4 before + 4 after partial)
static constexpr size_t kPartRounds = 56;   // Rp = 56
static constexpr size_t kTotalRounds = kFullRounds + kPartRounds; // 64

// MDS matrix M (circulant, MDS for all p > 3):
//   [[2, 1, 1],
//    [1, 2, 1],
//    [1, 1, 2]]
// Row i: M[i][j] = (i == j) ? 2 : 1

// ---------------------------------------------------------------------------
// Round constants (generated deterministically from SHA256)
//
// round_const[r][e] = rehash_to_valid_scalar(
//     SHA256("PoseidonC_secp256k1" || BE32(r) || BE32(e)))
// One constant per (round, element) pair.
// ---------------------------------------------------------------------------

struct PoseidonConstants {
    Scalar rc[kTotalRounds][kStateSize];
};

static Scalar derive_round_constant(uint32_t round, uint32_t element) {
    static const char kTag[] = "PoseidonC_secp256k1";
    auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();

    uint8_t hash[32];
    SHA256_CTX h;
    SHA256_Init(&h);
    SHA256_Update(&h, kTag, sizeof(kTag) - 1);
    // Big-endian round index
    uint8_t r_be[4] = {
        static_cast<uint8_t>(round >> 24),
        static_cast<uint8_t>(round >> 16),
        static_cast<uint8_t>(round >> 8),
        static_cast<uint8_t>(round)
    };
    uint8_t e_be[4] = {
        static_cast<uint8_t>(element >> 24),
        static_cast<uint8_t>(element >> 16),
        static_cast<uint8_t>(element >> 8),
        static_cast<uint8_t>(element)
    };
    SHA256_Update(&h, r_be, 4);
    SHA256_Update(&h, e_be, 4);
    SHA256_Final(hash, &h);

    return Scalar::from_hash(hash, ctx);
}

static const PoseidonConstants& get_constants() {
    static PoseidonConstants consts = []() {
        PoseidonConstants c;
        for (uint32_t r = 0; r < kTotalRounds; ++r) {
            for (uint32_t e = 0; e < kStateSize; ++e) {
                c.rc[r][e] = derive_round_constant(r, e);
            }
        }
        return c;
    }();
    return consts;
}

// ---------------------------------------------------------------------------
// Native helpers
// ---------------------------------------------------------------------------

// Compute x^5 in the scalar field.
static Scalar sbox5_native(const Scalar& x) {
    const Scalar x2 = x * x;
    const Scalar x4 = x2 * x2;
    return x4 * x;
}

// Apply MDS matrix M = [[2,1,1],[1,2,1],[1,1,2]] to state in-place.
static void mds_native(Scalar state[kStateSize]) {
    const Scalar two(uint64_t(2));
    const Scalar s0 = state[0] * two + state[1] + state[2];
    const Scalar s1 = state[0] + state[1] * two + state[2];
    const Scalar s2 = state[0] + state[1] + state[2] * two;
    state[0] = s0;
    state[1] = s1;
    state[2] = s2;
}

// Add round constants for round `r` to state.
static void add_round_constants_native(Scalar state[kStateSize],
                                        const PoseidonConstants& c,
                                        size_t r) {
    for (size_t e = 0; e < kStateSize; ++e) {
        state[e] = state[e] + c.rc[r][e];
    }
}

// ---------------------------------------------------------------------------
// Native evaluation
// ---------------------------------------------------------------------------

Scalar poseidon2_native(const Scalar& a, const Scalar& b) {
    const PoseidonConstants& c = get_constants();

    // Initialize state: [capacity=0, rate0=a, rate1=b]
    Scalar state[kStateSize] = {Scalar::zero(), a, b};

    // First Rf/2 = 4 full rounds
    for (size_t r = 0; r < kFullRounds / 2; ++r) {
        add_round_constants_native(state, c, r);
        for (size_t e = 0; e < kStateSize; ++e) {
            state[e] = sbox5_native(state[e]);
        }
        mds_native(state);
    }

    // Rp = 56 partial rounds (only element [0] goes through S-Box)
    const size_t part_start = kFullRounds / 2;
    for (size_t r = 0; r < kPartRounds; ++r) {
        add_round_constants_native(state, c, part_start + r);
        state[0] = sbox5_native(state[0]);
        // Elements [1] and [2] skip S-Box in partial rounds
        mds_native(state);
    }

    // Last Rf/2 = 4 full rounds
    const size_t full2_start = part_start + kPartRounds;
    for (size_t r = 0; r < kFullRounds / 2; ++r) {
        add_round_constants_native(state, c, full2_start + r);
        for (size_t e = 0; e < kStateSize; ++e) {
            state[e] = sbox5_native(state[e]);
        }
        mds_native(state);
    }

    // Output: first rate element
    return state[1];
}

std::array<uint8_t, 32> poseidon2_bytes(const std::array<uint8_t, 32>& a,
                                         const std::array<uint8_t, 32>& b) {
    const Scalar sa(a.data());
    const Scalar sb(b.data());
    const Scalar out = poseidon2_native(sa, sb);
    std::array<uint8_t, 32> result;
    std::memcpy(result.data(), out.data(), 32);
    return result;
}

// ---------------------------------------------------------------------------
// R1CS gadget helpers
// ---------------------------------------------------------------------------

// Apply x^5 S-Box in-circuit.
// Cost: 3 R1CS multiplication constraints.
// Returns a new Variable representing x^5.
static Variable sbox5_gadget(R1CS& cs, Variable x, const std::string& label) {
    Variable x2 = gadgets::mul(cs, x, x, label + "_x2");
    Variable x4 = gadgets::mul(cs, x2, x2, label + "_x4");
    return gadgets::mul(cs, x4, x, label + "_x5");
}

// Apply MDS matrix M = [[2,1,1],[1,2,1],[1,1,2]] in-circuit.
// Cost: 0 multiplication constraints (pure linear operations).
// Updates the three state variables in place.
static void mds_gadget(R1CS& cs,
                        Variable& s0, Variable& s1, Variable& s2,
                        const Scalar& val0, const Scalar& val1, const Scalar& val2,
                        const std::string& label) {
    const Scalar two(uint64_t(2));

    // new_s0 = 2*s0 + s1 + s2
    const Scalar new_val0 = val0 * two + val1 + val2;
    // new_s1 = s0 + 2*s1 + s2
    const Scalar new_val1 = val0 + val1 * two + val2;
    // new_s2 = s0 + s1 + 2*s2
    const Scalar new_val2 = val0 + val1 + val2 * two;

    Variable new_s0 = cs.alloc(new_val0);
    Variable new_s1 = cs.alloc(new_val1);
    Variable new_s2 = cs.alloc(new_val2);

    // Constrain: new_s0 = 2*s0 + s1 + s2
    // Encoded as: new_s0 - 2*s0 - s1 - s2 = 0
    // R1CS form: (new_s0 - 2*s0 - s1 - s2) * 1 = 0
    LinearCombination lc0;
    lc0 = lc0 + LinearCombination(Scalar::one(), new_s0)
               - LinearCombination(two, s0)
               - LinearCombination(Scalar::one(), s1)
               - LinearCombination(Scalar::one(), s2);
    cs.enforce_zero(lc0, label + "_mds0");

    LinearCombination lc1;
    lc1 = lc1 + LinearCombination(Scalar::one(), new_s1)
               - LinearCombination(Scalar::one(), s0)
               - LinearCombination(two, s1)
               - LinearCombination(Scalar::one(), s2);
    cs.enforce_zero(lc1, label + "_mds1");

    LinearCombination lc2;
    lc2 = lc2 + LinearCombination(Scalar::one(), new_s2)
               - LinearCombination(Scalar::one(), s0)
               - LinearCombination(Scalar::one(), s1)
               - LinearCombination(two, s2);
    cs.enforce_zero(lc2, label + "_mds2");

    s0 = new_s0;
    s1 = new_s1;
    s2 = new_s2;
}

// Add round constants in-circuit.
// Cost: 0 multiplication constraints.
// The constant is absorbed into the linear combination by allocating a new var.
static void add_rc_gadget(R1CS& cs,
                           Variable& s, const Scalar& s_val,
                           const Scalar& rc, Scalar& new_val,
                           const std::string& label) {
    new_val = s_val + rc;
    Variable new_s = cs.alloc(new_val);
    // Constrain: new_s = s + rc
    LinearCombination lc;
    lc = lc + LinearCombination(Scalar::one(), new_s)
             - LinearCombination(Scalar::one(), s)
             - LinearCombination::constant(rc);
    cs.enforce_zero(lc, label + "_arc");
    s = new_s;
}

// ---------------------------------------------------------------------------
// R1CS gadget: full Poseidon-2 permutation
// ---------------------------------------------------------------------------

Variable poseidon2_gadget(R1CS& cs, Variable a, Variable b,
                           const std::string& label) {
    const PoseidonConstants& c = get_constants();

    // Initialize state values for witness tracking
    Scalar val0 = Scalar::zero();           // capacity
    Scalar val1 = cs.get_value(a);          // rate[0] = a
    Scalar val2 = cs.get_value(b);          // rate[1] = b

    // Allocate initial capacity element (constrained to 0)
    Variable s0 = gadgets::constant(cs, Scalar::zero(), label + "_cap");
    Variable s1 = a;
    Variable s2 = b;

    // ---- First Rf/2 = 4 full rounds ----
    for (size_t r = 0; r < kFullRounds / 2; ++r) {
        const std::string rlabel = label + "_fr" + std::to_string(r);

        // Add round constants
        Scalar nv0, nv1, nv2;
        add_rc_gadget(cs, s0, val0, c.rc[r][0], nv0, rlabel + "_s0");
        add_rc_gadget(cs, s1, val1, c.rc[r][1], nv1, rlabel + "_s1");
        add_rc_gadget(cs, s2, val2, c.rc[r][2], nv2, rlabel + "_s2");
        val0 = nv0; val1 = nv1; val2 = nv2;

        // Apply S-Box to all elements (3 constraints each = 9 total per full round)
        s0 = sbox5_gadget(cs, s0, rlabel + "_sbox0"); val0 = sbox5_native(val0);
        s1 = sbox5_gadget(cs, s1, rlabel + "_sbox1"); val1 = sbox5_native(val1);
        s2 = sbox5_gadget(cs, s2, rlabel + "_sbox2"); val2 = sbox5_native(val2);

        // Apply MDS matrix
        mds_gadget(cs, s0, s1, s2, val0, val1, val2, rlabel);
        const Scalar two(uint64_t(2));
        const Scalar old0 = val0, old1 = val1, old2 = val2;
        val0 = old0 * two + old1 + old2;
        val1 = old0 + old1 * two + old2;
        val2 = old0 + old1 + old2 * two;
    }

    // ---- Rp = 56 partial rounds ----
    const size_t part_start = kFullRounds / 2;
    for (size_t r = 0; r < kPartRounds; ++r) {
        const size_t abs_r = part_start + r;
        const std::string rlabel = label + "_pr" + std::to_string(r);

        // Add round constants to all elements
        Scalar nv0, nv1, nv2;
        add_rc_gadget(cs, s0, val0, c.rc[abs_r][0], nv0, rlabel + "_s0");
        add_rc_gadget(cs, s1, val1, c.rc[abs_r][1], nv1, rlabel + "_s1");
        add_rc_gadget(cs, s2, val2, c.rc[abs_r][2], nv2, rlabel + "_s2");
        val0 = nv0; val1 = nv1; val2 = nv2;

        // Apply S-Box only to element [0] (3 constraints per partial round)
        s0 = sbox5_gadget(cs, s0, rlabel + "_sbox0"); val0 = sbox5_native(val0);

        // Apply MDS matrix
        mds_gadget(cs, s0, s1, s2, val0, val1, val2, rlabel);
        const Scalar two(uint64_t(2));
        const Scalar old0 = val0, old1 = val1, old2 = val2;
        val0 = old0 * two + old1 + old2;
        val1 = old0 + old1 * two + old2;
        val2 = old0 + old1 + old2 * two;
    }

    // ---- Last Rf/2 = 4 full rounds ----
    const size_t full2_start = part_start + kPartRounds;
    for (size_t r = 0; r < kFullRounds / 2; ++r) {
        const size_t abs_r = full2_start + r;
        const std::string rlabel = label + "_fr2_" + std::to_string(r);

        // Add round constants
        Scalar nv0, nv1, nv2;
        add_rc_gadget(cs, s0, val0, c.rc[abs_r][0], nv0, rlabel + "_s0");
        add_rc_gadget(cs, s1, val1, c.rc[abs_r][1], nv1, rlabel + "_s1");
        add_rc_gadget(cs, s2, val2, c.rc[abs_r][2], nv2, rlabel + "_s2");
        val0 = nv0; val1 = nv1; val2 = nv2;

        // Apply S-Box to all elements
        s0 = sbox5_gadget(cs, s0, rlabel + "_sbox0"); val0 = sbox5_native(val0);
        s1 = sbox5_gadget(cs, s1, rlabel + "_sbox1"); val1 = sbox5_native(val1);
        s2 = sbox5_gadget(cs, s2, rlabel + "_sbox2"); val2 = sbox5_native(val2);

        // Apply MDS matrix
        mds_gadget(cs, s0, s1, s2, val0, val1, val2, rlabel);
        const Scalar two(uint64_t(2));
        const Scalar old0 = val0, old1 = val1, old2 = val2;
        val0 = old0 * two + old1 + old2;
        val1 = old0 + old1 * two + old2;
        val2 = old0 + old1 + old2 * two;
    }

    // Output: s1 (first rate element), which holds val1 after all rounds
    // This is already correctly computed; verify witness consistency.
    assert(cs.get_value(s1) == val1);
    return s1;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
